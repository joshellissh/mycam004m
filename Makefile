# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_VIDEO_MYCAM004M) += mycam004m.o
obj-$(CONFIG_VIDEO_MYCAM004M_FAKE) += mycam004m-fake.o

# CONFIG_VIDEO_MYCAM004M[_FAKE] only exist if this driver's Kconfig has
# been sourced into the target kernel's Kconfig (the "drop into
# drivers/media/i2c/" in-tree scenario). For a standalone external
# module build (M=<this dir> against an unmodified kernel tree, the
# common case), those symbols are never set, so the obj-$(CONFIG_...)
# lines above silently expand to nothing and the build "succeeds"
# having compiled zero files. Force both modules in that case instead.
ifeq ($(CONFIG_VIDEO_MYCAM004M),)
obj-m += mycam004m.o
endif
ifeq ($(CONFIG_VIDEO_MYCAM004M_FAKE),)
obj-m += mycam004m-fake.o
endif

# Everything below only matters for a standalone (external module) build
# invoked directly in this directory, e.g. `make` or `make KDIR=...`.
# When this Makefile is reached via Kbuild instead (either M=<this dir>
# from a real kernel build, or because the directory was dropped into a
# kernel tree and built in-tree), $(KERNELRELEASE) is already set and
# only the obj-y line above is used.
ifeq ($(KERNELRELEASE),)

# Point this at a configured kernel tree (a `build` output directory
# with Module.symvers / .config / scripts already present, not bare
# source) before building. Defaults to the running kernel's own tree,
# which is almost never right for a cross-compiled board target like
# BeaglePlay -- override it, e.g.:
#
#   make KDIR=/path/to/kernel-build ARCH=arm64 \
#        CROSS_COMPILE=aarch64-linux-gnu-
#
# See README.md for how to point KDIR at this project's actual
# BeaglePlay Yocto kernel build output.
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all modules modules_install clean

all modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
