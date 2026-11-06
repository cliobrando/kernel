BUSIF=SDIO
MY_PWD=$(PWD)
ROM=true
CHIP=RK915

ifeq ($(CONFIG_RK915),)
CONFIG_RK915=m
endif

L_BUSIF=$(shell echo $(BUSIF) | tr A-Z a-z)

NAME     = rk915

# $(src) resolves differently depending on how kbuild invokes this Makefile
# (in-tree, in-tree with M=, or a standalone ext build); using the absolute
# path of this Makefile works for all of them.
RK915_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

ccflags-y += -I$(RK915_DIR)/inc \
                -I$(RK915_DIR)/shared

ccflags-y += -DDEBUG

OBJS        =   src/main.o \
		src/hal.o \
		src/umac_if.o \
		src/rpu_if.o \
		src/tx.o \
		src/rx.o \
		src/beacon.o \
		src/p2p.o \
		src/pktgen.o \
		src/procfs.o \
		src/utils.o \
		src/vif.o \
		src/wow.o \
		src/soc.o \
		src/hal_io.o \
		src/platform.o \
		src/firmware.o \
		src/init.o

## To enable sleep functionality
## we need to enable ROM
ifeq ($(ROM),true)
ccflags-y += -DRPU_SLEEP_ENABLE
#ccflags-y += -DPS_SLEEP_TEST
#ccflags-y += -DRPU_NO_SLEEP_FLAG
#ccflags-y += -DRPU_ENABLE_PS
else
ccflags-y += -DNO_HP_READY_WAR
endif
# BUSIF
ifeq ($(BUSIF),SDIO)
ccflags-y += -DHAL_SDIO
OBJS	   +=	src/sdio.o
endif

#ccflags-y += -DSKIP_DL_FW
ifeq ($(CHIP),RK915)
ccflags-y += -DRK915
endif

#ccflags-y += -DRK3036_DONGLE
#ccflags-y += -DSTA_AP_COEXIST
ccflags-y += -DSDIO_CLOCK_SWITCH
ccflags-y += -Wno-array-bounds -Wno-error=array-bounds

# The kernel builds with -Werror=date-time and requires gcc >= 4.9
ccflags-y += -Wno-date-time

obj-$(CONFIG_RK915) += $(NAME).o
$(NAME)-objs= $(OBJS)

ifneq ($(KERNELRELEASE),)

#EXTRA_LDFLAGS += --strip-debug

else

all:
	@echo "BUSIF is $(L_BUSIF)"
	@echo "Compiling for $(BUSIF)"
	@make -C $(KROOT) M=$(MY_PWD) modules
clean:
	@make -C $(KROOT) M=$(MY_PWD) clean
	rm -f src/*.o

endif

