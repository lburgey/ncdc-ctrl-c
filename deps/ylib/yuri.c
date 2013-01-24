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

#include "yuri.h"
#include <stdlib.h>


/* The ctype.h functions are locale-dependent. We don't want that. */
#define y_isalpha(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z'))
#define y_isnum(x)   ((x) >= '0' && (x) <= '9')
#define y_isalnum(x) (y_isalpha(x) || y_isnum(x))
#define y_tolower(x) ((x) < 'A' || (x) > 'Z' ? (x) : (x)+0x20)

#define y_ishex(x)    (((x) >= 'a' && (x) <= 'f') || ((x) >= 'A' && (x) <= 'F') || y_isnum(x))
#define y_isscheme(x) ((x) == '+' || (x) == '-' || (x) == '.' || y_isalnum(x))
#define y_isdomain(x) ((x) == '-' || y_isalnum(x))


/* Copy len bytes from *src to *dest. A nil character is appended to dest, so
 * it must be large enough to hold len+1 bytes.
 * XXX: This is *not* equivalent to BSD strlcpy()! */
static void yuri__strlcpy(char *dest, const char *src, int len) {
	while(len-- > 0)
		*(dest++) = *(src++);
	*dest = 0;
}


/* Similar to yuri__strlcpy(), except calls y_tolower() on each character */
static void yuri__strllower(char *dest, const char *src, int len) {
	while(len-- > 0) {
		*dest = y_tolower(*src);
		src++;
		dest++;
	}
	*dest = 0;
}


/* Parses the "<scheme>://" part and returns the pointer after the scheme.
 * Simply returns 'in' if it couldn't find a (valid) scheme. */
static const char *yuri__scheme(const char *in, yuri_t *out) {
	const char *end = in;
	if(!y_isalpha(*end))
		return in;
	do
		++end;
	while(end <= in+15 && y_isscheme(*end));
	if(end > in+15 || *end != ':' || end[1] != '/' || end[2] != '/')
		return in;
	yuri__strllower(out->scheme, in, end-in);
	return end + 3;
}


/* Parses the ":<port>" part in the string pointed to by [in..end]. Returns the
 * new end of the string, or the current end if it couldn't find a (valid)
 * port string. */
static const char *yuri__port(const char *in, const char *end, yuri_t *out) {
	const char *nend = end-1;
	uint32_t res = 0, mul = 1;
	/* Read backwards */
	while(nend >= in && y_isnum(*nend)) {
		if(mul >= 100000)
			return end;
		res += mul * (*nend-'0');
		if(res > 65535)
			return end;
		mul *= 10;
		nend--;
	}
	if(!res || nend < in || *nend != ':' || nend[1] == '0')
		return end;
	out->port = res;
	return nend;
}


/* RFC3986, p. 19, IPv4address. */
int yuri_validate_ipv4(const char *str, int len) {
	int i;
	for(i=0; i<4; i++) {
		if(i) {
			if(len < 1 || *str != '.')
				return -1;
			str++; len--;
		}
		if(len >= 3 && ((str[0] == '2' && str[1] == '5' && str[2] >= '0' && str[2] <= '5')   /* 250-255 */
		             || (str[0] == '2' && str[1] >= '0' && str[1] <= '4' && y_isnum(str[2])) /* 200-249 */
		             || (str[0] == '1' && y_isnum(str[1]) && y_isnum(str[2])))               /* 100-199 */
				) {
			str += 3; len -= 3;
		} else if(len >= 2 && str[0] >= '1' && str[0] <= '9' && y_isnum(str[1])) { /* 10-99 */
			str += 2; len -= 2;
		} else if(len >= 1 && y_isnum(str[0])) { /* 0-9 */
			str++; len--;
		} else
			return -1;
	}
	return len ? -1 : 0;
}


int yuri_validate_ipv6(const char *str, int len) {
	int i, hasskip = 0;
	if(len >= 2 && *str == ':' && str[1] == ':') {
		hasskip = 1;
		str += 2; len -= 2;
	}
	for(i=0; i<8; i++) {
		if(!len && hasskip)
			break;
		/* separator */
		if(i) {
			if(len < 1 || *str != ':')
				return -1;
			str++; len--;
		}
		if(len < 1)
			return -1;
		if(i && !hasskip && *str == ':') {
			hasskip = 1;
			str++; len--;
			if(!len)
				break;
		}
		/* last 32 bits may use IPv4 notation */
		if(len >= 4 && (hasskip ? i < 6 : i == 6) && str[1] != ':' && str[2] != ':' && (str[1] == '.' || str[2] == '.' || str[3] == '.'))
			return yuri_validate_ipv4(str, len);
		/* 1-4 hex digits */
		if(!y_ishex(*str))
			return -1;
		str++; len--;
#define H if(len >= 1 && y_ishex(*str)) { str++; len--; }
		H H H
#undef H
	}
	return len || (hasskip && i==8) ? -1 : 0;
}


/* RFC1034 section 3.5 has an explanation of a (commonly used) domain syntax,
 * but I suspect it may be overly strict. This implementation will suffice, I
 * suppose. Unlike the IPv4 and IPv6 validators, this function is not public.
 * Mostly because DNS names aren't strictly specified, and because there are
 * alternative representations depending on where the name comes from (see also
 * the comment on the length check) */
static int yuri__validate_dns(const char *str, int len) {
	int haslabel = 0, /* whether we've seen a label */
		lastishyp = 0, /* whether the last seen character in the label is a hyphen */
		startdig = 0, /* whether the last seen label starts with a digit (Not allowed per RFC1738, a sensible restriction IMO) */
		llen = 0; /* length of the current label */

	/* In the case of percent encoding, the length of the domain may be much
	 * larger. But this implementation (currently) does not accept percent
	 * encoding in the domain name. Similarly, the character limit applies to
	 * the ASCII form of the domain, in the case of an IDN, this check doesn't
	 * really work either. This implementation (currently) does not support
	 * IDN. In fact, this function should be "validate-and-normalize" instead
	 * of just validate in such a case. */
	if(len > 255)
		return -1;

	for(; len > 0; str++, len--) {
		if(*str == '.') {
			if(!llen || lastishyp)
				return -1;
			llen = 0;
			continue;
		} else if(llen >= 63)
			return -1;
		if(!y_isdomain(*str))
			return -1;
		lastishyp = *str == '-';
		if(llen == 0) {
			if(lastishyp) /* That is, don't start with a hyphen */
				return -1;
			startdig = y_isnum(*str);
		}
		haslabel = 1;
		llen++;
	}
	return haslabel && !startdig ? 0 : -1;
}


int yuri_parse(const char *in, yuri_t *out) {
	const char *authend, *hostend;

	*out->scheme = 0;
	*out->host = 0;
	out->port = 0;
	out->rest = NULL;

	in = yuri__scheme(in, out);

	/* Find the end of the authority component (RFC3986, section 3.2) */
	for(authend=in; *authend && *authend != '/' && *authend != '?' && *authend != '#'; authend++)
		;

	hostend = yuri__port(in, authend, out);
	if(hostend-in > 2 && *in == '[' && *(hostend-1) == ']' && yuri_validate_ipv6(in+1, hostend-in-2) == 0)
		yuri__strlcpy(out->host, in+1, hostend-in-2);
	else if(yuri_validate_ipv4(in, hostend-in) == 0 || yuri__validate_dns(in, hostend-in) == 0)
		yuri__strlcpy(out->host, in, hostend-in);
	else
		return -1;

	if(*authend && *authend == '/')
		authend++;
	out->rest = *authend ? authend : NULL;

	return 0;
}

/* vim: set noet sw=4 ts=4: */
