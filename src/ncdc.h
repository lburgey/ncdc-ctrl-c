/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2014 Yoran Heling

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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <setjmp.h>

#include <wchar.h>
#include <locale.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_LINUX_SENDFILE
# include <sys/sendfile.h>
#elif HAVE_BSD_SENDFILE
# include <sys/socket.h>
# include <sys/uio.h>
#endif

#include <yuri.h>
#include <zlib.h>
#include <bzlib.h>
#include <sqlite3.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#ifdef USE_GCRYPT
#include <gcrypt.h>
#else
#include <gnutls/crypto.h>
#endif

#define _XOPEN_SOURCE_EXTENDED
#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#elif HAVE_NCURSES_NCURSES_H
#include <ncurses/ncurses.h>
#else
#include <ncurses.h>
#endif

#ifdef USE_GEOIP
#include <GeoIP.h>
#endif


// GnuTLS / libgcrypt functions
// crypt_aes128cbc() uses a 16-byte zero'd IV. Data is encrypted or decrypted in-place.
#ifdef USE_GCRYPT
#define crypt_rnd(buf, len) gcry_randomize(buf, len, GCRY_STRONG_RANDOM)
#define crypt_nonce(buf, len) gcry_create_nonce(buf, len)
static inline void crypt_aes128cbc(gboolean encrypt, const char *key, size_t keylen, char *data, size_t len) {
  gcry_cipher_hd_t ciph;
  char iv[16] = {};
  gcry_cipher_open(&ciph, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_CBC, 0);
  gcry_cipher_setkey(ciph, key, keylen);
  gcry_cipher_setiv(ciph, iv, 16);
  if(encrypt)
    g_warn_if_fail(gcry_cipher_encrypt(ciph, data, len, NULL, 0) == 0);
  else
    g_warn_if_fail(gcry_cipher_decrypt(ciph, data, len, NULL, 0) == 0);
  gcry_cipher_close(ciph);
}
#else
#define crypt_rnd(buf, len) g_warn_if_fail(gnutls_rnd(GNUTLS_RND_RANDOM, buf, len) == 0)
#define crypt_nonce(buf, len) g_warn_if_fail(gnutls_rnd(GNUTLS_RND_NONCE, buf, len) == 0)
static inline void crypt_aes128cbc(gboolean encrypt, const char *key, size_t keylen, char *data, size_t len) {
  gnutls_cipher_hd_t ciph;
  char iv[16] = {};
  gnutls_datum_t ivd = { (unsigned char *)iv, 16 };
  gnutls_datum_t keyd = { (unsigned char *)key, keylen };
  gnutls_cipher_init(&ciph, GNUTLS_CIPHER_AES_128_CBC, &keyd, &ivd);
  if(encrypt)
    g_warn_if_fail(gnutls_cipher_encrypt(ciph, data, len) == 0);
  else
    g_warn_if_fail(gnutls_cipher_decrypt(ciph, data, len) == 0);
  gnutls_cipher_deinit(ciph);
}
#endif
