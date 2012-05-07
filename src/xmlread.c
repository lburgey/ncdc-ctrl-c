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

/* This is a minimal XML stream parser designed for parsing ADC files.xml
 * documents. As these documents don't tend to use the full XML specification,
 * this parser lacks a few features:
 *
 * - Character entities (&#...;) are validated to be syntactically correct, but
 *   are otherwise ignored.
 * - Only ASCII characters are allowed in element and attribute names, Unicode
 *   characters in these constructs result in an error.
 * - The contents of attribute values are not validated to contain only
 *   characters in the allowed ranges. These values are passed to the
 *   application even if they don't form a valid UTF-8 sequence.  The only
 *   exception to this is the 0 byte, which will result in an error.
 * - Element contents (<Tag> ..contents.. </Tag>) are validated but otherwise
 *   ignored.
 * - An element may have multiple attributes with the same name, it is assumed
 *   that the application handles this situation.
 * - No validation is performed that open tags are properly closed. E.g.
 *   "<a></b>" is valid. The application is responsible for this validation.
 * - The 'encoding' information in the <?xml ..> tag is ignored.
 * - The following features are not supported, and will result in a parse error
 *   when present in the XML document:
 *   - CDATA sections (<![CDATA ..)
 *   - Processing instructions (<? .. ?>
 *   - Document type declaration (<!DOCTYPE ..>)
 *   - Attribute-list declarations (<!ATTLIST ..>)
 *   - Element type declarations (<!ELEMENT ..>)
 *   - Entity declarations (<!ENTITY ..>)
 *   - Conditional sections (<![IGNORE .. or <![INCLUDE ..)
 *   - Notation declarations (<!NOTATION ..>)
 *
 * (To my knowledge, the parser in DC++ and derivatives behave similarly).
 *
 * TODO: Since this parser is recursive, figure out some maximum bound on the
 * stack space used. (There should be a maximum, limited by MAX_DEPTH)
 */

#include "ncdc.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>


#if INTERFACE

#define XMLT_OPEN   1 // arg1 = tag name
#define XMLT_CLOSE  2 // arg1 = tag name or NULL for self-closing tags
#define XMLT_ATTR   3 // arg1 = name, arg2 = value (not validated to be correct UTF-8)

// Called whenever an XMLT_ entity has been found. Should return 0 to
// continue processing, anything else to abort.
typedef int (*xml_cb_t)(void *, int, const char *, const char *, GError **);

// Read callback. Should return -1 on error, 0 on EOF, number of bytes read
// otherwise.
typedef int (*xml_read_t)(void *, char *, int, GError **);

#endif


#define MAX_NAME      128
#define MAX_ATTRVAL   (8*1024) // this is more than enough for file lists.
#define MAX_DEPTH     50
#define READ_BUF_SIZE (32*1024)

struct ctx {
  xml_cb_t cb;
  xml_read_t read;
  void *dat;

  char name[MAX_NAME];
  char val[MAX_ATTRVAL];
  char readbuf[READ_BUF_SIZE];
  char *buf;
  gboolean readeof;
  int len;

  int level;
  int line;
  int byte;
  GError *err;
  jmp_buf jmp;
};



// Helper functions


static void err(struct ctx *x, const char *fmt, ...) {
  va_list arg;
  va_start(arg, fmt);
  if(!x->err) {
    char *msg = g_strdup_vprintf(fmt, arg);
    g_set_error(&x->err, 1, 0, "Line %d:%d: %s", x->line, x->byte, msg);
    g_free(msg);
  }
  va_end(arg);
  longjmp(x->jmp, 1);
}


// Make sure we have more than n bytes in the buffer. Returns the buffer
// length, which may be smaller on EOF. Also validates that the XML data does
// not contain the 0 byte (this simplifies error checking a bit).
static int fill(struct ctx *x, int n) {
  if(G_LIKELY(x->len >= n))
    return x->len;
  if(x->readeof)
    return x->len;

  if(x->len > 0)
    memmove(x->readbuf, x->buf, x->len);
  x->buf = x->readbuf;

  do {
    int r = x->read(x->dat, x->readbuf + x->len, READ_BUF_SIZE - x->len, &x->err);
    if(r < 0)
      err(x, "Parse error");
    if(!r) {
      x->readeof = TRUE;
      break;
    }
    if(memchr(x->readbuf + x->len, 0, r) != NULL)
      err(x, "Invalid zero byte in XML data");
    x->len += r;
  } while(x->len < n);

  return x->len;
}


// Require n bytes to be present, set error otherwise.
static void rfill(struct ctx *x, int n) {
  if(G_UNLIKELY(n >= x->len) && fill(x, n) < n)
    err(x, "Unexpected EOF");
}


// consume some characters (also updates ->bytes and ->lines)
static void con(struct ctx *x, int n) {
  int i = 0;
  while(i < n) {
    if(x->buf[i++] == '\n') {
      x->line++;
      x->byte = 0;
    }
    x->byte++;
  }
  x->buf += n;
  x->len -= n;
}


// Validate and consume a string literal
static void lit(struct ctx *x, const char *str) {
  int len = strlen(str);
  rfill(x, len);
  if(strncmp(x->buf, str, len) != 0)
    err(x, "Expected '%s'", str);
  con(x, len);
}




// Language definition


#define isWhiteSpace(x) (x == 0x20 || x == 0x09 || x == 0x0d || x == 0x0a)
#define isDecimal(x) ('0' <= x && x <= '9')
#define isHex(x) (isDecimal(x) || ('a' <= x && x <= 'f') || ('A' <= x && x <= 'F'))
#define isNameStartChar(x) (x == ':' || ('A' <= x && x <= 'Z') || x == '_' || ('a' <= x && x <= 'z'))
#define isNameChar(x) (isNameStartChar(x) || x == '-' || x == '.' || isDecimal(x))
#define isCharData(x) (x != '&' && x != '<')


// Consumes whitespace until an other character or EOF was found.  If req, then
// there must be at least one whitespace character, otherwise it's optional.
static void S(struct ctx *x, int req) {
  if(req) {
    rfill(x, 1);
    if(!isWhiteSpace(*x->buf))
      err(x, "White space expected, got '%c'", *x->buf);
  }
  while((x->len > 0 || fill(x, 1) > 0) && isWhiteSpace(*x->buf))
    con(x, 1);
}


static void Eq(struct ctx *x) {
  S(x, 0);
  lit(x, "=");
  S(x, 0);
}


// Parses a CharRef or EntityRef and writes the result to x->val+n, returning
// the number of bytes written (either 0 or 1).
// Note: CharRef's are parsed but ignored. This is what DC++ does, and
// simplifies things a bit. Custom EntityRefs are not supported, only those
// predefined in the XML standard can be used.
static int Reference(struct ctx *x, int n) {
  con(x, 1); // Assuming the caller has already verified that this is indeed a Reference.

  // We're currently parsing [^;]* here, while the standard requires a (more
  // strict) 'Name' token or a CharRef. This doesn't really matter, since we
  // validate the contents of name later on.
  char name[16] = {};
  int i = 0;
  rfill(x, 1);
  while(i < 15 && *x->buf != ';') {
    name[i++] = *x->buf;
    con(x, 1);
    rfill(x, 1);
  }
  if(i >= 15)
    err(x, "Entity name too long");
  con(x, 1);

  // Predefined entities
#define p(s, c) if(strcmp(name, s) == 0) {x->val[n] = c; return 1;}
  p("lt", '<');
  p("gt", '>');
  p("amp", '&');
  p("apos", '\'');
  p("quot", '"');
#undef p

  // CharRefs
  if(name[0] == '#' && name[1] == 'x') {
    i = 2;
    do
      if(!isHex(name[i]))
        err(x, "Invalid character reference '&%s;'", name);
    while(++i < strlen(name));
    return 0;
  }

  // decimal CharRef
  if(name[0] == '#') {
    i = 1;
    do
      if(!isDecimal(name[i]))
        err(x, "Invalid character reference '&%s;'", name);
    while(++i < strlen(name));
    return 0;
  }

  // Anything else is an error
  err(x, "Unknown entity reference '&%s;'", name);
  return 0;
}


// Parses an attribute value and writes its (decoded) contents to x->val.
static void AttValue(struct ctx *x) {
  rfill(x, 2);
  char esc = *x->buf;
  if(esc != '"' && esc != '\'')
    err(x, "' or \" expected, got '%c'", *x->buf);
  con(x, 1);

  int n = 0;
  while(*x->buf != esc) {
    if(*x->buf == '<')
      err(x, "Invalid '<' in attribute value");
    if(n >= MAX_ATTRVAL-4)
      err(x, "Too long attribute value.");
    if(*x->buf == '&')
      n += Reference(x, n);
    else {
      x->val[n++] = *x->buf;
      con(x, 1);
    }
    rfill(x, 1);
  }
  x->val[n] = 0;

  if(*x->buf != esc)
    err(x, "%c expected, got %c", esc, *x->buf);
  con(x, 1);
}


static void comment(struct ctx *x) {
  lit(x, "<!--");
  while(1) {
    rfill(x, 3);
    if(x->buf[0] == '-' && x->buf[1] == '-') {
      if(x->buf[2] != '>')
        err(x, "'--' not allowed in XML comment");
      con(x, 3);
      break;
    }
    con(x, 1);
  }
}


// Consumes any number of whitespace and comments. (So it's actually Misc*)
static void Misc(struct ctx *x) {
  while(fill(x, 4) >= 4) {
    if(strncmp(x->buf, "<!--", 4) == 0) {
      comment(x);
      continue;
    }
    if(!isWhiteSpace(*x->buf))
      break;
    S(x, 0);
  }
  S(x, 0);
}


// Consumes a name and stores it in x->name.
static void Name(struct ctx *x) {
  rfill(x, 1);
  int n = 0;
  if(!isNameStartChar(*x->buf))
    err(x, "Invalid character in element or attribute name");
  x->name[n++] = *x->buf;
  con(x, 1);
  while(n < MAX_NAME-1 && fill(x, 1) > 0 && isNameChar(*x->buf)) {
    x->name[n++] = *x->buf;
    con(x, 1);
  }
  if(n >= MAX_NAME-1)
    err(x, "Too long element or attribute name");
  x->name[n] = 0;
}


// Returns the number of bytes consumed.
static int CharData(struct ctx *x) {
  int r = 0;
  while(fill(x, 3) >= 3) {
    if(!isCharData(*x->buf))
      return r;
    if(strncmp(x->buf, "]]>", 3) == 0)
      err(x, "']]>' not allowed in content");
    r++;
    con(x, 1);
  }

  while(fill(x, 1) >= 1) {
    if(!isCharData(*x->buf))
      return r;
    r++;
    con(x, 1);
  }
  return r;
}


static void element(struct ctx *x);

static void content(struct ctx *x) {
  CharData(x);
  while(1) {
    // Getting an EOF 2 bytes after content is always an error regardless of
    // the content (since content always follows a close tag), so this rfill
    // usage is safe.
    rfill(x, 2);
    if(x->buf[0] == '<' && x->buf[1] == '/')
      return;
    else if(x->buf[0] == '<' && x->buf[1] == '!')
      comment(x);
    else if(x->buf[0] == '<')
      element(x);
    else if(x->buf[0] == '&')
      Reference(x, 0);
    else if(!CharData(x)) // shouldn't happen, actually.
      err(x, "Invalid character in content");
  }
}


static void element(struct ctx *x) {
  if(x->level <= 0)
    err(x, "Maximum element depth exceeded");

  lit(x, "<");
  Name(x);
  if(x->cb(x->dat, XMLT_OPEN, x->name, NULL, &x->err))
    err(x, "Processing aborted by the application");

  while(1) {
    // Is this tag ending yet?
    rfill(x, 1);
    if(*x->buf == '>' || *x->buf == '/')
      break;
    S(x, 1);
    if(*x->buf == '>' || *x->buf == '/')
      break;

    // Otherwise, we have an attribute
    Name(x);
    Eq(x);
    AttValue(x);
    if(x->cb(x->dat, XMLT_ATTR, x->name, x->val, &x->err))
      err(x, "Processing aborted by the application");
  }

  // EmptyElementTag
  if(*x->buf == '/') {
    lit(x, "/>");
    if(x->cb(x->dat, XMLT_CLOSE, NULL, NULL, &x->err))
      err(x, "Processing aborted by the application");
    return;
  }

  // Otherwise, this was an STag
  lit(x, ">");
  x->level--;
  content(x);
  x->level++;
  lit(x, "</");
  Name(x);
  lit(x, ">");
  if(x->cb(x->dat, XMLT_CLOSE, x->name, NULL, &x->err))
    err(x, "Processing aborted by the application");
}


static void XMLDecl(struct ctx *x) {
  if(fill(x, 5) < 5 || strncmp(x->buf, "<?xml", 5) != 0)
    return;

  con(x, 5);
  S(x, 1);

  // version
  lit(x, "version");
  Eq(x);
  AttValue(x);
  if(x->val[0] != '1' || x->val[1] != '.')
    err(x, "Invalid XML version");
  int i = 2;
  do
    if(!isDecimal(x->val[i]))
      err(x, "Invalid XML version");
  while(++i < strlen(x->val));

  // Accepts either whitespace or a '?' to signal the end of this XML
  // declaration.
#define se rfill(x, 1); if(x->buf[0] == '?') goto end; S(x, 1); rfill(x, 1); if(x->buf[0] == '?') goto end;

  // encoding
  se
  if(x->buf[0] == 'e') {
    lit(x, "encoding");
    Eq(x);
    AttValue(x);
    se
  }

  // standalone
  lit(x, "standalone");
  Eq(x);
  AttValue(x);
  if(strcmp(x->val, "yes") != 0 && strcmp(x->val, "no") != 0)
    err(x, "Invalid value for \"standalone\"");
  S(x, 0);
#undef se

end:
  lit(x, "?>");
}




// Parses the complete XML document, returns 0 on success or -1 on error.
int xml_parse(xml_cb_t cb, xml_read_t read, void *dat, GError **e) {
  // Don't allocate this the stack, it's fairly large.
  struct ctx *x = g_new(struct ctx, 1);
  x->dat = dat;
  x->cb = cb;
  x->read = read;

  x->buf = x->readbuf;
  x->readeof = FALSE;
  x->len = 0;

  x->line = x->byte = 1;
  x->level = MAX_DEPTH;
  x->err = NULL;

  if(!setjmp(x->jmp)) {
    // UTF-8 BOM
    if(fill(x, 3) >= 3 && strncmp(x->buf, "\xef\xbb\xbf", 3) == 0)
      con(x, 3);
    XMLDecl(x);
    Misc(x);
    element(x);
    Misc(x);
    // We should have consumed everything now.
    if(fill(x, 1))
      err(x, "Expected end-of-file");
  }

  if(x->err)
    g_propagate_error(e, x->err);
  g_free(x);
  return 0;
}

