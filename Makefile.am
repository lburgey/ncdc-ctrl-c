EXTRA_DIST=ChangeLog doc/ncdc.pod.in
CLEANFILES=doc/ncdc.1 doc/ncdc.pod
AM_CFLAGS=$(GLIB_CFLAGS) $(GNUTLS_CFLAGS) $(SQLITE_CFLAGS)
AM_CPPFLAGS=-I$(builddir)/src

# append -DGIT_VERSION, when enabled
if USE_GIT_VERSION
GIT_VERSION := `cd '$(srcdir)' && git describe --abbrev=4 --dirty=-d | sed s/^v//`
AM_CPPFLAGS += -DGIT_VERSION="\"$(GIT_VERSION)\""
endif

man_MANS=doc/ncdc.1

noinst_PROGRAMS=gendoc
gendoc_SOURCES=doc/gendoc.c

doc/ncdc.pod: $(srcdir)/doc/ncdc.pod.in gendoc$(EXEEXT)
	$(AM_V_GEN)./gendoc$(EXEEXT) <"$(srcdir)/doc/ncdc.pod.in" >doc/ncdc.pod

doc/ncdc.1: doc/ncdc.pod Makefile
if USE_POD2MAN
	$(AM_V_GEN)pod2man --center "ncdc manual" --release "@PACKAGE@-@VERSION@" doc/ncdc.pod >doc/ncdc.1
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
	src/bloom.c\
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
	src/strutil.c\
	src/tth.c\
	src/ui.c\
	src/uit_conn.c\
	src/uit_dl.c\
	src/uit_fl.c\
	src/uit_hub.c\
	src/uit_main.c\
	src/uit_msg.c\
	src/uit_search.c\
	src/uit_userlist.c\
	src/ui_util.c\
	src/util.c\
	src/vars.c\
	src/xmlread.c

auto_headers=$(ncdc_SOURCES:.c=.h)
noinst_HEADERS=src/doc.h src/ncdc.h
ncdc_LDADD=-lm $(NCURSES_LIBS) $(Z_LIBS) $(BZ2_LIBS) $(GLIB_LIBS) $(GNUTLS_LIBS) $(SQLITE_LIBS)
MOSTLYCLEANFILES=$(auto_headers) mkhdr.done

$(auto_headers): mkhdr.done
mkhdr.done: $(mkhdr_dep) $(ncdc_SOURCES)
	$(AM_V_GEN)$(mkhdr) `echo $(ncdc_SOURCES) | sed 's#\([^ ]*\)\.c#$(srcdir)/\1.c:$(builddir)/\1.h#g'` && touch mkhdr.done

# Regenerate the header dependencies below, should be run every time
# ncdc_SOURCES is modified.
update-headerdeps:
	cd $(srcdir) &&\
		perl -le 'print "$$_.\$$(OBJEXT): $$_.h" for grep s/\.c//, @ARGV' -- $(ncdc_SOURCES) |\
		perl -e 'open(I, "<Makefile.am") && open (O, ">Makefile.am~") or die $$!; while(<I>) { print O $$_; last if /^# HEADER_DEPS/ }; print O $$_ while(<>);'
	mv $(srcdir)/Makefile.am~ $(srcdir)/Makefile.am


# !! Do not write anything below this line !!
# HEADER_DEPS
src/bloom.$(OBJEXT): src/bloom.h
src/cc.$(OBJEXT): src/cc.h
src/commands.$(OBJEXT): src/commands.h
src/db.$(OBJEXT): src/db.h
src/dl.$(OBJEXT): src/dl.h
src/fl_load.$(OBJEXT): src/fl_load.h
src/fl_local.$(OBJEXT): src/fl_local.h
src/fl_save.$(OBJEXT): src/fl_save.h
src/fl_util.$(OBJEXT): src/fl_util.h
src/hub.$(OBJEXT): src/hub.h
src/listen.$(OBJEXT): src/listen.h
src/main.$(OBJEXT): src/main.h
src/net.$(OBJEXT): src/net.h
src/proto.$(OBJEXT): src/proto.h
src/search.$(OBJEXT): src/search.h
src/strutil.$(OBJEXT): src/strutil.h
src/tth.$(OBJEXT): src/tth.h
src/ui.$(OBJEXT): src/ui.h
src/uit_conn.$(OBJEXT): src/uit_conn.h
src/uit_dl.$(OBJEXT): src/uit_dl.h
src/uit_fl.$(OBJEXT): src/uit_fl.h
src/uit_hub.$(OBJEXT): src/uit_hub.h
src/uit_main.$(OBJEXT): src/uit_main.h
src/uit_msg.$(OBJEXT): src/uit_msg.h
src/uit_search.$(OBJEXT): src/uit_search.h
src/uit_userlist.$(OBJEXT): src/uit_userlist.h
src/ui_util.$(OBJEXT): src/ui_util.h
src/util.$(OBJEXT): src/util.h
src/vars.$(OBJEXT): src/vars.h
src/xmlread.$(OBJEXT): src/xmlread.h
