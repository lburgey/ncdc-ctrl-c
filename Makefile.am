EXTRA_DIST=ChangeLog doc/ncdc-db-upgrade.pod doc/ncdc.pod.in
CLEANFILES=doc/ncdc.1 doc/ncdc-db-upgrade.1 doc/ncdc.pod

man_MANS=doc/ncdc.1
if ENABLE_DB_UPGRADE
man_MANS+=doc/ncdc-db-upgrade.1
endif

noinst_PROGRAMS=gendoc
gendoc_SOURCES=doc/gendoc.c

doc/ncdc.pod: $(srcdir)/doc/ncdc.pod.in gendoc$(EXEEXT)
	$(AM_V_GEN)./gendoc$(EXEEXT) <"$(srcdir)/doc/ncdc.pod.in" >doc/ncdc.pod

doc/ncdc.1: doc/ncdc.pod Makefile
if USE_POD2MAN
	$(AM_V_GEN)pod2man --center "ncdc manual" --release "@PACKAGE@-@VERSION@" doc/ncdc.pod >doc/ncdc.1
endif

doc/ncdc-db-upgrade.1: $(srcdir)/doc/ncdc-db-upgrade.pod Makefile
if USE_POD2MAN
	$(AM_V_GEN)pod2man --center "ncdc manual" --release "@PACKAGE@-@VERSION@" "$(srcdir)/doc/ncdc-db-upgrade.pod" >doc/ncdc-db-upgrade.1
endif



if HAVE_MH
mkhdr=makeheaders
mkhdr_dep=
else
mkhdr=./makeheaders$(EXEEXT)
mkhdr_dep=makeheaders$(EXEEXT)
noinst_PROGRAMS+=makeheaders
endif
makeheaders_SOURCES=deps/makeheaders.c



bin_PROGRAMS=ncdc
ncdc_SOURCES=\
	src/cc.c\
	src/commands.c\
	src/db.c\
	src/dl.c\
	src/fl_load.c\
	src/fl_local.c\
	src/fl_save.c\
	src/fl_util.c\
	src/hub.c\
	src/listen.c\
	src/main.c\
	src/net.c\
	src/proto.c\
	src/search.c\
	src/tth.c\
	src/ui.c\
	src/uit_main.c\
	src/ui_util.c\
	src/util.c\
	src/vars.c\
	src/xmlread.c

auto_headers=$(ncdc_SOURCES:.c=.h)
noinst_HEADERS=src/doc.h src/ncdc.h
ncdc_CPPFLAGS=-I$(top_builddir)/src
ncdc_CFLAGS=$(GLIB_CFLAGS) $(GNUTLS_CFLAGS) $(SQLITE_CFLAGS)
ncdc_LDADD=-lm $(NCURSES_LIBS) $(BZ2_LIBS) $(GLIB_LIBS) $(GNUTLS_LIBS) $(SQLITE_LIBS)
MOSTLYCLEANFILES=$(auto_headers) mkhdr.done

mkhdr.done: $(mkhdr_dep) ${ncdc_SOURCES}
	$(AM_V_GEN)$(mkhdr) `echo $(ncdc_SOURCES) | sed 's#\([^ ]*\)\.c#$(srcdir)/\1.c:$(builddir)/\1.h#g'` && touch mkhdr.done

${auto_headers}: mkhdr.done
${ncdc_OBJECTS}: ${auto_headers}


# append -DGIT_VERSION, when enabled
if USE_GIT_VERSION
GIT_VERSION := `cd '$(srcdir)' && git describe --abbrev=4 --dirty=-d | sed s/^v//`
ncdc_CFLAGS += -DGIT_VERSION="\"$(GIT_VERSION)\""
endif





if ENABLE_DB_UPGRADE
bin_PROGRAMS+=ncdc-db-upgrade
endif

ncdc_db_upgrade_SOURCES=util/ncdc-db-upgrade.c
ncdc_db_upgrade_CFLAGS=$(SQLITE_CFLAGS) $(GLIB_CFLAGS) $(LIBXML_CFLAGS)
ncdc_db_upgrade_LDADD=$(SQLITE_LIBS) $(GDBM_LIBS) $(GLIB_LIBS) $(LIBXML_LIBS) $(BZ2_LIBS)

