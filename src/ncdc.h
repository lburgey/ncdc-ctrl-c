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

#include "config.h"
#include <glib.h>
#include <glib/gprintf.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

#define _XOPEN_SOURCE_EXTENDED
#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#elif HAVE_NCURSES_NCURSES_H
#include <ncurses/ncurses.h>
#else
#include <ncurses.h>
#endif

// Use GIT_VERSION, if available
#ifdef GIT_VERSION
# undef VERSION
# define VERSION GIT_VERSION
#endif

#define TIMEOUT_SUPPORT GLIB_CHECK_VERSION(2, 26, 0)

// SUDP requires gnutls_rnd(), added in 2.12.0
#define SUDP_SUPPORT (GNUTLS_VERSION_MAJOR > 2 || (GNUTLS_VERSION_MAJOR == 2 && GNUTLS_VERSION_MINOR >= 12))

/* GnuTLS before 2.x may require manual linking against gcrypt to initialize
 * thread-safe operation. For 2.12.x (and perhaps 2.10, not entirely sure),
 * GnuTLS may also use libnettle, in which case this is not needed. There's no
 * reliable way to know which library is being used - neither at compile time
 * nor at run time. Curl does attempt to get around this issue "properly", but
 * in many cases pointlessly links in gcrypt when it's not needed. GnuTLS 3.0
 * always uses nettle and does not require any manual initialization of the
 * library.
 * I'm not fond of participating in the autodetect or overlinking nightmare, so
 * I'll take another approach: If you're not using GnuTLS 3.0+, TLS-enabled
 * file transfers will be disabled by default, and you'll get a big fat warning
 * if you try to change that setting. :-)
 * Note #1: Connecting to TLS-enabled hubs is no problem either way, since
 * those remain in the main thread and as such have no threading issues.
 * Note #2: This is a compile-time check. It's possible that someone built
 * against 2.x but links with 3.x at run-time. I doubt that happens often in
 * practice, though, especially considering that 2.x and 3.x are not completely
 * ABI compatible. */
#define THREADSAFE_TLS (GNUTLS_VERSION_MAJOR >= 3)

