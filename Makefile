# GtkACE - a GTK 3 client for Wolfpack Empire
#
# Needs: a C compiler, make, pkg-config, GTK 3 and SQLite 3 development
# files.  On Debian/Ubuntu:
#   sudo apt install build-essential pkg-config libgtk-3-dev libsqlite3-dev

CC ?= cc
CFLAGS ?= -O2 -g
PKG_CONFIG ?= pkg-config
INSTALL ?= install
PREFIX ?= /usr/local
DESTDIR ?=

PKGS = gtk+-3.0 sqlite3
DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
DEP_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS) 2>/dev/null)

ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(strip $(DEP_LIBS)),)
$(error GTK 3 or SQLite 3 not found - install libgtk-3-dev and libsqlite3-dev)
endif
endif

WARNFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

ALL_CFLAGS = $(CFLAGS) $(WARNFLAGS) $(DEP_CFLAGS) -MMD -MP

OBJS = main.o mainwin.o games.o conn.o db.o world.o mapview.o units.o intel.o dialogs.o telegrams.o

all: gtkace

gtkace: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(DEP_LIBS) -lm

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

install: gtkace
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin \
		$(DESTDIR)$(PREFIX)/share/applications
	$(INSTALL) -m 755 gtkace $(DESTDIR)$(PREFIX)/bin/gtkace
	$(INSTALL) -m 644 gtkace.desktop $(DESTDIR)$(PREFIX)/share/applications/

clean:
	rm -f gtkace *.o *.d

.PHONY: all install clean

-include $(OBJS:.o=.d)
