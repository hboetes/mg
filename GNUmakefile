# Makefile for mg

# This Makefile has been written by Han Boetes
# <hboetes@gmail.com> and is released in Public Domain.

# *sigh* Those debian folks are really tidy on their licenses.

name=		mg

prefix=		/usr/local
bindir=		$(prefix)/bin
libdir=		$(prefix)/lib
includedir=	$(prefix)/include
mandir=		$(prefix)/man

PKG_CONFIG=	/usr/bin/pkg-config --silence-errors
INSTALL=	/usr/bin/install
STRIP=		/usr/bin/strip

UNAME:=		$(shell uname)
ifeq ($(UNAME),FreeBSD)
  BSD_CPPFLAGS:= -DHAVE_UTIL_H
  BSD_LIBS:=	 -lutil
else
  BSD_CPPFLAGS:= $(shell $(PKG_CONFIG) --cflags libbsd-overlay) -DHAVE_PTY_H
  BSD_LIBS:=	 $(shell $(PKG_CONFIG) --libs libbsd-overlay) -lutil
endif

# Test if required libraries are installed. Rather bummer that they
# are also required to run make clean or uninstall. Oh well... Who
# does that?
ifeq ($(BSD_LIBS),)
  $(error You probably need to install "libbsd-dev" or "libbsd-devel" or something like that.)
endif

CURSES_LIBS:= $(shell $(PKG_CONFIG) --libs ncurses)
ifeq ($(CURSES_LIBS),)
  $(error You probably need to install "libncurses5-dev" or "libncurses6-devel" or something like that.)
endif

ifdef STATIC
  LDFLAGS=-static -static-libgcc
endif

CC?=		gcc
CFLAGS?=	-O2 -pipe
CFLAGS+=	-g -Wall
CPPFLAGS=	-DREGEX
CPPFLAGS+=	-D_GNU_SOURCE
CPPFLAGS+=	$(BSD_CPPFLAGS)
LIBS=		$(CURSES_LIBS) $(BSD_LIBS)


OBJS=	autoexec.o basic.o bell.o buffer.o cinfo.o dir.o display.o \
	echo.o extend.o file.o fileio.o funmap.o interpreter.o help.o \
	kbd.o keymap.o line.o macro.o main.o match.o modes.o paragraph.o \
	re_search.o region.o search.o spawn.o tty.o ttyio.o ttykbd.o \
	undo.o util.o version.o window.o word.o yank.o
OBJS+=	cmode.o cscope.o dired.o grep.o tags.o


# Portability stuff.
CFLAGS+= 	 -Wno-strict-aliasing -Wno-deprecated-declarations
EXE_EXT=

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

all: $(name)

$(name): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $(name) $(LIBS)

distclean: clean
	-rm -f *.core core.* .#*

clean:
	-rm -f *.o $(name)$(EXE_EXT)


install: $(name) $(name).1
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) -d $(DESTDIR)$(mandir)/man1
	$(INSTALL) -m 755 $(name)		$(DESTDIR)$(bindir)/$(name)
	$(INSTALL) -m 444 $(name).1		$(DESTDIR)$(mandir)/man1/$(name).1

install-strip: install
	$(STRIP) $(DESTDIR)$(bindir)/$(name)

uninstall:
	rm -f \
	$(DESTDIR)$(bindir)/$(name)$(EXE_EXT) \
	$(DESTDIR)$(mandir)/man1/$(name).1

rebuild:
	make clean all
