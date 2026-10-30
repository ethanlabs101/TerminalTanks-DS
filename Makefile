# Terminal Tanks DS -- BlocksDS build (see docs/BUILDING.md)
#   export WONDERFUL_TOOLCHAIN=/opt/wonderful BLOCKSDS=/path/to/blocksds   (scripts/setup_toolchain.sh)
#   make            -> terminaltanks.nds
#   make host-tests -> builds the host parity probe

BLOCKSDS            ?= /opt/blocksds/core
WONDERFUL_TOOLCHAIN ?= /opt/wonderful
ARM_NONE_EABI_PATH  ?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

NAME        := terminaltanks
GAME_TITLE  := Terminal Tanks DS
GAME_SUB    := v1.5.0 port
GAME_AUTHOR := ethanlabs101
GAME_ICON   := assets/icon.bmp

VERSION     := $(shell git describe --always --dirty 2>/dev/null || echo unknown)

PREFIX  := $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC      := $(PREFIX)gcc
LD      := $(PREFIX)gcc

ARM7ELF := $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS    := -ldswifi9_noip -lnds9 -lc -lgcc -lm
LIBDIRS := $(BLOCKSDS)/libs/dswifi $(BLOCKSDS)/libs/libnds
SPECS   := $(BLOCKSDS)/sys/crts/ds_arm9.specs

BUILDDIR := build/arm9
ELF      := build/$(NAME).elf
MAP      := build/$(NAME).map
ROM      := $(NAME).nds

SOURCES := $(wildcard core/*.c) core/gen/content_data.c core/gen/legacy_v1.c $(wildcard source/*.c) $(wildcard source/gen/*.c)
OBJS    := $(addprefix $(BUILDDIR)/,$(SOURCES:.c=.o))
DEPS    := $(OBJS:.o=.d)

# The simulation is compiled as ARM code (faster soft-float) and with -ffp-contract=off so it stays bit-identical
# to the host build that the parity tests check against Python.
ARCH     := -mcpu=arm946e-s+nofp
INCFLAGS := $(foreach p,$(LIBDIRS),-I$(p)/include) -Icore -Isource
CFLAGS   := $(EXTRA_CFLAGS) -DTT_XMATH -std=gnu11 -Wall -Wno-unused-function -Wno-format-truncation -O2 -ffp-contract=off -ffunction-sections -fdata-sections \
            $(ARCH) $(INCFLAGS) -DTT_VERSION=\"$(VERSION)\" -specs=$(SPECS)
LDFLAGS  := $(ARCH) $(foreach p,$(LIBDIRS),-L$(p)/lib) -Wl,-Map,$(MAP) -Wl,--gc-sections -specs=$(SPECS)

THUMB_SRC := source/gen/font_data.c core/gen/content_data.c

.PHONY: all clean host-tests selftest

all: $(ROM)

$(ROM): $(ELF) $(GAME_ICON)
	@echo "  NDSTOOL $@"
	@$(BLOCKSDS)/tools/ndstool/ndstool -c $@ -7 $(ARM7ELF) -9 $(ELF) -b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUB);$(GAME_AUTHOR)" >/dev/null

$(ELF): $(OBJS)
	@echo "  LD      $@"
	@$(LD) -o $@ $(OBJS) $(LDFLAGS) -Wl,--start-group $(LIBS) -Wl,--end-group

$(BUILDDIR)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) $(if $(filter $<,$(THUMB_SRC)),-mthumb,-marm) -MMD -MP -c -o $@ $<

host-tests:
	@mkdir -p build/host
	gcc -std=c11 -O2 -ffp-contract=off -Wno-format-truncation -o build/host/probe tests/parity/probe.c core/*.c core/gen/content_data.c core/gen/legacy_v1.c -lm
	gcc -std=c11 -O2 -ffp-contract=off -Wno-format-truncation -DTT_XMATH -o build/host/probe_xmath tests/parity/probe.c core/*.c core/gen/content_data.c core/gen/legacy_v1.c -lm

# DS self-test ROM: the parity probe on the ARM9 (tests/ds_selftest; generate cases.h with gen_cases.py first)
ST_SOURCES := $(wildcard core/*.c) core/gen/content_data.c core/gen/legacy_v1.c tests/ds_selftest/main.c
ST_OBJS    := $(addprefix build/st/,$(ST_SOURCES:.c=.o))
selftest: build/selftest.nds
build/selftest.nds: $(ST_OBJS)
	@$(LD) -o build/selftest.elf $(ST_OBJS) $(LDFLAGS) -Wl,--start-group $(LIBS) -Wl,--end-group
	@$(BLOCKSDS)/tools/ndstool/ndstool -c $@ -7 $(ARM7ELF) -9 build/selftest.elf >/dev/null
build/st/%.o: %.c
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) $(if $(filter $<,$(THUMB_SRC)),-mthumb,-marm) -MMD -MP -c -o $@ $<
build/st/tests/ds_selftest/main.o: tests/ds_selftest/cases.h tests/parity/probe.c

clean:
	rm -rf build/arm9 build/st $(ELF) $(MAP) $(ROM) build/selftest.nds build/selftest.elf

-include $(DEPS)
