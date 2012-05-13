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
#include "fl_load.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bzlib.h>


#define S_START    0 // waiting for <FileListing>
#define S_FLOPEN   1 // In a <FileListing ..>
#define S_DIROPEN  2 // In a <Directory ..>
#define S_INDIR    3 // In a <Directory>..</Directory> or <FileListing>..</FileListing>
#define S_FILEOPEN 4 // In a <File ..>
#define S_INFILE   5 // In a <File>..</File>
#define S_UNKNOWN  6 // In some tag we didn't recognize
#define S_END      7 // Received </FileListing>

typedef struct ctx_t {
  BZFILE *fh_bz;
  FILE   *fh_f;
  gboolean eof;

  gboolean local;
  int state;
  char *name;
  char filetth[24];
  gboolean filehastth;
  guint64 filesize;
  gboolean dirincomplete;
  fl_list_t *root;
  fl_list_t *cur;
  int unknown_level;
} ctx_t;


static int readcb(void *context, char *buf, int len, GError **err) {
  ctx_t *x = context;

  if(x->fh_bz) {
    if(x->eof)
      return 0;
    int bzerr;
    int r = BZ2_bzRead(&bzerr, x->fh_bz, buf, len);
    if(bzerr != BZ_OK && bzerr != BZ_STREAM_END) {
      g_set_error(err, 1, 0, "bzip2 decompression error (%d): %s", bzerr, g_strerror(errno));
      return -1;
    }
    if(bzerr == BZ_STREAM_END)
      x->eof = TRUE;
    return r;

  }

  int r = fread(buf, 1, len, x->fh_f);
  if(r < 0 && feof(x->fh_f))
    r = 0;
  if(r < 0)
    g_set_error(err, 1, 0, "Read error: %s", g_strerror(errno));
  return r;
}


static int entitycb(void *context, int type, const char *arg1, const char *arg2, GError **err) {
  ctx_t *x = context;
  //printf("%d,%d: %s, %s\n", x->state, type, arg1, arg2);
  switch(x->state) {

  // The first token must always be a <FileListing>
  case S_START:
    if(type == XMLT_OPEN && strcmp(arg1, "FileListing") == 0) {
      x->state = S_FLOPEN;
      return 0;
    }
    break;

  // Any attributes in a <FileListing> are currently ignored.
  case S_FLOPEN:
    if(type == XMLT_ATTR)
      return 0;
    if(type == XMLT_ATTDONE) {
      x->state = S_INDIR;
      return 0;
    }
    break;

  // Handling the attributes of a Directory element.
  case S_DIROPEN:
    if(type == XMLT_ATTR && strcmp(arg1, "Name") == 0 && !x->name) {
      x->name = g_utf8_validate(arg2, -1, NULL) ? g_strdup(arg2) : str_convert("UTF-8", "UTF-8", arg2);
      return 0;
    }
    if(type == XMLT_ATTDONE) {
      if(!x->name) {
        g_set_error(err, 1, 0, "Missing Name attribute in Directory element");
        return -1;
      }
      // Create the directory entry
      fl_list_t *new = fl_list_create(x->name, FALSE);
      new->isfile = FALSE;
      new->sub = g_ptr_array_new_with_free_func(fl_list_free);
      fl_list_add(x->cur, new, -1);
      x->cur = new;

      g_free(x->name);
      x->name = NULL;
      x->state = S_INDIR;
      return 0;
    }
    // Ignore unknown or duplicate attributes.
    if(type == XMLT_ATTR)
      return 0;
    break;

  // In a directory listing.
  case S_INDIR:
    if(type == XMLT_OPEN && strcmp(arg1, "Directory") == 0) {
      x->state = S_DIROPEN;
      return 0;
    }
    if(type == XMLT_OPEN && strcmp(arg1, "File") == 0) {
      x->state = S_FILEOPEN;
      return 0;
    }
    if(type == XMLT_OPEN) {
      x->state = S_UNKNOWN;
      x->unknown_level = 1;
      return 0;
    }
    if(type == XMLT_CLOSE) {
      char *expect = x->root == x->cur ? "FileListing" : "Directory";
      if(arg1 && strcmp(arg1, expect) != 0) {
        g_set_error(err, 1, 0, "Invalid close tag, expected </%s> but got </%s>", expect, arg1);
        return -1;
      }
      fl_list_sort(x->cur);
      if(x->cur == x->root)
        x->state = S_END;
      else
        x->cur = x->cur->parent;
      return 0;
    }
    break;

  // Handling the attributes of a File element. (If there are multiple
  // attributes with the same name, only the first is used.)
  case S_FILEOPEN:
    if(type == XMLT_ATTR && strcmp(arg1, "Name") == 0 && !x->name) {
      x->name = g_utf8_validate(arg2, -1, NULL) ? g_strdup(arg2) : str_convert("UTF-8", "UTF-8", arg2);
      return 0;
    }
    if(type == XMLT_ATTR && strcmp(arg1, "TTH") == 0 && !x->filehastth) {
      if(!istth(arg2)) {
        g_set_error(err, 1, 0, "Invalid TTH");
        return -1;
      }
      base32_decode(arg2, x->filetth);
      x->filehastth = TRUE;
      return 0;
    }
    if(type == XMLT_ATTR && strcmp(arg1, "Size") == 0 && x->filesize == G_MAXUINT64) {
      char *end = NULL;
      x->filesize = g_ascii_strtoull(arg2, &end, 10);
      if(!end || *end) {
        g_set_error(err, 1, 0, "Invalid file size");
        return -1;
      }
      return 0;
    }
    if(type == XMLT_ATTDONE) {
      if(!x->name || !x->filehastth || x->filesize == G_MAXUINT64) {
        g_set_error(err, 1, 0, "Missing %s attribute in File element",
          !x->name ? "Name" : !x->filehastth ? "TTH" : "Size");
        return -1;
      }
      // Create the file entry
      fl_list_t *new = fl_list_create(x->name, x->local);
      new->isfile = TRUE;
      new->size = x->filesize;
      new->hastth = TRUE;
      memcpy(new->tth, x->filetth, 24);
      fl_list_add(x->cur, new, -1);

      x->filehastth = FALSE;
      x->filesize = G_MAXUINT64;
      g_free(x->name);
      x->name = NULL;
      x->state = S_INFILE;
      return 0;
    }
    // Ignore unknown or duplicate attributes.
    if(type == XMLT_ATTR)
      return 0;
    break;

  // In a File element. Nothing is allowed here exept a close of the File
  // element. (Really?)
  case S_INFILE:
    if(type == XMLT_CLOSE && (!arg1 || strcmp(arg1, "File") == 0)) {
      x->state = S_INDIR;
      return 0;
    }
    break;

  // No idea in what kind of tag we are, just count start/end tags so we can
  // continue parsing when we're out of this unknown tag.
  case S_UNKNOWN:
    if(type == XMLT_OPEN)
      x->unknown_level++;
    else if(type == XMLT_CLOSE && !--x->unknown_level)
      x->state = S_INDIR;
    return 0;
  }

  g_set_error(err, 1, 0, "Unexpected token in state %s: %s, %s",
    x->state == S_START    ? "START"    :
    x->state == S_FLOPEN   ? "FLOPEN"   :
    x->state == S_DIROPEN  ? "DIROPEN"  :
    x->state == S_INDIR    ? "INDIR"    :
    x->state == S_FILEOPEN ? "FILEOPEN" :
    x->state == S_INFILE   ? "INFILE"   :
    x->state == S_END      ? "END"      : "UNKNOWN",
    type == XMLT_OPEN    ? "OPEN"    :
    type == XMLT_CLOSE   ? "CLOSE"   :
    type == XMLT_ATTR    ? "ATTR"    :
    type == XMLT_ATTDONE ? "ATTDONE" : "???",
    arg1 ? arg1 : "<NULL>");
  return -1;
}


static int ctx_open(ctx_t *x, const char *file, GError **err) {
  memset(x, 0, sizeof(ctx_t));

  // open file
  x->fh_f = fopen(file, "r");
  if(!x->fh_f) {
    g_set_error_literal(err, 1, 0, g_strerror(errno));
    return -1;
  }

  // open BZ2 decompression
  if(strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0) {
    int bzerr;
    x->fh_bz = BZ2_bzReadOpen(&bzerr, x->fh_f, 0, 0, NULL, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to open bzip2 file (%d): %s", bzerr, g_strerror(errno));
      return -1;
    }
  }

  return 0;
}


static void ctx_close(ctx_t *x) {
  if(x->fh_bz) {
    int bzerr;
    BZ2_bzReadClose(&bzerr, x->fh_bz);
  }

  if(x->fh_f)
    fclose(x->fh_f);

  if(x->name)
    g_free(x->name);
}


fl_list_t *fl_load(const char *file, GError **err, gboolean local) {
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  ctx_t x;
  GError *ierr = NULL;
  if(ctx_open(&x, file, &ierr))
    goto end;

  x.state = S_START;
  x.root = fl_list_create("", FALSE);
  x.root->sub = g_ptr_array_new_with_free_func(fl_list_free);
  x.cur = x.root;
  x.filesize = G_MAXUINT64;
  x.local = local;

  if(xml_parse(entitycb, readcb, &x, &ierr))
    goto end;

end:
  g_return_val_if_fail(ierr || x.state == S_END, NULL);
  ctx_close(&x);
  if(ierr) {
    g_propagate_error(err, ierr);
    if(x.root)
      fl_list_free(x.root);
    x.root = NULL;
  }
  return x.root;
}





// Async version of fl_load(). Performs the load in a background thread. Only
// used for non-local filelists.

typedef struct async_t {
  char *file;
  void (*cb)(fl_list_t *, GError *, void *);
  void *dat;
  GError *err;
  fl_list_t *fl;
} async_t;


static gboolean async_d(gpointer dat) {
  async_t *arg = dat;
  arg->cb(arg->fl, arg->err, arg->dat);
  g_free(arg->file);
  g_slice_free(async_t, arg);
  return FALSE;
}


static void async_f(gpointer dat, gpointer udat) {
  async_t *arg = dat;
  arg->fl = fl_load(arg->file, &arg->err, FALSE);
  g_idle_add(async_d, arg);
}


// Ownership of both the file list and the error is passed to the callback
// function.
void fl_load_async(const char *file, void (*cb)(fl_list_t *, GError *, void *), void *dat) {
  static GThreadPool *pool = NULL;
  if(!pool)
    pool = g_thread_pool_new(async_f, NULL, 2, FALSE, NULL);
  async_t *arg = g_slice_new0(async_t);
  arg->file = g_strdup(file);
  arg->dat = dat;
  arg->cb = cb;
  g_thread_pool_push(pool, arg, NULL);
}

