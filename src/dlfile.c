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


#if INTERFACE

/* Size of a chunk within the downloaded file. This determines the granularity
 * of the file data that is remembered across restarts, the size of the chunk
 * bitmap and the minimum download request.
 * Must be a power of two and less than or equal to DL_MINBLOCKSIZE */
#define DLFILE_CHUNKSIZE (128*1024)


/* For file lists (dl->islist), only len and buf are used. The other fields
 * aren't used because no length of TTH info is known before downloading. */
struct dlfile_thread_t {
  dl_t *dl;
  tth_ctx_t hash_tth;
  guint32 allocated; /* Number of remaining chunks allocated to this thread (including current) */
  guint32 avail;     /* Number of undownloaded chunks in and after this thread (including current & allocated) */
  guint32 chunk;     /* Current chunk number */
  guint32 len;       /* Number of bytes downloaded into this chunk buffer */
  char buf[DLFILE_CHUNKSIZE];
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

  *reset = chunksinblock;
  while(bita_get(dl->bitmap, t->chunk)) {
    char *buf = t->buf;
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
    (*reset)--;
  }

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

    if(!t || bita_get(dl->bitmap, i))
      t = hasfullblock ? NULL : dlfile_load_block(dl, fd, i, chunksinblock, &reset);
    else {
      t->avail += chunksinblock;
      reset = chunksinblock;
    }

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


/* The 'speed' argument should be a pessimistic estimate of the peers' speed,
 * in bytes/s. I think this is best obtained from a 30 second average.
 * Returns the thread pointer. */
dlfile_thread_t *dlfile_getchunk(dl_t *dl, guint64 speed) {
  dlfile_thread_t *t = NULL;

  /* XXX: Create incfile and load dl->bitmap etc here? The code below assumes
   * everything has been initialized properly. */

  /* File lists should always be downloaded in a single GET request because
   * their contents may be modified between subsequent requests. */
  if(dl->islist) {
    if(dl->threads)
      t = dl->threads->data;
    else {
      t = g_slice_new0(dlfile_thread_t);
      dl->threads = g_slist_prepend(dl->threads, t);
    }
    t->len = 0;
    return t;
  }

  /* Number of chunks to request as one segment. The size of a segment is
   * chosen to approximate a download time of ~5 min.
   * XXX: Make the minimum segment size configurable to allow users to disable
   * segmented downloading (still at least DLFILE_CHUNKSIZE). */
  guint32 chunks = MIN(G_MAXUINT32, 1 + ((speed * 300) / DLFILE_CHUNKSIZE));

  /* Walk through the threads and look for:
   *      t = Thread with largest avail and with allocated = 0
   *   tsec = Thread with largest avail-allocated
   */
  dlfile_thread_t *tsec = NULL;
  GSList *l;
  for(l=dl->threads; l; l=l->next) {
    dlfile_thread_t *ti = l->data;
    if(!tsec || ti->avail-ti->allocated > tsec->avail-tsec->allocated)
      tsec = ti;
    if(!ti->allocated && (!t || ti->avail > t->avail))
      t = ti;
  }
  g_return_val_if_fail(tsec, NULL);

  /* XXX: Should creating a new thread be attempted if t->avail < chunks? */
  if(!t) {
    guint32 chunksinblock = dl->hash_block/DLFILE_CHUNKSIZE;
    guint32 chunk = ((tsec->chunk + tsec->allocated + (tsec->avail - tsec->allocated)/2) / chunksinblock) * chunksinblock;
    if(chunk < tsec->chunk + tsec->allocated)
      return NULL;
    t = g_slice_new0(dlfile_thread_t);
    t->chunk = chunk;
    t->avail = tsec->avail - (chunk - tsec->chunk);
    tth_init(&t->hash_tth);

    tsec->avail -= t->avail;
    dl->threads = g_slist_prepend(dl->threads, t);
  }

  t->allocated = MIN(t->avail, chunks);
  return t;
}


/* Called when new data has been received from a downloading thread.  The data
 * is written to the file, the TTH calculation is updated and checked with the
 * DB, and the bitmap is updated.
 * This function may be called from another OS thread.
 * Returns TRUE to indicate success, FALSE on failure. */
gboolean dlfile_recv(dlfile_thread_t *t, const char *buf, int len) {
  return TRUE;
}


void dlfile_recv_done(dlfile_thread_t *t) {
}
