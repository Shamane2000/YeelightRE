# mcufw/Makefile -- compile + link the minimal firmware and pack the MRVL container.

ROOT      := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
SDK_DIR   := $(abspath $(ROOT)/../../vendor/ez-connect-lite)
AXF2FW    := $(SDK_DIR)/sdk/tools/bin/Linux/axf2firmware
BUILD     := $(ROOT)/build
SRC       := $(ROOT)/src

CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size
READELF   := $(CROSS)readelf
NM        := $(CROSS)nm

# Vendor SDK archive: mcufw pulls GPIO_PinMuxFun/GPIO_PinModeConfig from
# mw300_pinmux.o (zero undefined symbols) so the pinmux path runs through real
# vendor code.  The archive also ships ttcp.o with its own main(), so it is
# linked as a plain archive (members pulled on demand), never --whole-archive.
VENDOR_LIB := $(SDK_DIR)/sdk/libs/libwmsdk.a

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

SRCS      := $(SRC)/startup.c $(SRC)/main.c $(SRC)/mcufw_gpt.c
OBJS      := $(BUILD)/startup.o $(BUILD)/main.o $(BUILD)/mcufw_gpt.o

.PHONY: all elf container check-vendor inspect clean

all: container

elf: $(ELF)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) $(VENDOR_LIB) -lgcc -o $@
	@echo "== elf layout"
	@$(READELF) -h $(ELF) | grep -E 'Entry point|Machine|Flags'
	@$(READELF) -lW $(ELF) | grep -E '^  LOAD|Type' | sed 's/^/   /'
	@$(SIZE) -A $(ELF) | grep -E '^\.(init|text|data|bss)'

# Hard proof that the vendor archive is really in the image: the pinmux
# accessor must be defined (T) and the archive member must not drag in a
# second main().
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

container: $(ELF) check-vendor
	@test -x $(AXF2FW) || { echo "FEHLER: $(AXF2FW) fehlt -- 'make -C .. bootstrap'"; exit 1; }
	@rm -f $(CONTAINER)
	cd $(BUILD) && $(AXF2FW) mcufw.elf mcufw_MRVL.bin
	@test -f $(CONTAINER) || { echo "FEHLER: axf2firmware hat $(CONTAINER) nicht erzeugt"; exit 1; }
	@echo "== container"
	@ls -l $(CONTAINER)
	@sha256sum $(CONTAINER)

inspect: $(ELF)
	@$(READELF) -SW $(ELF) | sed -n '1,40p'

clean:
	rm -rf $(BUILD)
