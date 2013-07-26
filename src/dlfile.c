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


#if INTERFACE

/* Size of a chunk within the downloaded file. This determines the granularity
 * of the file data that is remembered across restarts, the size of the chunk
 * bitmap and the minimum download request.
 * Must be a power of two and less than or equal to DL_MINBLOCKSIZE */
#define DLFILE_CHUNKSIZE (128*1024)


struct dlfile_thread_t {
  tth_ctx_t hash_tth;
  guint32 allocated; /* Number of remaining chunks allocated to this thread (including current) */
  guint32 chunk;     /* Current chunk number */
  guint32 len;       /* Number of bytes downloaded into this chunk buffer */
  char buf[DLFILE_CHUNKSIZE];
};

#endif


static int dlfile_chunks(guint64 size) {
  return (size+DLFILE_CHUNKSIZE-1)/DLFILE_CHUNKSIZE;
}


static gboolean dlfile_load_bitmap(dl_t *dl, int fd) {
  int chunks = dlfile_chunks(dl->size);
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


/* Load a TTHL block and, if necessary, create a new thread and/or reset any
 * further chunk flags. 'chunk' refers to the starting chunk.
 * Returns TRUE when the bitmap may have been modified. */
static gboolean dlfile_load_block(dl_t *dl, int fd, int chunk) {
  int i;
  int chunksinblock = MIN(dl->hash_block / DLFILE_CHUNKSIZE, dlfile_chunks(dl->size) - chunk);

  /* If the first chunk of this block isn't present, then just throw away the
   * entire block and don't create a thread. */
  if(!bita_get(dl->bitmap, chunk)) {
    for(i=0; i<chunksinblock; i++)
      bita_reset(dl->bitmap, chunk+i);
    return TRUE;
  }

  dlfile_thread_t *t = g_slice_new0(dlfile_thread_t);
  t->len = 0;
  t->chunk = chunk;
  t->allocated = chunksinblock;
  tth_init(&t->hash_tth);

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
    t->allocated--;
  }

  gboolean needsave = FALSE;
  for(i=t->chunk; i<chunk+chunksinblock; i++)
    if(bita_get(dl->bitmap, i)) {
      bita_reset(dl->bitmap, chunk+i);
      needsave = TRUE;
    }

  return needsave;
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

  /* Check for unfinished TTHL blocks and create threads to continue those */
  int chunknum = dlfile_chunks(dl->size);
  int chunksperblock = dl->hash_block / DLFILE_CHUNKSIZE;
  int thisblock = 0;
  int chunk = 0;
  int needsave = 0;
  while(chunk<chunknum) {
    if((chunk % chunksperblock) == 0)
      thisblock = bita_get(dl->bitmap, chunk);
    else if(!thisblock != !bita_get(dl->bitmap, chunk)) {
      needsave |= dlfile_load_block(dl, fd, (chunk / chunksperblock) * chunksperblock);
      chunk = chunksperblock + ((chunk / chunksperblock) * chunksperblock);
      continue;
    }
    chunk++;
  }

  if(needsave)
    dlfile_save_bitmap(dl, fd);

  close(fd);
}


/* The 'speed' argument should be a pessimistic estimate of the peers' speed,
 * in bytes/s. I think this is best obtained from a 30 second average.
 * Returns the thread number. */
int dlfile_getchunk(dl_t *dl, guint64 speed) {
  /* Poossible algorithm:
   * - Calculate a good segment size, something like size = speed * 300
   *   (i.e. takes ~5 min. to download the chunk)
   * - Look for an existing dlfile_thread_t object with allocated=0, and
   *   continue from that.
   * - Otherwise, create a new thread. The thread should start at the next
   *   unallocated tth-leaf-aligned chunk(?).
   */
}


/* Called when new data has been received from a downloading thread.  The data
 * is written to the file, the TTH calculation is updated and checked with the
 * DB, and the bitmap is updated.
 * This function may be called from another OS thread.
 * Returns TRUE to indicate success, FALSE on failure. */
gboolean dlfile_recv(dl_t *dl, int thread, const char *buf, int len) {
}

