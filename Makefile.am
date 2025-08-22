# -*- Makefile -*-
#
#

AM_CPPFLAGS += \
	-I$(srcdir)/inc \
	-I$(srcdir)/inc/util \
	-I$(srcdir)/inc/msl \
	-I$(srcdir)/vcpkg_installed/x64-linux/include \
	$(MPI_CPPFLAGS)

compdir = $(pkglibdir)
comp_LTLIBRARIES = libcscore.la
libcscore_la_SOURCES = \
    cscore.h \
    cscore.cc \
    branch/bimodal/bimodal.cc \
    branch/bimodal/bimodal.h \
    branch/gshare/gshare.cc \
    branch/gshare/gshare.h \
    branch/hashed_perceptron/folded_shift_register.h \
    branch/hashed_perceptron/hashed_perceptron.cc \
    branch/hashed_perceptron/hashed_perceptron.h \
    branch/perceptron/perceptron.cc \
    branch/perceptron/perceptron.h \
    btb/basic_btb/basic_btb.cc \
    btb/basic_btb/basic_btb.h \
    btb/basic_btb/direct_predictor.cc \
    btb/basic_btb/direct_predictor.h \
    btb/basic_btb/.dirstamp \
    btb/basic_btb/indirect_predictor.cc \
    btb/basic_btb/indirect_predictor.h \
    btb/basic_btb/return_stack.cc \
    btb/basic_btb/return_stack.h \
    inc/access_type.h \
    inc/address.h \
    inc/bandwidth.h \
    inc/block.h \
    inc/cache_builder.h \
    inc/cache.h \
    inc/cache_stats.h \
    inc/champsim.h \
    inc/channel.h \
    inc/chrono.h \
    inc/core_builder.h \
    inc/core_stats.h \
    inc/deadlock.h \
    inc/defaults.hpp \
    inc/dram_controller.h \
    inc/dram_stats.h \
    inc/environment.h \
    inc/event_counter.h \
    inc/extent.h \
    inc/extent_set.h \
    inc/inf_stream.h \
    inc/instruction.h \
    inc/modules.h \
    inc/msl/bits.h \
    inc/msl/fwcounter.h \
    inc/msl/lru_table.h \
    inc/ooo_cpu.h \
    inc/operable.h \
    inc/phase_info.h \
    inc/ptw_builder.h \
    inc/ptw.h \
    inc/register_allocator.h \
    inc/repeatable.h \
    inc/SST_CS_packets.h \
    inc/stats_printer.h \
    inc/trace_instruction.h \
    inc/tracereader.h \
    inc/util/algorithm.h \
    inc/util/bit_enum.h \
    inc/util/bits.h \
    inc/util/detect.h \
    inc/util/lru_table.h \
    inc/util/ratio.h \
    inc/util/span.h \
    inc/util/to_underlying.h \
    inc/util/type_traits.h \
    inc/util/units.h \
    inc/vmem.h \
    inc/waitable.h \
    prefetcher/ip_stride/.dirstamp \
    prefetcher/ip_stride/ip_stride.cc \
    prefetcher/ip_stride/ip_stride.h \
    prefetcher/next_line/.dirstamp \
    prefetcher/next_line/next_line.cc \
    prefetcher/next_line/next_line.h \
    prefetcher/no/.dirstamp \
    prefetcher/no/no.cc \
    prefetcher/no/no.h \
    prefetcher/spp_dev/.dirstamp \
    prefetcher/spp_dev/spp_dev.cc \
    prefetcher/spp_dev/spp_dev.h \
    prefetcher/va_ampm_lite/.dirstamp \
    prefetcher/va_ampm_lite/va_ampm_lite.cc \
    prefetcher/va_ampm_lite/va_ampm_lite.h \
    replacement/drrip/.dirstamp \
    replacement/drrip/drrip.cc \
    replacement/drrip/drrip.h \
    replacement/lru/.dirstamp \
    replacement/lru/lru.cc \
    replacement/lru/lru.h \
    replacement/random/.dirstamp \
    replacement/random/random.cc \
    replacement/random/random.h \
    replacement/ship/.dirstamp \
    replacement/ship/ship.cc \
    replacement/ship/ship.h \
    replacement/srrip/.dirstamp \
    replacement/srrip/srrip.cc \
    replacement/srrip/srrip.h \
    src/address.cc \
    src/bandwidth.cc \
    src/cache.cc \
    src/cache_stats.cc \
    src/channel.cc \
    src/chrono.cc \
    src/core_stats.cc \
    src/dram_controller.cc \
    src/dram_stats.cc \
    src/extent.cc \
	src/json_printer.cc \
    src/modules.cc \
    src/ooo_cpu.cc \
    src/operable.cc \
    src/plain_printer.cc \
    src/ptw_builder.cc \
    src/ptw.cc \
    src/register_allocator.cc \
    src/SST_CS_packets.cc \
    src/tracereader.cc \
    src/vmem.cc 
	

EXTRA_DIST = \
    README

libcscore_la_LDFLAGS = -module -avoid-version

install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE     cscore=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS      cscore=$(abs_srcdir)/tests

