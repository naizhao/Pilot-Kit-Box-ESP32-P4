# CC1312R 固件骨架构建（WP-E T4）
#
# 跑法：
#   export SIMPLELINK_SDK_ROOT=~/ti/simplelink_cc13xx_cc26xx_sdk_8_33_00_16
#   cd firmware/cc1312r && make
SDK   ?= $(SIMPLELINK_SDK_ROOT)
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

DEV   := cc13x2_cc26x2
DEVP  := $(SDK)/source/ti/devices/$(DEV)

CFLAGS := -std=c11 -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
          -O2 -g3 -Wall -Wextra \
          -DDeviceFamily_CC13X2 \
          -DCC13_FW_VER_MAJOR=0 -DCC13_FW_VER_MINOR=1 \
          -I. -I../components/rp_cc13xx_codec -I../components/adsb_link_codec \
          -I$(DEVP)/inc -I$(SDK)/source

LDFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
           -T $(DEVP)/linker_files/cc13x2r1f3.lds \
           --specs=nosys.specs -Wl,--gc-sections

SRCS_C := main.c spi_slave.c \
          ../components/rp_cc13xx_codec/rp_cc13xx_codec.c \
          ../components/adsb_link_codec/adsb_link_codec.c \
          $(DEVP)/startup_files/startup_gcc.c \
          $(DEVP)/startup_files/ccfg.c
OBJS := $(patsubst %.c,build/%.o,$(notdir $(SRCS_C)))
VPATH := . ../components/rp_cc13xx_codec ../components/adsb_link_codec \
         $(DEVP)/startup_files

all: build/adsb978_cc13.bin

build:
	mkdir -p build

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/adsb978_cc13.elf: $(OBJS) $(DEVP)/driverlib/bin/gcc/driverlib.lib
	$(CC) $(LDFLAGS) $(OBJS) $(DEVP)/driverlib/bin/gcc/driverlib.lib -o $@

build/adsb978_cc13.bin: build/adsb978_cc13.elf
	$(OBJCOPY) -O binary $< $@
	@shasum -a 256 $@ | cut -c1-16

clean:
	rm -rf build

.PHONY: all clean
