/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2012 Yoran Heling

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
#include "fl_save.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <bzlib.h>
#include <zlib.h>


// This isn't a strict maximum, may be exceeded by a single <File> or <Directory> entry.
#define BUFSIZE (64*1024 - 1024)
// Minimum output buffer size to give to zlib's deflate() function.
#define ZLIBBUFSIZE (16*1024)


// Output configurations
#define FO_FU 0 // Write to file (uncompressed)
#define FO_FB 1 // Write to file (bzip2)
#define FO_MU 2 // Write to memory (uncompressed)
#define FO_MZ 3 // Write to memory (zlib)


typedef struct ctx_t {
  int conf;         // FO_*
  int size;
  GString *buf;     // Write buffer (final in F0_MU, temporary otherwise)
  GString *dest;    // F0_MZ - Destination buffer
  z_stream *zlib;   // F0_MZ
  BZFILE *fh_bz;    // F0_FB
  FILE *fh_f;       // F0_F*
  const char *file; // F0_F* - Filename (ownership is of the caller)
  char *tmpfile;    // F0_F* - Temp filename (ownership is ours)
  GError *err;
} ctx_t;


// Flushes the write buffer to the underlying bzip2/zlib/file object (if any).
static int doflush(ctx_t *x, gboolean force) {
  switch(x->conf) {
  case FO_FB: {
    int bzerr;
    BZ2_bzWrite(&bzerr, x->fh_bz, x->buf->str, x->buf->len);
    if(bzerr != BZ_OK) {
      g_set_error(&x->err, 1, 0, "Write error: %s", g_strerror(errno));
      return -1;
    }
    x->size += x->buf->len;
    x->buf->len = 0;
    break;
  }

  case FO_FU: {
    int r = fwrite(x->buf->str, 1, x->buf->len, x->fh_f);
    if(r != x->buf->len) {
      g_set_error(&x->err, 1, 0, "Write error: %s", g_strerror(errno));
      return -1;
    }
    x->size += x->buf->len;
    x->buf->len = 0;
    break;
  }

  case FO_MZ: {
    int r;
    x->zlib->next_in = (Bytef *)x->buf->str;
    x->zlib->avail_in = x->buf->len;
    do {
      if(x->zlib->avail_out < ZLIBBUFSIZE) {
        g_string_set_size(x->dest, x->dest->len+ZLIBBUFSIZE);
        x->dest->len -= ZLIBBUFSIZE;
        x->zlib->avail_out += ZLIBBUFSIZE;
        x->zlib->next_out = (Bytef *)(x->dest->str + x->zlib->total_out);
      }
      r = deflate(x->zlib, force ? Z_FINISH : Z_NO_FLUSH);
      x->dest->len = x->zlib->total_out;
    } while(x->buf->len > 0 && r == Z_OK);
    if(force ? (r != Z_STREAM_END) : (r != Z_OK && r != Z_BUF_ERROR)) {
      g_set_error(&x->err, 1, 0, "Zlib compression error (%d)", r);
      return -1;
    }
    int read = ((char *)x->zlib->next_in)-x->buf->str;
    x->size += read;
    g_string_erase(x->buf, 0, read);
    break;
  }

  case FO_MU:
    x->size = x->buf->len;
    // Nothing to do here, x->buf is already our destiniation.
    break;
  }

  return 0;
}


// Append a single character
#define ac(c) g_string_append_c(x->buf, c)

// Append a string
#define as(s) g_string_append(x->buf, s)

// Append an unsigned 64-bit integer
#define a64(i) g_string_append_printf(x->buf, "%"G_GUINT64_FORMAT, i)


// XML-escape and write a string literal
static void al(ctx_t *x, const char *str) {
  while(*str) {
    switch(*str) {
    case '&': as("&amp;"); break;
    case '>': as("&gt;"); break;
    case '<': as("&lt;"); break;
    case '"': as("&quot;"); break;
    default: ac(*str);
    }
    str++;
  }
}


// Recursively write the child nodes of an fl_list item.
static int af(ctx_t *x, fl_list_t *fl, int level) {
  int i;
  for(i=0; i<fl->sub->len; i++) {
    fl_list_t *cur = g_ptr_array_index(fl->sub, i);

    if(cur->isfile && cur->hastth) {
      char tth[40] = {};
      base32_encode(cur->tth, tth);
      as("<File Name=\"");
      al(x, cur->name);
      as("\" Size=\"");
      a64(cur->size);
      as("\" TTH=\"");
      as(tth); // No need to escape this, it's base32
      as("\"/>\n");
    }

    if(!cur->isfile) {
      as("<Directory Name=\"");
      al(x, cur->name);
      ac('"');
      if(level < 1 && !fl_list_isempty(cur))
        as(" Incomplete=\"1\"");

      if(level > 0) {
        as(">\n");
        if(af(x, cur, level-1))
          return 0;
        as("</Directory>\n");
      } else
        as("/>\n");
    }

    if(x->buf->len >= BUFSIZE && doflush(x, FALSE))
      return -1;
  }
  return 0;
}


// Write the top-level XML
static int at(ctx_t *x, fl_list_t *fl, const char *cid, int level) {
  as("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  as("<FileListing Version=\"1\" Generator=\"");
  al(x, PACKAGE_STRING);
  as("\" CID=\"");
  as(cid); // No need to escape this, it's base32
  as("\" Base=\"");
  if(fl) {
    char *path = fl_list_path(fl);
    al(x, path);
    // Make sure the base path always ends with a slash, some clients will fail otherwise.
    if(path[strlen(path)-1] != '/')
      ac('/');
    g_free(path);
  } else
    ac('/');
  as("\">\n");

  // all <Directory ..> elements
  if(fl && fl->sub)
    if(af(x, fl, level-1))
      return -1;

  as("</FileListing>\n");
  return 0;
}


static int ctx_open(ctx_t *x, int conf, const char *file, GString *buf) {
  memset(x, 0, sizeof(ctx_t));
  x->file = file;
  x->conf = conf;

  // Set buf/dest
  if(x->conf == FO_MU)
    x->buf = buf;
  else
    x->buf = g_string_new("");
  if(x->conf == FO_MZ)
    x->dest = buf;

  // zlib compressor
  if(x->conf == FO_MZ) {
    x->zlib = g_slice_new0(z_stream);
    x->zlib->zalloc = Z_NULL;
    x->zlib->zfree = Z_NULL;
    x->zlib->opaque = NULL;
    int r = deflateInit(x->zlib, Z_DEFAULT_COMPRESSION);
    if(r != Z_OK) {
      g_set_error(&x->err, 1, 0, "Unable to initialize zlib compression (%d: %s)", r, x->zlib->msg);
      return -1;
    }
  }

  // open file
  if(x->conf == FO_FB || x->conf == FO_FU) {
    x->tmpfile = g_strdup_printf("%s.tmp-%d", file, rand());
    x->fh_f = fopen(x->tmpfile, "w");
    if(!x->fh_f) {
      g_set_error_literal(&x->err, 1, 0, g_strerror(errno));
      return -1;
    }
  }

  // bzip2 compressor
  if(x->conf == FO_FB) {
    int bzerr;
    x->fh_bz = BZ2_bzWriteOpen(&bzerr, x->fh_f, 7, 0, 0);
    if(bzerr != BZ_OK) {
      g_set_error(&x->err, 1, 0, "Unable to create bzip2 file (%d): %s", bzerr, g_strerror(errno));
      return -1;
    }
  }

  return 0;
}


// Flushes the buffer, closes/renames the file and frees some memory. Does not
// free x->file (not our property) and x->err (still used).
static void ctx_close(ctx_t *x) {
  if(!x->err)
    doflush(x, TRUE);

  if(x->conf != FO_MU && x->buf)
    g_string_free(x->buf, TRUE);

  if(x->conf == FO_MZ && x->zlib) {
    deflateEnd(x->zlib);
    g_slice_free(z_stream, x->zlib);
  }

  if(x->conf == FO_FB && x->fh_bz) {
    int bzerr;
    BZ2_bzWriteClose(&bzerr, x->fh_bz, 0, NULL, NULL);
    if(bzerr != BZ_OK && !x->err)
      g_set_error(&x->err, 1, 0, "Error closing bzip2 stream (%d): %s", bzerr, g_strerror(errno));
  }

  if(x->conf == FO_FB || x->conf == FO_FU) {
    if(x->fh_f && fclose(x->fh_f) && !x->err)
      g_set_error(&x->err, 1, 0, "Error closing file: %s", g_strerror(errno));

    if(!x->err && rename(x->tmpfile, x->file) < 0)
      g_set_error(&x->err, 1, 0, "Error moving file: %s", g_strerror(errno));

    if(x->tmpfile && x->err)
      unlink(x->tmpfile);
    g_free(x->tmpfile);
  }
}


// Serialize a file list to a string. Config is chosen from the arguments:
// FU: buf == NULL, file doesn't end with .bz2
// FB: buf == NULL, file ends with .bz2
// MU: buf != NULL, !zlib
// MZ: buf == NULL, zlib
// Returns the uncompressed size of the list or 0 on error.
int fl_save(fl_list_t *fl, const char *cid, int level, gboolean zlib, GString *buf, const char *file, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  ctx_t x;
  int conf = buf && zlib ? FO_MZ : buf ? FO_MU :
    strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0 ? FO_FB : FO_FU;
  if(ctx_open(&x, conf, file, buf) == 0)
    at(&x, fl, cid, level);
  ctx_close(&x);
  if(x.err)
    g_propagate_error(err, x.err);

  return x.err ? 0 : x.size;
}

