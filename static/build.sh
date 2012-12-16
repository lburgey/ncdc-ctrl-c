#!/bin/sh

# This script assumes the following:
# - You have the following installed on your machine:
#   bash
#   make
#   GNU tar
#   wget
#   ncurses (with the 'tic' binary)
#   perl
#   python2
#   git
#   (Anything else I forgot)
# - A checkout of the ncdc git repo can be found in "..", and the configure
#   script exists (i.e. autoreconf has been run).
# - This script is run on an x86 machine
#
# Usage:
#   ./build.sh $arch
#   Where $arch = 'arm', 'mipsel', 'i486' or 'x86_64'
#
# TODO:
# - Cross-compile to platforms other than Linux?


MUSL_VERSION=0.9.8
ZLIB_VERSION=1.2.7
BZIP2_VERSION=1.0.6
SQLITE_VERSION=3071500
GMP_VERSION=5.0.5
NETTLE_VERSION=2.5
GNUTLS_VERSION=3.1.5
NCURSES_VERSION=5.9
GLIB_VERSION=2.34.3


# We don't actually use pkg-config at all. Setting this variable to 'true'
# effectively tricks autoconf scripts into thinking that we have pkg-config
# installed, and that all packages it probes for exist. We handle the rest with
# manual CPPFLAGS/LDFLAGS.
export PKG_CONFIG=true 

export CFLAGS="-O3 -g -static"

# (The variables below are automatically set by the functions, they're defined
# here to make sure they have global scope and for documentation purposes.)

# This is the arch we're compiling for, e.g. arm/mipsel.
TARGET=
# This is the name of the toolchain we're using, and thus the value we should
# pass to autoconf's --host argument.
HOST=
# Installation prefix.
PREFIX=
# Path of the extracted source code of the package we're currently building.
srcdir=

mkdir -p tarballs


# "Fetch, Extract, Move"
fem() { # base-url name targerdir extractdir 
  echo "====== Fetching and extracting $1 $2"
  cd tarballs
  if [ -n "$4" ]; then
    EDIR="$4"
  else
    EDIR=$(basename $(basename $(basename $2 .tar.bz2) .tar.gz) .tar.xz)
  fi
  if [ ! -e "$2" ]; then
    wget "$1$2" || exit
  fi
  if [ ! -d "$3" ]; then
    tar -xvf "$2" || exit
    mv "$EDIR" "$3"
  fi
  cd ..
}


prebuild() { # dirname
  if [ -e "$TARGET/$1/_built" ]; then
    echo "====== Skipping build for $TARGET/$1 (assumed to be done)"
    return 1
  fi
  echo "====== Starting build for $TARGET/$1"
  rm -rf "$TARGET/$1"
  mkdir -p "$TARGET/$1"
  cd "$TARGET/$1"
  srcdir="../../tarballs/$1"
  return 0
}


postbuild() {
  touch _built
  cd ../..
}


# Pre-built cross-compilation binaries for musl. Handy. :)
getmusl() {
  # Order of $HOST is different than the tar/dir names, so we need this case.
  case $TARGET in
    arm)    DIR=arm-linux-musleabi ;;
    mipsel) DIR=mipsel-linux-musl ;;
    i486)   DIR=i486-linux-musl ;;
    x86_64) DIR=x86_64-linux-musl ;;
  esac
  fem https://bitbucket.org/GregorR/musl-cross/downloads/ crossx86-$DIR-$MUSL_VERSION.tar.xz "musl-$TARGET" $DIR
}


getzlib() {
  fem http://zlib.net/ zlib-$ZLIB_VERSION.tar.gz zlib
  prebuild zlib || return
  # zlib doesn't support out-of-source builds
  cp -R $srcdir/* .
  CC=$HOST-gcc ./configure --prefix=$PREFIX --static || exit
  make install || exit
  postbuild
}


getbzip2() {
  fem http://bzip.org/$BZIP2_VERSION/ bzip2-$BZIP2_VERSION.tar.gz bzip2
  prebuild bzip2 || return
  cp -R $srcdir/* .
  make CC=$HOST-gcc AR=$HOST-ar RANLIB=$HOST-ranlib libbz2.a || exit
  mkdir -p $PREFIX/lib $PREFIX/include
  cp libbz2.a $PREFIX/lib
  cp bzlib.h $PREFIX/include
  postbuild
}


getsqlite() {
  fem http://sqlite.org/ sqlite-autoconf-$SQLITE_VERSION.tar.gz sqlite
  prebuild sqlite || return
  $srcdir/configure --prefix=$PREFIX --disable-readline --disable-dynamic-extensions\
    --disable-shared --enable-static --host=$HOST || exit
  make install-includeHEADERS install-libLTLIBRARIES || exit
  postbuild
}


getgmp() {
  fem ftp://ftp.gmplib.org/pub/gmp-$GMP_VERSION/ gmp-$GMP_VERSION.tar.xz gmp
  prebuild gmp || return
  case $TARGET in
    mipsel) ABI=o32 ;;
    i486)   ABI=32 ;;
  esac
  $srcdir/configure --host=$HOST ABI=$ABI --disable-shared --without-readline --enable-static --prefix=$PREFIX || exit
  make install || exit
  postbuild
}


getnettle() {
  fem http://www.lysator.liu.se/~nisse/archive/ nettle-$NETTLE_VERSION.tar.gz nettle
  prebuild nettle || return
  $srcdir/configure --prefix=$PREFIX --disable-shared --host=$HOST\
    CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" || exit
  make install-headers install-static || exit
  postbuild
}


getgnutls() {
  fem http://ftp.gnu.org/gnu/gnutls/ gnutls-$GNUTLS_VERSION.tar.xz gnutls
  prebuild gnutls || return
  # Patch some gnulib stuff to fix building against musl
  if [ ! -e "$srcdir/patched" ]; then
    for i in "$srcdir/gl/"{printf,fprintf,freadahead,closein,fseterr,vfprintf}.c ; do echo ''>$i; done
    echo "#define __printf__ printf" >> "$srcdir/config.h.in"
    echo "#define rpl_fprintf fprintf" >> "$srcdir/config.h.in"
    touch "$srcdir/patched"
  fi
  $srcdir/configure --prefix=$PREFIX --disable-gtk-doc-html --disable-shared --disable-silent-rules\
    --enable-static --disable-cxx --disable-srp-authentication --disable-openssl-compatibility\
    --disable-guile --disable-crywrap --with-included-libtasn1 --without-p11-kit\
    --host=$HOST CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" || exit
  make -C gl install || exit
  make -C lib install || exit
  postbuild
}


getncurses() {
  fem http://ftp.gnu.org/pub/gnu/ncurses/ ncurses-$NCURSES_VERSION.tar.gz ncurses
  prebuild ncurses || return
  $srcdir/configure --prefix=$PREFIX\
    --without-cxx --without-cxx-binding --without-ada --without-manpages --without-progs\
    --without-tests --without-curses-h --without-pkg-config --without-shared --without-debug\
    --without-gpm --without-sysmouse --enable-widec --with-default-terminfo-dir=/usr/share/terminfo\
    --with-terminfo-dirs=/usr/share/terminfo:/lib/terminfo:/usr/local/share/terminfo\
    --with-fallbacks="screen linux vt100 xterm" --host=$HOST\
    CPPFLAGS=-D_GNU_SOURCE || exit
  make || exit
  make install.libs || exit
  postbuild
}


getglib() {
  fem http://ftp.gnome.org/pub/gnome/sources/glib/${GLIB_VERSION%.*}/ glib-$GLIB_VERSION.tar.xz glib
  prebuild glib || return
  export glib_cv_stack_grows=no             # Arch
  export glib_cv_uscore=no                  # OS/Arch
  export glib_cv_have_strlcpy=yes           # libc
  export ac_cv_func_posix_getpwuid_r=yes    # libc
  export ac_cv_func_posix_getgrgid_r=yes    # libc
  export ac_cv_alignof_guint32=4            # Arch, not mentioned in Glib docs...
  export ac_cv_alignof_guint64=8            # Arch, ~
  case $TARGET in                           # Arch, ~
    x86_64) export ac_cv_alignof_unsigned_long=8 ;;
    *)      export ac_cv_alignof_unsigned_long=4 ;;
  esac
  $srcdir/configure --prefix=$PREFIX --enable-static --disable-shared\
    --disable-gtk-doc-html --disable-xattr --disable-fam --disable-dtrace\
    --disable-gcov --with-pcre=internal --disable-silent-rules\
    --host=$HOST CPPFLAGS=-D_GNU_SOURCE || exit
  perl -pi -e 's{(#define GLIB_LOCALE_DIR).+}{$1 "/usr/share/locale"}' config.h
  make -C glib install || exit
  make -C gthread install || exit
  postbuild
}


getncdc() {
  prebuild ncdc || return
  srcdir=../../..
  $srcdir/configure --host=$HOST --disable-silent-rules\
    CPPFLAGS="-I$PREFIX/include -D_GNU_SOURCE" LDFLAGS="-static -L$PREFIX/lib -lz -lbz2"\
    SQLITE_LIBS=-lsqlite3 GNUTLS_LIBS="-lgnutls -lz -lhogweed -lnettle -lgmp"\
    GLIB_LIBS="-pthread -lglib-2.0 -lgthread-2.0"\
    GLIB_CFLAGS="-I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include" || exit
  # Make sure that the Makefile dependencies for makeheaders and gendoc are "up-to-date"
  mkdir -p deps deps/.deps doc doc/.deps
  touch deps/.dirstamp deps/.deps/.dirstamp deps/makeheaders.o doc/.dirstamp doc/.deps/.dirstamp doc/gendoc.o 
  gcc $srcdir/deps/makeheaders.c -o makeheaders || exit
  gcc -I. -I$srcdir $srcdir/doc/gendoc.c -o gendoc || exit
  make || exit

  VER=`cd '../../..' && git describe --abbrev=5 --dirty= | sed s/^v//`
  tar -czf ../../ncdc-linux-$TARGET-$VER-unstripped.tar.gz ncdc
  $HOST-strip ncdc
  tar -czf ../../ncdc-linux-$TARGET-$VER.tar.gz ncdc
  echo "====== ncdc-linux-$TARGET-$VER.tar.gz and -unstripped created."

  postbuild
}


allncdc() {
  getzlib
  getbzip2
  getsqlite
  getgmp
  getnettle
  getgnutls
  getncurses
  getglib
  getncdc
}


buildarch() {
  TARGET=$1
  case $TARGET in
    arm)    HOST=arm-musl-linuxeabi ;;
    mipsel) HOST=mipsel-musl-linux ;;
    i486)   HOST=i486-musl-linux ;;
    x86_64) HOST=x86_64-musl-linux ;;
    *)      echo "Unknown target: $TARGET" ;;
  esac
  PREFIX="`pwd`/$TARGET/inst"
  mkdir -p $TARGET $PREFIX

  getmusl
  MBIN="`pwd`/tarballs/musl-$TARGET/bin"
  OLDPATH="$PATH"
  PATH="$PATH:$MBIN"
  allncdc
  PATH="$OLDPATH"
}


buildarch $1

