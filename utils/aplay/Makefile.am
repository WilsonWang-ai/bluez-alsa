# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

if ENABLE_APLAY

bin_PROGRAMS = bluealsa-aplay

bluealsa_aplay_SOURCES = \
	$(top_srcdir)/src/shared/dbus-client.c \
	$(top_srcdir)/src/shared/ffb.c \
	$(top_srcdir)/src/shared/log.c \
	alsa-pcm.c \
	aplay.c

bluealsa_aplay_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@

bluealsa_aplay_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@

endif
