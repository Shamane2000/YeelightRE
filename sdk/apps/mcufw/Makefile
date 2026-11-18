# mcufw/Makefile -- compile + link the minimal firmware and pack the MRVL container.
#
# Phase 3.4 adds the network layer:
#   mcufw_wlan.c     mlan/mdev link boundary + bring-up state machine
#   mcufw_miio.c     miIO-compatible UDP channel (port 54321)
#   mcufw_crypto.c   own MD5 + AES-128-CBC (no vendor context layouts)
#   mcufw_selftest.c known-answer tests, shared with the host test
#   mcufw_shim.c     link shims for the FreeRTOS/lwIP surface (inert by design)

ROOT      := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
SDK_DIR   := $(abspath $(ROOT)/../../vendor/ez-connect-lite)
AXF2FW    := $(SDK_DIR)/sdk/tools/bin/Linux/axf2firmware
BUILD     := $(ROOT)/build
SRC       := $(ROOT)/src

CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
AR        := $(CROSS)ar
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size
READELF   := $(CROSS)readelf
NM        := $(CROSS)nm

# Vendor SDK archive: mcufw pulls GPIO_PinMuxFun/GPIO_PinModeConfig from
# mw300_pinmux.o (phase 3.2) and now the whole mlan/mdev init graph
# (mlan_init.o, mdev_startup.o) so the WLAN stack is really in the image.
# The archive also ships ttcp.o with its own main(), so it is linked as a plain
# archive (members pulled on demand), never --whole-archive.
VENDOR_LIB := $(SDK_DIR)/sdk/libs/libwmsdk.a

# mdev_startup.o also defines Reset_IRQHandler / Default_IRQHandler /
# HardFault_IRQHandler.  mcufw owns the reset vector (startup.c, deterministic
# .data/.bss init proven in 3.1/3.2), so the vendor copies are renamed inside a
# build-local copy of the archive instead of relaxing the linker with
# --allow-multiple-definition.  Everything else in the member stays verbatim,
# and CLK_Init/boot_init keep their names.
VENDOR_STAGE := $(BUILD)/vendor
PATCHED_LIB  := $(BUILD)/libwmsdk_mcufw.a
RENAMES      := Reset_IRQHandler Default_IRQHandler HardFault_IRQHandler

LDSCRIPT  := $(ROOT)/mw300_min.ld
TARGET    := $(BUILD)/mcufw
ELF       := $(TARGET).elf
CONTAINER := $(BUILD)/mcufw_MRVL.bin

# -ffreestanding + -fno-unwind-tables: no host libc, no .ARM.exidx bloat.
# -fno-common: gcc>=10 default, stated explicitly for older toolchains.
# -ffile-prefix-map: -g records the build directory in DW_AT_comp_dir; mapping the
# repo root to a fixed string makes the ELF byte-identical across clone paths
# (proven by building the same tree in two directories).
REPO_ROOT := $(abspath $(ROOT)/../../..)
CFLAGS    := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -mabi=aapcs \
             -O2 -g -std=c11 -Wall -Wextra -Werror \
             -ffreestanding -fno-common -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fno-ident \
             -ffile-prefix-map=$(REPO_ROOT)=/yeelight-re \
             -I$(SRC)
LDFLAGS   := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -mabi=aapcs \
             -nostdlib -nostartfiles -Wl,--build-id=none \
             -Wl,-Map=$(TARGET).map -T$(LDSCRIPT)

SRCS      := $(SRC)/startup.c $(SRC)/main.c $(SRC)/mcufw_gpt.c \
             $(SRC)/mcufw_crypto.c $(SRC)/mcufw_miio.c $(SRC)/mcufw_wlan.c \
             $(SRC)/mcufw_selftest.c $(SRC)/mcufw_shim.c

OBJS      := $(BUILD)/startup.o $(BUILD)/main.o $(BUILD)/mcufw_gpt.o \
             $(BUILD)/mcufw_crypto.o $(BUILD)/mcufw_miio.o \
             $(BUILD)/mcufw_wlan.o $(BUILD)/mcufw_selftest.o \
             $(BUILD)/mcufw_shim.o

.PHONY: all elf container check-vendor inspect clean host-test

all: container

elf: $(ELF)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(PATCHED_LIB): $(VENDOR_LIB) | $(BUILD)
	@rm -rf $(VENDOR_STAGE) && mkdir -p $(VENDOR_STAGE)
	cd $(VENDOR_STAGE) && $(AR) x $(VENDOR_LIB)
	cd $(VENDOR_STAGE) && $(OBJCOPY) \
		--redefine-sym Reset_IRQHandler=mdev_Reset_IRQHandler \
		--redefine-sym Default_IRQHandler=mdev_Default_IRQHandler \
		--redefine-sym HardFault_IRQHandler=mdev_HardFault_IRQHandler \
		mdev_startup.o mdev_startup.patched.o \
		&& mv mdev_startup.patched.o mdev_startup.o
	@rm -f $(PATCHED_LIB)
	cd $(VENDOR_STAGE) && $(AR) rcs $(PATCHED_LIB) *.o
	@echo "== vendor archive patched: $(RENAMES) -> mdev_* in mdev_startup.o"

$(ELF): $(OBJS) $(PATCHED_LIB) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) $(PATCHED_LIB) -lgcc -o $@
	@echo "== elf layout"
	@$(READELF) -h $(ELF) | grep -E 'Entry point|Machine|Flags'
	@$(READELF) -lW $(ELF) | grep -E '^  LOAD|Type' | sed 's/^/   /'
	@$(SIZE) -A $(ELF) | grep -E '^\.(init|text|data|bss)'

# Hard proof that the vendor archive is really in the image: the pinmux
# accessor must be defined (T), the WLAN init graph must be defined, and the
# archive member must not drag in a second main().
check-vendor: $(ELF)
	@$(NM) $(ELF) | grep -q "T GPIO_PinMuxFun" \
		|| { echo "FEHLER: GPIO_PinMuxFun aus libwmsdk.a nicht gelinkt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T GPIO_PinModeConfig" \
		|| { echo "FEHLER: GPIO_PinModeConfig aus libwmsdk.a nicht gelinkt"; exit 1; }
	@test "$$($(NM) $(ELF) | grep -c "^[0-9a-f]* T main$$")" = "1" \
		|| { echo "FEHLER: mehrdeutige main()-Definition im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_gpt0_pwm_init" || { echo "FEHLER: GPT0-PWM-Treiber (mcufw_gpt.c) nicht im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_lamp_set" || { echo "FEHLER: mcufw_lamp_set fehlt im Image"; exit 1; }
	@echo "== lamp pwm: $$($(NM) $(ELF) | grep -c " T mcufw_lamp_") Symbole (GPT0 CH0/1/4/5)"
	@echo "== vendor link: $$($(NM) $(ELF) | grep -c " T GPIO_") Symbole aus $(notdir $(VENDOR_LIB))"

# Phase 3.4 acceptance: WLAN stack linked, miIO channel linked, zero undefined.
check-net: $(ELF)
	@for sym in wlan_init_lock_list wlan_init_adapter wlan_init_priv wlan_init_fw wlan_allocate_adapter; do \
		$(NM) $(ELF) | grep -q " T $$sym$$" \
			|| { echo "FEHLER: $$sym aus libwmsdk.a(mlan_init.o) nicht gelinkt"; exit 1; }; \
	done
	@for sym in CLK_Init boot_init; do \
		$(NM) $(ELF) | grep -q " T $$sym$$" \
			|| { echo "FEHLER: $$sym aus libwmsdk.a(mdev_startup.o) nicht gelinkt"; exit 1; }; \
	done
	@$(NM) $(ELF) | grep -q " T mcufw_miio_handle" || { echo "FEHLER: miIO-Listener (mcufw_miio.c) nicht im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q " T mcufw_wlan_bringup" || { echo "FEHLER: mcufw_wlan_bringup fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q " T mcufw_selftest" || { echo "FEHLER: mcufw_selftest fehlt im Image"; exit 1; }
	@undef=$$($(NM) -u $(ELF) | wc -l); test "$$undef" = "0" \
		|| { echo "FEHLER: $$undef undefinierte Symbole im ELF:"; $(NM) -u $(ELF); exit 1; }
	@echo "== wlan link: $$($(NM) $(ELF) | grep -cE " T (wlan_|CLK_Init|boot_init)") Symbole aus mlan_init.o/mdev_startup.o"
	@echo "== wlan syms: $$($(NM) $(ELF) | grep -E " T (wlan_|CLK_Init|boot_init)" | awk '{print $$3}' | tr '\n' ' ')"
	@echo "== undefiniert: 0"

container: $(ELF) check-vendor check-net
	@test -x $(AXF2FW) || { echo "FEHLER: $(AXF2FW) fehlt -- 'make -C .. bootstrap'"; exit 1; }
	@rm -f $(CONTAINER)
	cd $(BUILD) && $(AXF2FW) mcufw.elf mcufw_MRVL.bin
	@test -f $(CONTAINER) || { echo "FEHLER: axf2firmware hat $(CONTAINER) nicht erzeugt"; exit 1; }
	@echo "== container"
	@ls -l $(CONTAINER)
	@sha256sum $(CONTAINER)

# Host build of the same network translation units: proves the crypto and the
# miIO parser against published vectors without any board in the loop.
HOST_SRCS := $(SRC)/mcufw_crypto.c $(SRC)/mcufw_miio.c $(SRC)/mcufw_selftest.c \
             $(ROOT)/host_test.c

host-test:
	@mkdir -p $(BUILD)
	$(CC_HOST) -O2 -g -std=c11 -Wall -Wextra -Werror -I$(SRC) \
		$(HOST_SRCS) -o $(BUILD)/host_test
	$(BUILD)/host_test

CC_HOST ?= gcc

inspect: $(ELF)
	@$(READELF) -SW $(ELF) | sed -n '1,40p'

clean:
	rm -rf $(BUILD)
