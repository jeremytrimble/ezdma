#
# Out-of-tree Makefile for building ezdma driver.
#
# This Makefile assumes that you've defined the "KERNEL_SRC" variable to point
# to the top-level directory of a linux source tree which you'd like to build
# ezdma against. You can specify KERNEL_SRC either on the make command line, or
# in your environment.
#

obj-m += ezdma.o

HOST_ARCH ?= $(shell uname -m | sed -e s/arm.*/arm/ -e s/aarch64.*/arm64/)
ARCH      ?= $(shell uname -m | sed -e s/arm.*/arm/ -e s/aarch64.*/arm64/)

ifeq ($(ARCH), arm)
    ifneq ($(HOST_ARCH), arm)
        CROSS_COMPILE ?= arm-linux-gnueabihf-
    endif
endif
ifeq ($(ARCH), arm64)
    ifneq ($(HOST_ARCH), arm64)
        CROSS_COMPILE ?= aarch64-linux-gnu-
    endif
endif

ifdef KERNEL_SRC
    KERNEL_SRC_DIR := $(KERNEL_SRC)
else
    KERNEL_SRC_DIR ?= /lib/modules/$(shell uname -r)/build
endif

#
# For out of kernel tree rules
#
all:
	$(MAKE) -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean
