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


#include "ncdc.h"
#include "geoip.h"

gboolean geoip_available = FALSE;

#ifdef USE_GEOIP
static GeoIP *geoip4;
static GeoIP *geoip6;
#endif


void geoip_reinit(int v) {
#ifdef USE_GEOIP
  GeoIP **var = v == 4 ? &geoip4 : &geoip6;
  if(*var) {
    GeoIP_delete(*var);
    *var = NULL;
  }

  const char *fn = var_get(0, v == 4 ? VAR_geoip_cc4 : VAR_geoip_cc6);
  if(!fn) {
    /* Get the file paths directly, so that we can offer more useful diagnostic
     * messages in case we fail to open it. Calling GeoIP_db_avail() ensures that
     * the GeoIPDBFileName variable has been initialized. */
    if(!GeoIPDBFileName)
        GeoIP_db_avail(GEOIP_COUNTRY_EDITION);
    fn = GeoIPDBFileName[v == 4 ? GEOIP_COUNTRY_EDITION : GEOIP_COUNTRY_EDITION_V6];
  }

  if(strcmp(fn, "disabled") != 0) {
    /* The '16' flag is GEOIP_SILENCE, but it's a fairly new option and not
     * defined in older versions. Just pass it along directly, ABI compatibility
     * should ensure this works with both old and new versions.
     * Also perform a g_file_test() first to ensure we're not opening
     * non-existing files. GeoIP versions that do not support GEOIP_SILENCE
     * will throw error messages on stdout/stderr, which screws up our ncurses
     * UI, so catching the most common error here is worth it. */
    *var = g_file_test(fn, G_FILE_TEST_EXISTS) ? GeoIP_open(fn, 16 | GEOIP_MEMORY_CACHE) : NULL;
    if(!*var)
      ui_mf(NULL, 0, "Can't open '%s', no country codes will be displayed for IPv%d addresses.", fn, v);
  }

  geoip_available = geoip4 || geoip6;
#endif
}


const char *geoip_country4(const char *ip) {
#ifdef USE_GEOIP
  if(geoip4)
    return GeoIP_country_code_by_addr(geoip4, ip);
#endif
  return NULL;
}


const char *geoip_country6(const char *ip) {
#ifdef USE_GEOIP
  if(geoip6)
    return GeoIP_country_code_by_addr_v6(geoip6, ip);
#endif
  return NULL;
}
