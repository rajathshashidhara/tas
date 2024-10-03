-include Makefile.local

##############################################################################
# Compile, link, and install flags

CPPFLAGS += -Iinclude/
CPPFLAGS += $(EXTRA_CPPFLAGS)
CFLAGS += -std=gnu99 -O3 -g -Wall -Werror -Wno-deprecated-declarations -Wno-address-of-packed-member -march=native -fno-omit-frame-pointer
CFLAGS += $(EXTRA_CFLAGS)
CFLAGS_SHARED += $(CFLAGS) -fPIC
LDFLAGS += -pthread -g
LDFLAGS += $(EXTRA_LDFLAGS)
LDLIBS += -lm -lpthread -lrt -ldl
LDLIBS += $(EXTRA_LDLIBS)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin
LIBDIR ?= $(PREFIX)/lib/x86_64-linux-gnu
INCDIR ?= $(PREFIX)/include


##############################################################################
# DPDK configuration

# Prefix for dpdk
RTE_SDK ?= /usr/
# mpdts to compile
DPDK_PMDS ?= ixgbe i40e mlx5 tap virtio

DPDK_CPPFLAGS += -I$(RTE_SDK)/include -I$(RTE_SDK)/include/dpdk \
  -I$(RTE_SDK)/include/x86_64-linux-gnu/dpdk/
DPDK_LDFLAGS+= -L$(RTE_SDK)/lib/x86_64-linux-gnu/
DPDK_LDLIBS+= \
  -Wl,--whole-archive \
  $(addprefix -lrte_pmd_,$(DPDK_PMDS)) \
  -lrte_eal \
  -lrte_mempool \
  -lrte_mempool_ring \
  -lrte_hash \
  -lrte_ring \
  -lrte_kvargs \
  -lrte_meter \
  -lrte_ethdev \
  -lrte_mbuf \
  -lnuma \
  -lrte_bus_pci \
  -lrte_pci \
  -lrte_cmdline \
  -lrte_timer \
  -lrte_net \
  -lrte_kni \
  -lrte_bus_vdev \
  -lrte_gso \
  -Wl,--no-whole-archive \
  -ldl -lbsd \
  -libverbs -lmlx5 \
  $(EXTRA_LIBS_DPDK)


##############################################################################

include mk/recipes.mk

DEPS :=
CLEAN :=
DISTCLEAN :=
TARGETS :=

# Subdirectories

dir := lib
include $(dir)/rules.mk

dir := tas
include $(dir)/rules.mk

dir := tools
include $(dir)/rules.mk

dir := tests
include $(dir)/rules.mk

dir := doc
include $(dir)/rules.mk


##############################################################################
# Top level targets

all: $(TARGETS)

clean:
	rm -rf $(CLEAN) $(DEPS)

distclean:
	rm -rf $(DISTCLEAN) $(CLEAN) $(DEPS)

install: tas/tas lib/libtas_sockets.so lib/libtas_interpose.so \
  lib/libtas.so tools/statetool
	mkdir -p $(DESTDIR)$(BINDIR)
	cp tas/tas $(DESTDIR)$(BINDIR)/tas-network-stack
	cp tools/statetool $(DESTDIR)$(BINDIR)/tas-statetool
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp lib/libtas_interpose.so $(DESTDIR)$(LIBDIR)/libtas_interpose.so
	cp lib/libtas_sockets.so $(DESTDIR)$(LIBDIR)/libtas_sockets.so
	cp lib/libtas.so $(DESTDIR)$(LIBDIR)/libtas.so
	mkdir -p $(DESTDIR)$(INCDIR)
	cp lib/tas/include/tas_ll.h $(DESTDIR)$(INCDIR)/tas_ll.h
	cp lib/tas/include/tas_ll_connect.h $(DESTDIR)$(INCDIR)/tas_ll_connect.h
	cp lib/sockets/include/tas_sockets.h $(DESTDIR)$(INCDIR)/tas_sockets.h

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tas-network-stack
	rm -f $(DESTDIR)$(BINDIR)/tas-statetool
	rm -f $(DESTDIR)$(LIBDIR)/libtas_interpose.so
	rm -f $(DESTDIR)$(LIBDIR)/libtas_sockets.so
	rm -f $(DESTDIR)$(LIBDIR)/libtas.so
	rm -f $(DESTDIR)$(INCDIR)/tas_ll.h
	rm -f $(DESTDIR)$(INCDIR)/tas_ll_connect.h
	rm -f $(DESTDIR)$(INCDIR)/tas_sockets.h


.DEFAULT_GOAL := all
.PHONY: all distclean clean install uninstall

# Include dependencies
-include $(DEPS)
