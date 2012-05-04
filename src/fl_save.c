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
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <libxml/xmlwriter.h>
#include <bzlib.h>


struct fl_save_context {
  char *file;     // some name, for debugging purposes
  BZFILE *fh_bz;  // if BZ2 compression is enabled (implies fh_h!=NULL)
  FILE *fh_f;     // if we're working with a file
  GString *buf;   // if we're working with a buffer
  GError **err;
};


static int fl_save_write(void *context, const char *buf, int len) {
  struct fl_save_context *xc = context;
  if(xc->fh_bz) {
    int bzerr;
    BZ2_bzWrite(&bzerr, xc->fh_bz, (char *)buf, len);
    if(bzerr == BZ_OK)
      return len;
    if(bzerr == BZ_IO_ERROR) {
      g_set_error_literal(xc->err, 1, 0, "bzip2 write error.");
      return -1;
    }
    g_return_val_if_reached(-1);
  } else if(xc->fh_f) {
    int r = fwrite(buf, 1, len, xc->fh_f);
    if(r < 0)
      g_set_error(xc->err, 1, 0, "Write error: %s", g_strerror(errno));
    return r;
  } else if(xc->buf) {
    g_string_append_len(xc->buf, buf, len);
    return len;
  } else
    g_return_val_if_reached(-1);
}


static int fl_save_close(void *context) {
  struct fl_save_context *xc = context;
  int bzerr;
  if(xc->fh_bz)
    BZ2_bzWriteClose(&bzerr, xc->fh_bz, 0, NULL, NULL);
  if(xc->fh_f)
    fclose(xc->fh_f);
  g_free(xc->file);
  g_slice_free(struct fl_save_context, xc);
  return 0;
}


// recursive
static gboolean fl_save_childs(xmlTextWriterPtr writer, struct fl_list *fl, int level) {
  int i;
  for(i=0; i<fl->sub->len; i++) {
    struct fl_list *cur = g_ptr_array_index(fl->sub, i);
#define CHECKFAIL(f) if(f < 0) return FALSE
    if(cur->isfile && cur->hastth) {
      char tth[40];
      base32_encode(cur->tth, tth);
      tth[39] = 0;
      CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"File"));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Name", (xmlChar *)cur->name));
      CHECKFAIL(xmlTextWriterWriteFormatAttribute(writer, (xmlChar *)"Size", "%"G_GUINT64_FORMAT, cur->size));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"TTH", (xmlChar *)tth));
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
    if(!cur->isfile) {
      CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"Directory"));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Name", (xmlChar *)cur->name));
      if(level < 1 && fl_list_isempty(cur))
        CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Incomplete", (xmlChar *)"1"));
      if(level > 0)
        fl_save_childs(writer, cur, level-1);
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
#undef CHECKFAIL
  }
  return TRUE;
}


static xmlTextWriterPtr fl_save_open(const char *file, gboolean isbz2, GString *buf, GError **err) {
  // open file (if any)
  FILE *f = NULL;
  if(file) {
    f = fopen(file, "w");
    if(!f) {
      g_set_error_literal(err, 1, 0, g_strerror(errno));
      return NULL;
    }
  }

  // open compressor (if needed)
  BZFILE *bzf = NULL;
  if(f && isbz2) {
    int bzerr;
    bzf = BZ2_bzWriteOpen(&bzerr, f, 7, 0, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to create BZ2 file (%d)", bzerr);
      fclose(f);
      return NULL;
    }
  }

  // create writer
  struct fl_save_context *xc = g_slice_new0(struct fl_save_context);
  xc->err = err;
  xc->file = file ? g_strdup(file) : g_strdup("string buffer");
  xc->fh_f = f;
  xc->fh_bz = bzf;
  xc->buf = buf;
  xmlTextWriterPtr writer = xmlNewTextWriter(xmlOutputBufferCreateIO(fl_save_write, fl_save_close, xc, NULL));

  if(!writer) {
    fl_save_close(xc);
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }
  return writer;
}


gboolean fl_save(struct fl_list *fl, const char *file, GString *buf, int level, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  // open a temporary file for writing
  gboolean isbz2 = FALSE;
  char *tmpfile = NULL;
  if(file) {
    isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;
    tmpfile = g_strdup_printf("%s.tmp-%d", file, rand());
  }

  xmlTextWriterPtr writer = fl_save_open(tmpfile, isbz2, buf, err);
  if(!writer) {
    g_free(tmpfile);
    return FALSE;
  }

  // write
  gboolean success = TRUE;
#define CHECKFAIL(f) if((f) < 0) { success = FALSE; goto fl_save_error; }
  CHECKFAIL(xmlTextWriterSetIndent(writer, 1));
  CHECKFAIL(xmlTextWriterSetIndentString(writer, (xmlChar *)"\t"));
  // <FileListing ..>
  CHECKFAIL(xmlTextWriterStartDocument(writer, NULL, "utf-8", "yes"));
  CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"FileListing"));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Version", (xmlChar *)"1"));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Generator", (xmlChar *)PACKAGE_STRING));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"CID", (xmlChar *)var_get(0, VAR_cid)));

  char *path = fl ? fl_list_path(fl) : g_strdup("/");
  // Make sure the base path always ends with a slash, some clients will fail otherwise.
  if(path[strlen(path)-1] != '/') {
    char *tmp = g_strdup_printf("%s/", path);
    g_free(path);
    path = tmp;
  }
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Base", (xmlChar *)path));
  g_free(path);

  // all <Directory ..> elements
  if(fl && fl->sub) {
    if(!fl_save_childs(writer, fl, level-1)) {
      success = FALSE;
      goto fl_save_error;
    }
  }

  CHECKFAIL(xmlTextWriterEndElement(writer));

  // close
fl_save_error:
  xmlTextWriterEndDocument(writer);
  xmlFreeTextWriter(writer);

  // rename or unlink file
  if(file) {
    if(success && rename(tmpfile, file) < 0) {
      if(err && !*err)
        g_set_error_literal(err, 1, 0, g_strerror(errno));
      success = FALSE;
    }
    if(!success)
      unlink(tmpfile);
    g_free(tmpfile);
  }
  return success;
}
