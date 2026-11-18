# sdk/Makefile -- build entry point for the YeelightRE custom-firmware toolchain.
#
#   make bootstrap   clone/verify the pinned EZ-Connect-Lite SDK + host tools
#   make             build mcufw.elf -> MRVL container, then verify it
#   make verify      run tools/verify_mrvl_container.py against container + reference
#   make clean       drop build/ and vendor/
#   make print-config
#
# Everything runs on devastator (192.168.1.154). No flashing, no device access.

SHELL := /bin/bash

ROOT      := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO      := $(abspath $(ROOT)/..)
SDK_DIR   := $(ROOT)/vendor/ez-connect-lite
AXF2FW    := $(SDK_DIR)/sdk/tools/bin/Linux/axf2firmware
APP_DIR   := $(ROOT)/apps/mcufw
BUILD     := $(APP_DIR)/build
REF_BIN   := $(REPO)/firmware/color1_142_0061/upd_yeelink.light.color1.bin
REF_ELF   := $(REPO)/firmware/color1_142_0061/color1_0061.elf
VERIFIER  := $(REPO)/tools/verify_mrvl_container.py
PYTHON    ?= python3

ELF       := $(BUILD)/mcufw.elf
CONTAINER := $(BUILD)/mcufw_MRVL.bin

.PHONY: all bootstrap elf container verify verify-reference clean print-config check-env

all: verify

bootstrap:
	@bash $(ROOT)/bootstrap.sh

check-env:
	@test -f $(AXF2FW) || { echo "FEHLER: $(AXF2FW) fehlt -- bitte 'make bootstrap'"; exit 1; }
	@command -v arm-none-eabi-gcc >/dev/null || { echo "FEHLER: arm-none-eabi-gcc fehlt"; exit 1; }

elf: check-env
	@$(MAKE) --no-print-directory -C $(APP_DIR) elf

container: elf
	@$(MAKE) --no-print-directory -C $(APP_DIR) container

verify: container
	@echo "== verify container against reference $REF_BIN"
	@$(PYTHON) $(VERIFIER) \
		--container $(CONTAINER) \
		--reference $(REF_BIN) \
		--app-slot-max 0x60000

verify-reference:
	@echo "== verify reference image $REF_BIN (self-check of the verifier)"
	@$(PYTHON) $(VERIFIER) --container $(REF_BIN) --app-slot-max 0x60000

# Proves the toolchain is faithful: repacking the reference ELF with the vendor
# axf2firmware reproduces the shipped vendor container -- segment table (offsets,
# sizes, load addresses, CRCs) and all payload bytes identical. Two fields are
# expected to differ and are reported as INFO:
#   0x08-0x0b  timestamp  (axf2firmware writes time(NULL))
#   0x10-0x13  entry      (the reference ELF artefact has e_entry=0x100001, the
#                          shipped container carries 0x1f0032d9 -- see docs/07)
verify-roundtrip: check-env
	@mkdir -p $(BUILD)
	@cp $(REF_ELF) $(BUILD)/ref_roundtrip.elf
	@cd $(BUILD) && $(AXF2FW) ref_roundtrip.elf ref_roundtrip.bin
	@echo "== roundtrip: reference ELF -> axf2firmware vs. shipped vendor image"
	@$(PYTHON) $(VERIFIER) \
		--container $(BUILD)/ref_roundtrip.bin \
		--determinism-against $(REF_BIN) \
		--entry-mismatch-ok \
		--app-slot-max 0x60000

print-config:
	@printf 'root       = %s\n' '$(ROOT)'
	@printf 'repo       = %s\n' '$(REPO)'
	@printf 'sdk_dir    = %s\n' '$(SDK_DIR)'
	@printf 'axf2firmware = %s\n' '$(AXF2FW)'
	@printf 'build      = %s\n' '$(BUILD)'
	@printf 'reference  = %s\n' '$(REF_BIN)'

clean:
	rm -rf $(BUILD)
