/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2013 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#include "ncdc.h"
#include "dlfile.h"


/* Terminology
 *
 *   chunk: Smallest "addressable" byte range within a file, see DLFILE_CHUNKSIZE
 *   block: Smallest "verifiable" byte range within a file (i.e. what a TTH
 *          leaf represents, see DL_MINBLOCKSIZE). Always a multiple of the
 *          chunk size.
 *  thread: A range of chunks that haven't been downloaded yet.
 * segment: A range of chunks that is requested for downloading in a single
 *          CGET/$ADCGET. Not necessarily aligned to or a multiple of the block
 *          size. Segments are allocated at the start of a thread.
 */

/* TODO: The following fields may be accessed from multiple threads
 * simultaneously and should be protected by a mutex:
 *   dl_t.{have,bitmap}
 *   dlfile_thread_t.{allocated,avail,chunk}
 *
 * Some other fields are shared, too, but those are never modified while a
 * downloading thread is active and thus do not need synchronisation.
 * These include dl_t.{size,islist,hash_block,incfd} and possibly more.
 */


#if INTERFACE

/* Size of a chunk within the downloaded file. This determines the granularity
 * of the file data that is remembered across restarts, the size of the chunk
 * bitmap and the minimum download request.
 * Must be a power of two and less than or equal to DL_MINBLOCKSIZE */
#define DLFILE_CHUNKSIZE (128*1024)


/* For file lists (dl->islist), only len and chunk are used. The other fields
 * aren't used because no length of TTH info is known before downloading. */
struct dlfile_thread_t {
  dl_t *dl;
  tth_ctx_t hash_tth;
  guint32 allocated; /* Number of remaining chunks allocated to this thread (including current) */
  guint32 avail;     /* Number of undownloaded chunks in and after this thread (including current & allocated) */
  guint32 chunk;     /* Current chunk number */
  guint32 len;       /* Number of bytes downloaded into this chunk */
};

#endif


static guint32 dlfile_chunks(guint64 size) {
  return (size+DLFILE_CHUNKSIZE-1)/DLFILE_CHUNKSIZE;
}


static gboolean dlfile_load_bitmap(dl_t *dl, int fd) {
  guint32 chunks = dlfile_chunks(dl->size);
  guint8 *bitmap = bita_new(chunks);
  guint8 *dest = bitmap;

  off_t off = dl->size;
  size_t left = bita_size(chunks);
  while(left > 0) {
    int r = pread(fd, dest, left, off);
    if(r <= 0) {
      if(r)
        g_warning("Error reading bitmap for `%s': %s.", dl->dest, g_strerror(errno));
      else
        g_warning("Unexpected EOF while reading bitmap for `%s'.", dl->dest);
      bita_free(bitmap);
      return FALSE;
    }
    left -= r;
    off += r;
    dest += r;
  }

  bita_free(dl->bitmap);
  dl->bitmap = bitmap;
  return TRUE;
}


/* TODO: Set dl error on failure. */
static void dlfile_save_bitmap(dl_t *dl, int fd) {
  guint8 *buf = dl->bitmap;
  off_t off = dl->size;
  size_t left = bita_size(dlfile_chunks(dl->size));
  while(left > 0) {
    int r = pwrite(fd, buf, left, off);
    if(r < 0) {
      g_warning("Error writing bitmap for `%s': %s.", dl->dest, g_strerror(errno));
      return;
    }
    left -= r;
    off += r;
    buf += r;
  }
}


static dlfile_thread_t *dlfile_load_block(dl_t *dl, int fd, guint32 chunk, guint32 chunksinblock, guint32 *reset) {
  dlfile_thread_t *t = g_slice_new0(dlfile_thread_t);
  t->chunk = chunk;
  t->avail = chunksinblock;
  tth_init(&t->hash_tth);

  char *bufp = malloc(DLFILE_CHUNKSIZE);

  *reset = chunksinblock;
  while(bita_get(dl->bitmap, t->chunk)) {
    char *buf = bufp;
    off_t off = (guint64)t->chunk * DLFILE_CHUNKSIZE;
    size_t left = DLFILE_CHUNKSIZE;
    while(left > 0) {
      int r = pread(fd, buf, left, off);
      if(r <= 0) {
        /* TODO: Handle error */
        return FALSE;
      }
      off -= r;
      left -= r;
      buf += r;
    }
    tth_update(&t->hash_tth, buf, DLFILE_CHUNKSIZE);
    t->chunk++;
    t->avail--;
    dl->have += DLFILE_CHUNKSIZE;
    (*reset)--;
  }

  free(bufp);
  dl->threads = g_slist_prepend(dl->threads, t);
  return t;
}


/* Go over the bitmap and create a thread for each range of undownloaded
 * chunks. Threads are created in a TTHL-block-aligned fashion to ensure that
 * the downloading progress can continue from the threads while keeping the
 * integrity checks. */
static void dlfile_load_threads(dl_t *dl, int fd) {
  guint32 chunknum = dlfile_chunks(dl->size);
  guint32 chunksperblock = dl->hash_block / DLFILE_CHUNKSIZE;
  gboolean needsave = FALSE;
  dlfile_thread_t *t = NULL;

  guint32 i,j;
  for(i=0; i<chunknum; i+=chunksperblock) {
    guint32 reset = 0;
    guint32 chunksinblock = MIN(chunksperblock, dlfile_chunks(dl->size) - i);

    for(j=i; j<i+chunksinblock; j++)
      if(!bita_get(dl->bitmap, j))
        break;
    gboolean hasfullblock = j == i+chunksinblock;

    if(t && !bita_get(dl->bitmap, i)) {
      t->avail += chunksinblock;
      reset = chunksinblock;
    } else if(hasfullblock) {
      t = NULL;
      dl->have += dl->hash_block;
    } else
      t = dlfile_load_block(dl, fd, i, chunksinblock, &reset);

    for(j=i+(chunksinblock-reset); j<i+chunksinblock; j++)
      if(bita_get(dl->bitmap, j)) {
        bita_reset(dl->bitmap, j);
        needsave = TRUE;
      }
  }

  if(needsave)
    dlfile_save_bitmap(dl, fd);
}


/* TODO: This function will delete the existing incoming file when loading it
 * failed. This is not necessarily a good idea. A better solution is to use
 * dl_queue_seterr() and allow the user to try again.
 * TODO: Load pre-bitmap incoming files and convert them to the new format? */
void dlfile_load(dl_t *dl) {
  dl->have = 0;
  int fd = open(dl->inc, O_RDONLY);
  if(fd < 0) {
    if(errno != ENOENT) {
      g_warning("Unable to open incoming file for `%s' (%s), trying to delete it.", dl->dest, g_strerror(errno));
      unlink(dl->inc);
    }
    return;
  }

  /* If the above didn't fail, then we should already have TTHL data.
   * Otherwise, close and delete whatever we have. */
  if(!dl->hastthl) {
    g_warning("No TTHL data for `%s', deleting partially downloaded data.", dl->dest);
    close(fd);
    unlink(dl->inc);
    return;
  }

  if(!dlfile_load_bitmap(dl, fd)) {
    close(fd);
    unlink(dl->inc);
    return;
  }

  dlfile_load_threads(dl, fd);
  close(fd);
}


/* Called from dl.c when a dl item is being deleted, either from
 * dlfile_finished() or when the item is removed from the UI. */
void dlfile_rm(dl_t *dl) {
  g_return_if_fail(!dl->active_threads);

  if(dl->incfd > 0)
    g_warn_if_fail(close(dl->incfd) == 0);

  if(dl->inc)
    unlink(dl->inc);

  GSList *l;
  for(l=dl->threads; l; l=l->next)
    g_slice_free(dlfile_thread_t, l->data);
  g_slist_free(dl->threads);
  g_free(dl->bitmap);
}


/* XXX: This function may block in the main thread for a while. Perhaps do it in a threadpool? */
static void dlfile_finished(dl_t *dl) {
  /* Remove bitmap from the file */
  /* TODO: Error handling */
  if(!dl->islist)
    ftruncate(dl->incfd, dl->size);
  close(dl->incfd);
  dl->incfd = 0;

  char *fdest = g_filename_from_utf8(dl->dest, -1, NULL, NULL, NULL);
  if(!fdest)
    fdest = g_strdup(dl->dest);

  /* Create destination directory, if it does not exist yet. */
  char *parent = g_path_get_dirname(fdest);
  g_mkdir_with_parents(parent, 0755);
  /* TODO: Error handling */
  g_free(parent);

  /* Prevent overwiting other files by appending a prefix to the destination if
   * it already exists. It is assumed that fn + any dupe-prevention-extension
   * does not exceed NAME_MAX. (Not that checking against NAME_MAX is really
   * reliable - some filesystems have an even more strict limit) */
  int num = 1;
  char *dest = g_strdup(fdest);
  while(!dl->islist && g_file_test(dest, G_FILE_TEST_EXISTS)) {
    g_free(dest);
    dest = g_strdup_printf("%s.%d", fdest, num++);
  }

  GError *err = NULL;
  file_move(dl->inc, dest, dl->islist, &err);
  if(err) {
    /* TODO: Error handling. */
    g_error_free(err);
  }

  g_free(dest);
  g_free(fdest);

  dl_finished(dl);
}


/* Create the inc file and initialize the necessary structs to prepare for
 * handling downloaded data. */
static void dlfile_open(dl_t *dl) {
  /* TODO: Error handling */
  if(dl->incfd <= 0)
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT, 0666);

  /* Everything else has already been initialized if we have a thread */
  if(dl->threads)
    return;

  if(!dl->islist) {
    dl->bitmap = bita_new(dlfile_chunks(dl->size));
    dlfile_save_bitmap(dl, dl->incfd);
  }

  dlfile_thread_t *t = g_slice_new0(dlfile_thread_t);
  t->chunk = 0;
  t->allocated = 0;
  if(!dl->islist)
    t->avail = dlfile_chunks(dl->size);
  tth_init(&t->hash_tth);
  dl->threads = g_slist_prepend(dl->threads, t);
}


/* The 'speed' argument should be a pessimistic estimate of the peers' speed,
 * in bytes/s. I think this is best obtained from a 30 second average.
 * Returns the thread pointer. */
dlfile_thread_t *dlfile_getchunk(dl_t *dl, guint64 speed) {
  dlfile_thread_t *t = NULL;
  dlfile_open(dl);

  /* File lists should always be downloaded in a single GET request because
   * their contents may be modified between subsequent requests. */
  if(dl->islist) {
    t = dl->threads->data;
    t->chunk = 0;
    t->len = 0;
    dl->allbusy = TRUE;
    dl->active_threads++;
    return t;
  }

  /* Walk through the threads and look for:
   *      t = Thread with largest avail and with allocated = 0
   *   tsec = Thread with largest avail-allocated
   */
  gboolean havefreechunk = FALSE;
  dlfile_thread_t *tsec = NULL;
  GSList *l;
  for(l=dl->threads; l; l=l->next) {
    dlfile_thread_t *ti = l->data;
    if(!tsec || ti->avail-ti->allocated > tsec->avail-tsec->allocated)
      tsec = ti;
    if(!ti->allocated && (!t || ti->avail > t->avail))
      t = ti;
    if(tsec != ti && t != ti && ti->avail > ti->allocated)
      havefreechunk = TRUE;
  }
  g_return_val_if_fail(tsec, NULL);

  if(!t) {
    guint32 chunksinblock = dl->hash_block/DLFILE_CHUNKSIZE;
    guint32 chunk = ((tsec->chunk + tsec->allocated + (tsec->avail - tsec->allocated)/2) / chunksinblock) * chunksinblock;
    if(chunk < tsec->chunk + tsec->allocated)
      chunk = tsec->chunk + tsec->allocated;
    t = g_slice_new0(dlfile_thread_t);
    t->chunk = chunk;
    t->avail = tsec->avail - (chunk - tsec->chunk);
    tth_init(&t->hash_tth);

    tsec->avail -= t->avail;
    dl->threads = g_slist_prepend(dl->threads, t);
  } else if(t != tsec)
    havefreechunk = TRUE;

  /* Number of chunks to request as one segment. The size of a segment is
   * chosen to approximate a download time of ~5 min.
   * XXX: Make the minimum segment size configurable to allow users to disable
   * segmented downloading (still at least DLFILE_CHUNKSIZE). */
  guint32 chunks = MIN(G_MAXUINT32, 1 + ((speed * 300) / DLFILE_CHUNKSIZE));
  t->allocated = MIN(t->avail, chunks);
  dl->active_threads++;
  dl->allbusy = !(havefreechunk || t->avail > t->allocated);
  return t;
}


static gboolean dlfile_recv_check(dlfile_thread_t *t, int num, char *leaf) {
  /* We don't have TTHL data for small files, so check against the root hash
   * instead. */
  if(t->dl->size < t->dl->hash_block) {
    g_return_val_if_fail(num == 0, FALSE);
    return memcmp(leaf, t->dl->hash, 24) == 0 ? TRUE : FALSE;
  }
  /* Otherwise, check against the TTHL data in the database. */
  return db_dl_checkhash(t->dl->hash, num, leaf);
}


static gboolean dlfile_recv_write(dlfile_thread_t *t, const char *buf, int len) {
  off_t off = ((guint64)t->chunk * DLFILE_CHUNKSIZE) + t->len;
  off_t offi = off;
  size_t rem = len;
  const char *bufi = buf;
  while(rem > 0) {
    ssize_t r = pwrite(t->dl->incfd, bufi, rem, offi);
    if(r <= 0)
      return FALSE; /* TODO: Error handling */
    offi += r;
    rem -= r;
    bufi += r;
  }
  fadv_oneshot(t->dl->incfd, off, len, VAR_FFC_DOWNLOAD);
  return TRUE;
}


/* Called when new data has been received from a downloading thread.  The data
 * is written to the file, the TTH calculation is updated and checked with the
 * DB, and the bitmap is updated.
 * This function may be called from another OS thread.
 * Returns TRUE to indicate success, FALSE on failure. */
gboolean dlfile_recv(void *vt, const char *buf, int len) {
  dlfile_thread_t *t = vt;
  dlfile_recv_write(t, buf, len);

  while(len > 0) {
    guint32 inchunk = MIN((guint32)len, DLFILE_CHUNKSIZE - t->len);
    t->len += inchunk;
    gboolean islast = ((guint64)t->chunk * DLFILE_CHUNKSIZE) + t->len == t->dl->size;

    tth_update(&t->hash_tth, buf, inchunk);
    buf += inchunk;
    len -= inchunk;

    if(!islast && t->len < DLFILE_CHUNKSIZE)
      continue;

    bita_set(t->dl->bitmap, t->chunk);
    t->chunk++;
    t->allocated--;
    t->avail--;
    t->len = 0;

    if(islast || t->chunk % (t->dl->hash_block / DLFILE_CHUNKSIZE) == 0) {
      char leaf[24];
      tth_final(&t->hash_tth, leaf);
      tth_init(&t->hash_tth);
      if(!dlfile_recv_check(t, t->chunk/(t->dl->hash_block / DLFILE_CHUNKSIZE), leaf))
        return FALSE; /* TODO: Error handling */
    }
  }

  /* TODO: Update t->dl->have */
  /* TODO: Flush bitmap if it has been changed (needs to be synchronized with other threads?) */
  return TRUE;
}


void dlfile_recv_done(dlfile_thread_t *t) {
  dl_t *dl = t->dl;
  dl->active_threads--;

  if(t->avail) {
    t->allocated = 0;
    dl->allbusy = FALSE;
  } else {
    dl->threads = g_slist_remove(dl->threads, t);
    g_slice_free(dlfile_thread_t, t);
  }

  /* TODO: If anything went wrong in dlfile_recv(), propagate the error to the dl_t object here. */

  /* File has been removed from the queue but the dl struct is still in memory
   * because this thread hadn't finished yet. Free it now. */
  if(!dl->active_threads && g_hash_table_lookup(dl_queue, dl->hash))
    dl_queue_rm(dl);
  else if(!dl->threads)
    dlfile_finished(dl);
}
