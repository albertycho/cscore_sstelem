# -*- Makefile -*-
#
#

AM_CPPFLAGS += \
	-I$(srcdir)/inc \
	-I$(srcdir)/inc/util \
	-I$(srcdir)/inc/msl \
	-I$(srcdir)/vcpkg/installed/x64-linux/include/ \
	$(MPI_CPPFLAGS)

compdir = $(pkglibdir)
comp_LTLIBRARIES = libcscore.la
libcscore_la_SOURCES = \
	cscore.h \
	cscore.cc \
	inc/trace_instruction.h \
	inc/instruction.h \
	inc/champsim.h \
	inc/channel.h \
	inc/cache_builder.h \
	inc/cache.h \
	src/address.cc \
	src/bandwidth.cc \
	src/cache_stats.cc \
	src/cache.cc \
	src/channel.cc \
	src/chrono.cc \
	src/extent.cc \
	src/modules.cc \
	src/operable.cc
	

EXTRA_DIST = \
    README

libcscore_la_LDFLAGS = -module -avoid-version

install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE     cscore=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS      cscore=$(abs_srcdir)/tests

