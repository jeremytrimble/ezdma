#
# Out-of-tree Makefile for building ezdma driver.
#
# This Makefile assumes that you've defined the "KERNELDIR" variable to point
# to the top-level directory of a linux source tree which you'd like to build
# ezdma against.  You can specify KERNELDIR either on the make command line, or
# in your environment, or optionally uncomment the KERNELDIR?= line below and
# set the default value within this file.
#
# This Makefile is parsed twice: 
#
#  - In the first pass, it make -C's into your KERNELDIR.
#
#  - In the second pass, it sets up make variables that cause the ezdma module
#  to be built against the kernel at your KERNELDIR.
#
#

# Cross-compile for ARM by default.
ARCH?=arm
CROSS_COMPILE?=arm-linux-gnueabihf-

# If desired, you can specify a default KERNELDIR by uncommenting the line
# below and placing your desired directory after the ?=
#KERNELDIR?=

LC_ALL?=C

export LC_ALL ARCH CROSS_COMPILE

ifeq ($(KERNELRELEASE),)
# First Pass

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf -- *~ *.ko *.o *.mod.c modules.order Module.symvers .adc* .tmp_versions .*.tmp .*.cmd

else
# Second Pass

obj-m += ezdma.o

endif
