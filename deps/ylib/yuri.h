/* Copyright (c) 2012 Yoran Heling

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

/* This is a URI parser and validator that supports the following formats:
 * - <host>
 * - <host>:<port>
 * - <scheme>://<host>
 * - <scheme>://<host>:<port>
 * - <anything above>/<rest>
 *
 * <scheme> must match /^[a-zA-Z][a-zA-Z0-9\.+-]{0,14}$/
 * <host> is either:
 *   - A full IPv4 address (1.2.3.4)
 *   - An IPv6 address within square brackets
 *   - A domain name. That is, something like /^([a-zA-Z0-9-]{1,63}\.)+$/, with
 *     a maximum length of 255 characters. Actual parser is a bit more strict
 *     than the above regex.
 * <port> must be a decimal number between 1 and 65535 (both inclusive)
 * <rest> is, at this point, neither validated nor parsed
 *
 * Not supported (yet):
 * - Path and query string parsing
 * - Username / password parts
 * - Symbolic port names
 * - Internationalized domain names. Parsing only succeeds when the address
 *   is in the ASCII form.
 * - Protocol relative URLs (e.g. "//domain.com/")
 * - Percent encoding in anything before <rest> is not handled. Even though the
 *   RFC's seem to imply that this is allowed. (Percent-encoding in <rest>
 *   isn't handled, either, since this parser completely ignores <rest>)
 *
 * RFC1738 and RFC3986 have been used as reference, but strict adherence to
 * those specifications isn't a direct goal. In particular, this parser allows
 * <scheme> to be absent and requires <host> to be present and limited to an
 * IPv4/IPv6/DNS address. This makes the parser suitable for schemes like
 * irc://, http://, ftp:// and adc://, but unsuitable for stuff like mailto:
 * and magnet:.
 *
 * Incidentally, the implementation (yuri.c) is written in pure C and does not
 * use any libc functions.
 */

#ifndef YURI_H
#define YURI_H

#include <stdint.h>

/* See description above for the supported formats. */
typedef struct {

	/* Empty string if there was no scheme in the URI. Uppercase characters
	 * (A-Z) are automatically converted to lowercase (a-z). */
	char scheme[16];

	/* IPv4/IPv6 address or hostname. The square brackets around the IPv6
	 * address in the URI are not copied. No normalization or case modification
	 * is performed. */
	char host[256];

	/* 0 if no port was included in the URI. */
	uint16_t port;

	/* Points directly into the string given to yuri_parse(), NULL if there is
	 * no rest. Points to the character after the '/', so in the case of
	 * "example.com/path", rest will point to "path". */
	const char *rest;
} yuri_t;


/* Returns -1 if the URI isn't valid, 0 on success. On failure, the
 * contents of out may contain rubbish, otherwise all fields will have been set
 * to their parsed value.
 * Attempts to do as much (sane) validation as possible. */
int yuri_parse(const char *in, yuri_t *out);


/* Validates an IPv4 address according to RFC3986. Returns 0 if it's valid, -1
 * if it isn't. (Note that RFC3986 only allows a full IPv4 address with all
 * four octets present) */
int yuri_validate_ipv4(const char *str, int len);


/* Validates an IPv6 address. Returns -1 if it's invalid, 0 if it is. The given
 * string should not include square brackets. */
int yuri_validate_ipv6(const char *str, int len);

#endif

/* vim: set noet sw=4 ts=4: */
