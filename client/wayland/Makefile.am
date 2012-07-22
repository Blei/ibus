# vim:set noet ts=4:
#
# ibus - The Input Bus
#
# Copyright (c) 2007-2010 Peng Huang <shawn.p.huang@gmail.com>
# Copyright (c) 2007-2010 Red Hat, Inc.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this program; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place, Suite 330,
# Boston, MA  02111-1307  USA

libibus = $(top_builddir)/src/libibus-@IBUS_API_VERSION@.la

weston_dir = /home/philipp/programming/weston
weston_client_ldadd = \
	$(weston_dir)/clients/libtoytoolkit.a \
	$(weston_dir)/shared/libshared.la \
	$(NULL)

bin_PROGRAMS = ibus-wayland

ibus_wayland_SOURCES = \
	main.c \
	$(NULL)

ibus_wayland_DEPENDENCIES = \
	$(libibus) \
	$(NULL)

ibus_wayland_LDADD = \
	$(libibus) \
	@GLIB2_LIBS@ \
	@CAIRO_LIBS@ \
	@WAYLAND_LIBS@ \
	$(weston_client_ldadd) \
	-lxkbcommon -ljpeg -lEGL -lm \
	$(weston_dir)/clients/text-protocol.o \
	$(NULL)

ibus_wayland_CFLAGS = \
	@GLIB2_CFLAGS@ \
	@CAIRO_CFLAGS@ \
	@DBUS_CFLAGS@ \
	@WAYLAND_CFLAGS@ \
	-I$(weston_dir)/clients \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src \
	$(NULL)

noinst_HEADERS = \
	locales.h \
	$(NULL)

$(libibus):
	(cd $(top_builddir)/src; make)

locales.h:
	( \
		locale -a | \
		gawk -F _ '{ if (length($$1) <= 3) print $$1 }' | \
		uniq | sort | \
		xargs python2 -c 'import sys;print "#define LOCALES_STRING \"%s\"" % ",".join(sys.argv[1:])' \
	 ) > $@

-include $(top_srcdir)/git.mk
