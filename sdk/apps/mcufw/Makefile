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

LDSCRIPT  := $(ROOT)/mw300_min.ld
TARGET    := $(BUILD)/mcufw
ELF       := $(TARGET).elf
CONTAINER := $(BUILD)/mcufw_MRVL.bin

# -ffreestanding + -fno-unwind-tables: no host libc, no .ARM.exidx bloat.
# -fno-common: gcc>=10 default, stated explicitly for older toolchains.
CFLAGS    := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -mabi=aapcs \
             -O2 -g -std=c11 -Wall -Wextra -Werror \
             -ffreestanding -fno-common -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fno-ident \
             -I$(SRC)
LDFLAGS   := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -mabi=aapcs \
             -nostdlib -nostartfiles -Wl,--build-id=none \
             -Wl,-Map=$(TARGET).map -T$(LDSCRIPT)

SRCS      := $(SRC)/startup.c $(SRC)/main.c
OBJS      := $(BUILD)/startup.o $(BUILD)/main.o

.PHONY: all elf container inspect clean

all: container

elf: $(ELF)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@
	@echo "== elf layout"
	@$(READELF) -h $(ELF) | grep -E 'Entry point|Machine|Flags'
	@$(READELF) -lW $(ELF) | grep -E '^  LOAD|Type' | sed 's/^/   /'
	@$(SIZE) -A $(ELF) | grep -E '^\.(init|text|data|bss)'

container: $(ELF)
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
