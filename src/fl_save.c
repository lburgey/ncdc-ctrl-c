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


#define BUFSIZE (32*1024)


struct ctx {
  GString *buf;     // we're always writing to a (temporary) buffer
  BZFILE *fh_bz;    // if BZ2 compression is enabled (implies fh_h!=NULL)
  FILE *fh_f;       // if we're writing to a file
  const char *file; // Filename
  char *tmpfile;    // Temp filename
  gboolean freebuf; // Whether we should free the buffer on close (otherwise it's passed back to the application)
  GError *err;
};


// Flushes the write buffer to the underlying bzip2/file object (if any).
static int doflush(struct ctx *x) {
  if(x->fh_bz) {
    int bzerr;
    BZ2_bzWrite(&bzerr, x->fh_bz, x->buf->str, x->buf->len);
    if(bzerr != BZ_OK) {
      g_set_error(&x->err, 1, 0, "Write error: %s", g_strerror(errno));
      return -1;
    }
    x->buf->len = 0;

  } else if(x->fh_f) {
    int r = fwrite(x->buf->str, 1, x->buf->len, x->fh_f);
    if(r != x->buf->len) {
      g_set_error(&x->err, 1, 0, "Write error: %s", g_strerror(errno));
      return -1;
    }
    x->buf->len = 0;
  }

  return 0;
}


#define checkflush if(x->buf->len >= BUFSIZE && doflush(x)) return -1

// Append a single character, returns the calling function on error.
#define ac(c) do {\
    g_string_append_c(x->buf, c);\
    checkflush;\
  } while(0)

// Append a string
#define as(s) do {\
    g_string_append(x->buf, s);\
    checkflush;\
  } while(0)

// Append an unsigned 64-bit integer
#define a64(i) do {\
    g_string_append_printf(x->buf, "%"G_GUINT64_FORMAT, i);\
    checkflush;\
  } while(0)


// XML-escape and write a string literal
static int al(struct ctx *x, const char *str) {
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
  return 0;
}


// Recursively write the child nodes of an fl_list item.
static int af(struct ctx *x, struct fl_list *fl, int level) {
  int i;
  for(i=0; i<fl->sub->len; i++) {
    struct fl_list *cur = g_ptr_array_index(fl->sub, i);

    if(cur->isfile && cur->hastth) {
      char tth[40] = {};
      base32_encode(cur->tth, tth);
      as("<File Name=\"");
      if(al(x, cur->name))
        return -1;
      as("\" Size=\"");
      a64(cur->size);
      as("\" TTH=\"");
      as(tth); // No need to escape this, it's base32
      as("\"/>\n");
    }

    if(!cur->isfile) {
      as("<Directory Name=\"");
      if(al(x, cur->name))
        return -1;
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
  }
  return 0;
}


// Write the top-level XML
static int at(struct ctx *x, struct fl_list *fl, int level) {
  as("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  as("<FileListing Version=\"1\" Generator=\"");
  if(al(x, PACKAGE_STRING))
    return -1;
  as("\" CID=\"");
  as(var_get(0, VAR_cid)); // No need to escape this, it's base32
  as("\" Base=\"");
  if(fl) {
    char *path = fl_list_path(fl);
    if(al(x, path)) {
      g_free(path);
      return -1;
    }
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


static int ctx_open(struct ctx *x, const char *file, GString *buf) {
  memset(x, 0, sizeof(struct ctx));
  x->buf = buf;
  x->file = file;

  // Always make sure we have a buffer
  if(!x->buf) {
    x->buf = g_string_new("");
    x->freebuf = TRUE;
  }

  gboolean isbz2 = FALSE;
  if(file) {
    isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;
    x->tmpfile = g_strdup_printf("%s.tmp-%d", file, rand());
  }

  // open temp file
  if(x->tmpfile) {
    x->fh_f = fopen(x->tmpfile, "w");
    if(!x->fh_f) {
      g_set_error_literal(&x->err, 1, 0, g_strerror(errno));
      return -1;
    }
  }

  // open compressor (if needed)
  if(isbz2 && x->fh_f) {
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
static int ctx_close(struct ctx *x) {
  if(!x->err)
    doflush(x);

  if(x->freebuf)
    g_string_free(x->buf, TRUE);

  if(x->fh_bz) {
    int bzerr;
    BZ2_bzWriteClose(&bzerr, x->fh_bz, 0, NULL, NULL);
    if(bzerr != BZ_OK && !x->err)
      g_set_error(&x->err, 1, 0, "Error closing bzip2 stream (%d): %s", bzerr, g_strerror(errno));
  }

  if(x->fh_f && fclose(x->fh_f) && !x->err)
    g_set_error(&x->err, 1, 0, "Error closing file: %s", g_strerror(errno));

  if(x->tmpfile && !x->err && rename(x->tmpfile, x->file) < 0)
    g_set_error(&x->err, 1, 0, "Error moving file: %s", g_strerror(errno));

  if(x->tmpfile && x->err)
    unlink(x->tmpfile);
  g_free(x->tmpfile);
  return x->err ? -1 : 0;
}


gboolean fl_save(struct fl_list *fl, const char *file, GString *buf, int level, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  struct ctx x;
  if(!ctx_open(&x, file, buf))
    at(&x, fl, level);
  ctx_close(&x);
  if(x.err)
    g_propagate_error(err, x.err);

  return x.err ? FALSE : TRUE;
}

