# -*- Makefile -*-
#
#

AM_CPPFLAGS += \
	$(MPI_CPPFLAGS)

compdir = $(pkglibdir)
comp_LTLIBRARIES = libcscore.la
libcscore_la_SOURCES = \
	cscore.h \
	cscore.cc \
	inc/trace_instruction.h \
	inc/instruction.h

EXTRA_DIST = \
    README

libcscore_la_LDFLAGS = -module -avoid-version

install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE     cscore=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS      cscore=$(abs_srcdir)/tests

