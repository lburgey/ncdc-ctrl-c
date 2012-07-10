# Generate a statically linked ncdc.
#
# Requirements:
#   GNU make, GNU tar, gcc, perl, wget, bash-stuff, python2
#   and perhaps a few more things I hadn't noticed yet.
#
# Usage:
#   This assumes you're in the ncdc git repository and have done the autoreconf
#   thing. Make sure you don't have any built files in the ncdc directory, do a
#   'make distclean' to be sure. Then cd into this 'static' directory and run
#   'make' as follows:
#
#     make HOST=$host ARCH=$arch
#
#   Where $host is either 'linux' or 'freebsd', and $arch is either 'i386',
#   'x86_64' or 'arm'.  Compilation may take anywhere between 5 minutes to 5
#   hours, but if everything went alright you'll have a nice ncdc binary in the
#   bin/ directory.
#
# Tested systems:
# - Arch Linux x86_64, i686 and arm:
#   Works without problems.
#   The arm build has been tested with the Raspberry Pi image of ArchLinuxArm
#   in a qemu guest. I currently have no idea on which hardware it'll actually
#   run.
# - FreeBSD 8.1 amd64
#   Compiles, but there seems to be an issue with displaying wide characters.
# - FreeBSD 8.1 i385
#   Fails to compile. :-(
#
# Note that there are still some issues with aborting/resuming the process and
# with recompiling only a single lib. In theory, deleting one of the package
# directories should recompile that lib and everything that depends on it, but
# that tends to fail sometimes. A 'make clean' followed by 'make' should always
# work, though.
#
# I am definitely no expert with writing scripts and makefiles such as this
# one, and even less an expert with build systems. Suggestions and/or patches
# to improve or this makefile are highly appreciated.
#
# TODO:
# - Fix the FreeBSD builds
# - Autodetect HOST and ARCH
# - Support cross-compilation? (Sounds like more work that setting up VMs)

# linux or freebsd
HOST:=linux
# i386, x86_64 or arm
ARCH:=x86_64

GLIBV=http://ftp.gnome.org/pub/gnome/sources/glib/2.32/glib-2.32.3.tar.xz
BZV=http://bzip.org/1.0.6/bzip2-1.0.6.tar.gz
ZV=http://zlib.net/zlib-1.2.7.tar.gz
SQLV=http://sqlite.org/sqlite-autoconf-3071300.tar.gz
GMPV=ftp://ftp.gmplib.org/pub/gmp-5.0.5/gmp-5.0.5.tar.xz
NETV=http://www.lysator.liu.se/~nisse/archive/nettle-2.4.tar.gz
TLSV=http://ftp.gnu.org/gnu/gnutls/gnutls-3.0.21.tar.xz
PKGV=http://pkgconfig.freedesktop.org/releases/pkg-config-0.26.tar.gz
NCV=http://ftp.gnu.org/pub/gnu/ncurses/ncurses-5.9.tar.gz
MUSLV=http://www.etalabs.net/musl/releases/musl-0.9.2.tar.gz

GLIBD=$(shell pwd)/$(shell basename $(GLIBV) .tar.xz)
BZD=$(shell pwd)/$(shell basename $(BZV) .tar.gz)
SQLD=$(shell pwd)/$(shell basename $(SQLV) .tar.gz)
ZD=$(shell pwd)/$(shell basename $(ZV) .tar.gz)
GMPD=$(shell pwd)/$(shell basename $(GMPV) .tar.xz)
NETD=$(shell pwd)/$(shell basename $(NETV) .tar.gz)
TLSD=$(shell pwd)/$(shell basename $(TLSV) .tar.xz)
PKGD=$(shell pwd)/$(shell basename $(PKGV) .tar.gz)
NCD=$(shell pwd)/$(shell basename $(NCV) .tar.gz)
MUSLD=$(shell pwd)/$(shell basename $(MUSLV) .tar.gz)

PREFIX=$(shell pwd)
HOSTCC=gcc
LIBC=
GLIBICONV=no
CFLAGS=-O2 -static
GMPABI=

# linux (uses musl)
ifeq ($(HOST),linux)
TAR=tar
LIBC=$(MUSLD)/_done
CC=$(PREFIX)/bin/musl-gcc -D_GNU_SOURCE
ifeq ($(ARCH),i386)
CFLAGS+= -march=i486
GMPABI=ABI=32
endif

# freebsd (native libc)
else
TAR=gtar
CC=$(HOSTCC) -static -L/usr/local/lib -I/usr/local/include
GLIBICONV=gnu
endif

PKG_CONFIG=$(PREFIX)/bin/pkg-config --static
PKG_CONFIG_PATH=$(PREFIX)/lib/pkgconfig

export PKG_CONFIG
export PKG_CONFIG_PATH
export CC
export CFLAGS

.PHONY: musl nc pkg tls net gmp z sql bz glib ncdc clean

all: ncdc

# I don't know why I have to do the 'make install' twice, but it makes glib happy.
$(MUSLD)/_done:
	[ -e "`basename $(MUSLV)`" ] || wget "$(MUSLV)"
	[ -e "$(MUSLD)" ] || $(TAR) -xvf "`basename $(MUSLV)`"
	cd "$(MUSLD)" && CC=${HOSTCC} ./configure --prefix="$(PREFIX)" --disable-shared
	CC=${HOSTCC} $(MAKE) -C "$(MUSLD)" install
	$(MAKE) -C "$(MUSLD)" install
	touch $(MUSLD)/_done

# We need to compile and install ncurses twice: Once to generate the 'tic'
# program, and the second time to create an ncurses library with some terminfo
# descriptions compiled-in as fallback.
$(NCD)/_done: $(LIBC)
	[ -e "`basename $(NCV)`" ] || wget "$(NCV)"
	[ -e "$(NCD)" ] || $(TAR) -xvf "`basename $(NCV)`"
	cd "$(NCD)" && ./configure --prefix="$(PREFIX)" --with-pc-path="$(PKG_CONFIG_PATH)"\
		--without-cxx --without-cxx-binding --without-ada --without-manpages --with-progs\
		--without-tests --without-curses-h --without-pkg-config --without-shared --without-debug\
		--without-gpm --without-sysmouse --enable-widec
	$(MAKE) -C "$(NCD)"
	$(MAKE) -C "$(NCD)" install.progs
	$(MAKE) -C "$(NCD)" clean
	cd "$(NCD)" && ./configure --prefix="$(PREFIX)" --with-pc-path="$(PKG_CONFIG_PATH)"\
		--without-cxx --without-cxx-binding --without-ada --without-manpages --without-progs\
		--without-tests --without-curses-h --without-pkg-config --without-shared --without-debug\
		--without-gpm --without-sysmouse --enable-widec --with-default-terminfo-dir=/usr/share/terminfo\
		--with-terminfo-dirs=/usr/share/terminfo:/lib/terminfo:/usr/local/share/terminfo\
		--with-fallbacks="screen linux vt100 xterm" PATH="$$PATH:$(PREFIX)/bin"
	$(MAKE) -C "$(NCD)"
	$(MAKE) -C "$(NCD)" install.libs
	touch $(NCD)/_done

$(PKGD)/_done: $(GLIBD)/_done $(LIBC)
	[ -e "`basename $(PKGV)`" ] || wget "$(PKGV)"
	[ -e "$(PKGD)" ] || $(TAR) -xvf "`basename $(PKGV)`"
	cd "$(PKGD)" && ./configure --prefix="$(PREFIX)" --with-pc-path="$(PKG_CONFIG_PATH)"\
		--disable-shared --enable-static\
		GLIB_CFLAGS="-I$(PREFIX)/include/glib-2.0 -I$(PREFIX)/lib/glib-2.0/include"\
		GLIB_LIBS="-L$(PREFIX)/lib -lglib-2.0"
	$(MAKE) -C "$(PKGD)" install
	touch $(PKGD)/_done

$(TLSD)/_done: $(NETD)/_done $(ZD)/_done $(PKGD)/_done
	[ -e "`basename $(TLSV)`" ] || wget "$(TLSV)"
	[ -e "$(TLSD)" ] || $(TAR) -xvf "`basename $(TLSV)`"
	@# gnulib patching, required for musl
	cd "$(TLSD)/gl" && for i in printf.c fprintf.c freadahead.c closein.c fseterr.c vfprintf.c ; do echo ''>$$i; done
	echo "#define __printf__ printf" >> "$(TLSD)/config.h.in"
	echo "#define rpl_fprintf fprintf" >> "$(TLSD)/config.h.in"
	@# end-of-gnulib-patching
	cd "$(TLSD)" && ./configure --prefix="$(PREFIX)" --disable-gtk-doc-html\
		--disable-shared --enable-static --disable-cxx --disable-srp-authentication\
		--disable-openpgp-authentication --disable-openssl-compatibility\
		--disable-guile --disable-crywrap --with-included-libtasn1 --without-p11-kit\
		CPPFLAGS="-I$(PREFIX)/include"
	$(MAKE) -C "$(TLSD)/gl" install
	$(MAKE) -C "$(TLSD)/lib" install
	$(MAKE) -C "$(TLSD)" install-data
	touch $(TLSD)/_done

$(NETD)/_done: $(LIBC) $(GMPD)/_done
	[ -e "`basename $(NETV)`" ] || wget "$(NETV)"
	[ -e "$(NETD)" ] || $(TAR) -xvf "`basename $(NETV)`"
	cd "$(NETD)" && ./configure --prefix="$(PREFIX)" --disable-shared\
		CPPFLAGS="-I$(PREFIX)/include"\
		LDFLAGS="-L$(PREFIX)/lib"
	$(MAKE) -C "$(NETD)" install
	touch $(NETD)/_done

$(GMPD)/_done: $(LIBC)
	[ -e "`basename $(GMPV)`" ] || wget "$(GMPV)"
	[ -e "$(GMPD)" ] || $(TAR) -xvf "`basename $(GMPV)`"
	cd "$(GMPD)" && ./configure --prefix="$(PREFIX)" --without-readline --disable-shared --enable-static $(GMPABI)
	$(MAKE) -C "$(GMPD)" install
	touch $(GMPD)/_done

$(ZD)/_done: $(LIBC)
	[ -e "`basename $(ZV)`" ] || wget "$(ZV)"
	[ -e "$(ZD)" ] || $(TAR) -xvf "`basename $(ZV)`"
	cd "$(ZD)" && ./configure --prefix="$(PREFIX)" --static
	$(MAKE) -C "$(ZD)" install
	touch $(ZD)/_done

$(SQLD)/_done: $(LIBC)
	[ -e "`basename $(SQLV)`" ] || wget "$(SQLV)"
	[ -e "$(SQLD)" ] || $(TAR) -xvf "`basename $(SQLV)`"
	cd "$(SQLD)" && ./configure --prefix="$(PREFIX)" --disable-readline\
		--disable-dynamic-extensions --disable-shared --enable-static
	$(MAKE) -C "$(SQLD)" install
	touch $(SQLD)/_done

$(BZD)/_done: $(LIBC)
	[ -e "`basename $(BZV)`" ] || wget "$(BZV)"
	[ -e "$(BZD)" ] || $(TAR) -xvf "`basename $(BZV)`"
	perl -pi -e 's/CC=(.+)/#CC=$$1/' "$(BZD)/Makefile"
	perl -pi -e 's/CFLAGS=(.+)/#CFLAGS=$$1/' "$(BZD)/Makefile"
	CFLAGS="$$CFLAGS -D_FILE_OFFSET_BITS=64" $(MAKE) -C "$(BZD)" install PREFIX="$(PREFIX)"
	touch $(BZD)/_done

# Note: PKG_CONFIG=true effectively tricks configure into thinking pkg-config
# exists and all the packages it probes for exist as well. This works for us,
# because all pkg-config-requiring stuff is for GIO rather than the core glib,
# and we're not building GIO at all.
$(GLIBD)/_done: $(LIBC)
	[ -e "`basename $(GLIBV)`" ] || wget "$(GLIBV)"
	[ -e "$(GLIBD)" ] || $(TAR) -xvf "`basename $(GLIBV)`"
	cd "$(GLIBD)"\
		&& PKG_CONFIG=true ./configure --prefix="$(PREFIX)" --enable-static=yes --enable-shared=no\
			--with-iconv=$(GLIBICONV) --disable-gtk-doc-html --disable-xattr --disable-fam\
			--disable-dtrace --disable-gcov --with-pcre=internal
	perl -pi -e 's{(#define GLIB_LOCALE_DIR).+}{$$1 "/usr/share/locale"}' "$(GLIBD)/config.h"
	$(MAKE) -C "$(GLIBD)/glib" install
	$(MAKE) -C "$(GLIBD)/gthread" install
	$(MAKE) -C "$(GLIBD)" install-data
	touch $(GLIBD)/_done

ncdc/_done: $(GLIBD)/_done $(BZD)/_done $(SQLD)/_done $(NCD)/_done $(TLSD)/_done
	mkdir -p ncdc
	cd ncdc && ../../configure --prefix="$(PREFIX)"\
		CPPFLAGS="-I$(PREFIX)/include"\
		LDFLAGS="-L$(PREFIX)/lib"
	$(MAKE) -C ncdc install V=1
	strip bin/ncdc
	touch ncdc/_done

# Rather aggressive clean
clean:
	rm -rf bin lib include share man etc ncdc $(MUSLD) $(NCD) $(PKGD) $(TLSD) $(NETD) $(GMPD) $(ZD) $(SQLD) $(BZD) $(GLIBD)

dist: ncdc/_done
	cd bin && tar -czf ../ncdc-$(HOST)-$(ARCH)-`perl -ne '/#define VERSION "(.+)"/ && print $$1' ../ncdc/config.h`.tar.gz ncdc

# Handy targets to build only one lib
musl: $(MUSLD)/_done
nc: $(NCD)/_done
pkg: $(PKGD)/_done
tls: $(TLSD)/_done
net: $(NETD)/_done
gmp: $(GMPD)/_done
z: $(ZD)/_done
sql: $(SQLD)/_done
bz: $(BZD)/_done
glib: $(GLIBD)/_done
ncdc: ncdc/_done

