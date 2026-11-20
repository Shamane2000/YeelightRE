# mcufw/Makefile -- Phase 4: persistente Config (Flash/NVRAM-Seam) + Power-on.
# Register-Pfad aus 3.7 (GPT0 HAL-Seam host/target) bleibt unveraendert.
#
# FreeRTOS und lwIP werden aus den SDK-Sources gebaut. Jede Source-Gruppe
# hat explizite Objekt-Pfade im Build-Verzeichnis, um Kollisionen bei
# gleichen Dateinamen (z.B. timers.c in FreeRTOS UND lwIP) zu vermeiden.

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
OBJDUMP   := $(CROSS)objdump

VENDOR_LIB := $(SDK_DIR)/sdk/libs/libwmsdk.a
VENDOR_STAGE := $(BUILD)/vendor
PATCHED_LIB  := $(BUILD)/libwmsdk_mcufw.a
RENAMES      := Reset_IRQHandler Default_IRQHandler HardFault_IRQHandler

LDSCRIPT  := $(ROOT)/mw300_min.ld
TARGET    := $(BUILD)/mcufw
ELF       := $(TARGET).elf
CONTAINER := $(BUILD)/mcufw_MRVL.bin

REPO_ROOT := $(abspath $(ROOT)/../../..)
CFLAGS    := -mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mabi=aapcs \
             -O2 -g -std=c11 -Wall -Wextra -Werror \
             -ffreestanding -fno-common -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fno-ident \
             -ffile-prefix-map=$(REPO_ROOT)=/yeelight-re \
             -I$(SRC) \
             -I$(SDK_DIR)/sdk/external/freertos/Source/include \
             -I$(SDK_DIR)/sdk/external/freertos/Source/portable/GCC/ARM_CM4F \
             -I$(SDK_DIR)/sdk/external/freertos \
             -I$(SDK_DIR)/sdk/external/lwip/src/include \
             -I$(SDK_DIR)/sdk/external/lwip/src/include/ipv4 \
             -I$(SDK_DIR)/sdk/external/lwip/src/include/ipv6 \
             -I$(SDK_DIR)/sdk/external/lwip/contrib/port/FreeRTOS/wmsdk \
             -I$(SDK_DIR)/sdk/src/incl/sdk \
             -I$(SDK_DIR)/sdk/src/incl/platform \
             -I$(SDK_DIR)/sdk/src/incl/platform/os \
             -I$(SDK_DIR)/sdk/src/incl/libc \
             -I$(SDK_DIR)/sdk/src/incl/libc/GCC \
             -I$(SDK_DIR)/sdk/src/incl/sdk/drivers/mw300
# SDK vendor code (FreeRTOS, lwIP, MQTT) is not -Werror clean.
# Strip -Werror and add targeted suppressions for known SDK warnings.
SDK_CFLAGS := $(filter-out -Werror,$(CFLAGS)) \
              -Wno-unused-parameter -Wno-sign-compare \
              -DTCP_SND_BUF_COUNT=4 -DLWIP_DISABLE_TCP_SANITY_CHECKS=1 \
              -include src/sdk_compat.h \

LDFLAGS   := -mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mabi=aapcs \
             -nostdlib -nostartfiles -Wl,--build-id=none \
             -Wl,-Map=$(TARGET).map -T$(LDSCRIPT)

# --- phase 4: persistent config backend ----------------------------------
# stub  = deklarierter No-Persist-Stub (Default; ehrlich, kein stilles Tun).
# flash = echtes flash_drv_* Backend, OPT-IN und erst nach Sektor-Nachweis
#         auf dem Bench-Traeger freizugeben (Phase 5).
MCUFW_CONFIG_BACKEND ?= stub
ifeq ($(MCUFW_CONFIG_BACKEND),flash)
CONFIG_HAL_OBJ := $(BUILD)/mcufw/mcufw_config_hal_target_flash.o
else ifeq ($(MCUFW_CONFIG_BACKEND),stub)
CONFIG_HAL_OBJ := $(BUILD)/mcufw/mcufw_config_hal_target_stub.o
else
$(error MCUFW_CONFIG_BACKEND muss stub oder flash sein (aktuell: $(MCUFW_CONFIG_BACKEND)))
endif

# --- phase 4b.1: rollback backend ---------------------------------------
MCUFW_ROLLBACK_BACKEND ?= stub
ifeq ($(MCUFW_ROLLBACK_BACKEND),flash)
ROLLBACK_HAL_OBJ := $(BUILD)/mcufw/mcufw_rollback_hal_target_flash.o
else ifeq ($(MCUFW_ROLLBACK_BACKEND),stub)
ROLLBACK_HAL_OBJ := $(BUILD)/mcufw/mcufw_rollback_hal_target_stub.o
else
$(error MCUFW_ROLLBACK_BACKEND muss stub oder flash sein (aktuell: $(MCUFW_ROLLBACK_BACKEND)))
endif

# --- mcufw application objects -------------------------------------------
MCUFW_OBJS := $(BUILD)/mcufw/startup.o $(BUILD)/mcufw/main.o \
              $(BUILD)/mcufw/mcufw_gpt.o $(BUILD)/mcufw/mcufw_crypto.o \
              $(BUILD)/mcufw/mcufw_miio.o $(BUILD)/mcufw/mcufw_wlan.o \
              $(BUILD)/mcufw/mcufw_selftest.o $(BUILD)/mcufw/mcufw_shim.o \
              $(BUILD)/mcufw/mcufw_rtos.o $(BUILD)/mcufw/mcufw_lwip.o \
              $(BUILD)/mcufw/mcufw_lan_rpc.o $(BUILD)/mcufw/mcufw_mqtt.o \
              $(BUILD)/mcufw/mcufw_cf.o $(BUILD)/mcufw/mcufw_lwip_chksum.o \
              $(BUILD)/mcufw/mcufw_state.o $(BUILD)/mcufw/mcufw_pwm.o \
              $(BUILD)/mcufw/mcufw_pwm_hal_target.o \
              $(BUILD)/mcufw/mcufw_net.o $(BUILD)/mcufw/mcufw_net_lwip.o \
              $(BUILD)/mcufw/mcufw_runtime.o \
              $(BUILD)/mcufw/mcufw_config.o $(CONFIG_HAL_OBJ) \
              $(BUILD)/mcufw/mcufw_rollback.o $(ROLLBACK_HAL_OBJ)

# --- FreeRTOS objects ----------------------------------------------------
FR_DIR := $(SDK_DIR)/sdk/external/freertos
FR_OBJS := $(BUILD)/freertos/list.o $(BUILD)/freertos/queue.o \
           $(BUILD)/freertos/tasks.o $(BUILD)/freertos/event_groups.o \
           $(BUILD)/freertos/croutine.o $(BUILD)/freertos/timers.o \
           $(BUILD)/freertos/heap_4.o $(BUILD)/freertos/port.o

# --- lwIP objects --------------------------------------------------------
LW_DIR := $(SDK_DIR)/sdk/external/lwip
LW_OBJS := $(BUILD)/lwip/sockets.o $(BUILD)/lwip/api_lib.o $(BUILD)/lwip/api_msg.o $(BUILD)/lwip/err.o $(BUILD)/lwip/netbuf.o $(BUILD)/lwip/netdb.o \
           $(BUILD)/lwip/def.o $(BUILD)/lwip/dhcp.o $(BUILD)/lwip/pbuf.o \
           $(BUILD)/lwip/tcp_out.o $(BUILD)/lwip/raw.o $(BUILD)/lwip/lwip_timers.o \
           $(BUILD)/lwip/dns.o $(BUILD)/lwip/stats.o $(BUILD)/lwip/tcp_in.o \
           $(BUILD)/lwip/ip4_addr.o \
           $(BUILD)/lwip/ip_frag.o $(BUILD)/lwip/ip4.o \
           $(BUILD)/lwip/inet.o $(BUILD)/lwip/icmp.o $(BUILD)/lwip/udp.o \
           $(BUILD)/lwip/tcp.o $(BUILD)/lwip/netif.o $(BUILD)/lwip/mem.o \
           $(BUILD)/lwip/igmp.o $(BUILD)/lwip/netifapi.o $(BUILD)/lwip/etharp.o $(BUILD)/lwip/tcpip.o $(BUILD)/lwip/sys_arch.o $(BUILD)/lwip/memp.o $(BUILD)/lwip/init.o


# --- extra lwIP objects (Phase 3.5a) ------------------------------------
$(BUILD)/lwip/netifapi.o: $(LW_DIR)/src/api/netifapi.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/sockets.o: $(LW_DIR)/src/api/sockets.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/api_lib.o: $(LW_DIR)/src/api/api_lib.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/api_msg.o: $(LW_DIR)/src/api/api_msg.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/err.o: $(LW_DIR)/src/api/err.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/netbuf.o: $(LW_DIR)/src/api/netbuf.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/netdb.o: $(LW_DIR)/src/api/netdb.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/etharp.o: $(LW_DIR)/src/netif/etharp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/tcpip.o: $(LW_DIR)/src/api/tcpip.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/memp.o: $(LW_DIR)/src/core/memp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/lwip/init.o: $(LW_DIR)/src/core/init.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

ALL_OBJS := $(MCUFW_OBJS) $(FR_OBJS) $(LW_OBJS) 

.PHONY: all elf container check-vectors check-vendor check-net check-owner check-owner-elf check-config inspect clean host-test

all: container

elf: $(ELF)

# --- directory creation ---------------------------------------------------
$(BUILD)/mcufw $(BUILD)/freertos $(BUILD)/lwip $(BUILD)/vendor:
	@mkdir -p $@

# --- mcufw application compile rules -------------------------------------
$(BUILD)/mcufw/%.o: $(SRC)/%.c | $(BUILD)/mcufw
	$(CC) $(CFLAGS) -c $< -o $@
# main.c lives in app root, not src/ — needs explicit rule
$(BUILD)/mcufw/main.o: main.c | $(BUILD)/mcufw
	$(CC) $(CFLAGS) -c $< -o $@

# Phase 3.8: the runtime loop must not flood the UART with status lines.  There
# is no RTOS tick source in this image yet (AP join / scheduler = phase 5), so
# the loop runs unpaced and the status interval is the only rate limit.
$(BUILD)/mcufw/mcufw_runtime.o: $(SRC)/mcufw_runtime.c $(SRC)/mcufw_runtime.h | $(BUILD)/mcufw
	$(CC) $(CFLAGS) -DMCUFW_RUNTIME_STATUS_EVERY=65536u -c $< -o $@


# --- FreeRTOS compile rules ----------------------------------------------
$(BUILD)/freertos/list.o: $(FR_DIR)/Source/list.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/queue.o: $(FR_DIR)/Source/queue.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/tasks.o: $(FR_DIR)/Source/tasks.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/event_groups.o: $(FR_DIR)/Source/event_groups.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/croutine.o: $(FR_DIR)/Source/croutine.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/timers.o: $(FR_DIR)/Source/timers.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/heap_4.o: $(FR_DIR)/Source/portable/MemMang/heap_4.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/freertos/port.o: $(FR_DIR)/Source/portable/GCC/ARM_CM4F/port.c | $(BUILD)/freertos
	$(CC) $(SDK_CFLAGS) -c $< -o $@

# --- lwIP compile rules --------------------------------------------------
$(BUILD)/lwip/def.o: $(LW_DIR)/src/core/def.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/dhcp.o: $(LW_DIR)/src/core/dhcp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/pbuf.o: $(LW_DIR)/src/core/pbuf.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/tcp_out.o: $(LW_DIR)/src/core/tcp_out.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/raw.o: $(LW_DIR)/src/core/raw.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/lwip_timers.o: $(LW_DIR)/src/core/timers.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/dns.o: $(LW_DIR)/src/core/dns.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/stats.o: $(LW_DIR)/src/core/stats.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/tcp_in.o: $(LW_DIR)/src/core/tcp_in.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/ip4_addr.o: $(LW_DIR)/src/core/ipv4/ip4_addr.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/ip4.o: $(LW_DIR)/src/core/ipv4/ip4.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/mcufw_lwip_chksum.o: src/mcufw_lwip_chksum.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lwip/ip_frag.o: $(LW_DIR)/src/core/ipv4/ip_frag.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/inet.o: $(LW_DIR)/src/core/ipv4/inet.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/icmp.o: $(LW_DIR)/src/core/ipv4/icmp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/udp.o: $(LW_DIR)/src/core/udp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/tcp.o: $(LW_DIR)/src/core/tcp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/netif.o: $(LW_DIR)/src/core/netif.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/lwip/mem.o: $(LW_DIR)/src/core/mem.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@


$(BUILD)/lwip/sys_arch.o: $(LW_DIR)/contrib/port/FreeRTOS/wmsdk/sys_arch.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@


$(BUILD)/lwip/igmp.o: $(LW_DIR)/src/core/ipv4/igmp.c | $(BUILD)/lwip
	$(CC) $(SDK_CFLAGS) -c $< -o $@

# --- vendor archive patching ---------------------------------------------
$(PATCHED_LIB): $(VENDOR_LIB) | $(BUILD)/vendor
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

# --- link ----------------------------------------------------------------
$(ELF): $(ALL_OBJS) $(PATCHED_LIB) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(ALL_OBJS) $(PATCHED_LIB) -lgcc -o $@
	@echo "== elf layout"
	@$(READELF) -h $(ELF) | grep -E 'Entry point|Machine|Flags'
	@$(READELF) -lW $(ELF) | grep -E '^  LOAD|Type' | sed 's/^/   /'
	@$(SIZE) -A $(ELF) | grep -E '^\.(init|text|data|bss)'

check-vendor: $(ELF)
	@$(NM) $(ELF) | grep -q "T GPIO_PinMuxFun" \
		|| { echo "FEHLER: GPIO_PinMuxFun aus libwmsdk.a nicht gelinkt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T GPIO_PinModeConfig" \
		|| { echo "FEHLER: GPIO_PinModeConfig aus libwmsdk.a nicht gelinkt"; exit 1; }
	@test "$$($(NM) $(ELF) | grep -c "^[0-9a-f]* T main$$")" = "1" \
		|| { echo "FEHLER: mehrdeutige main()-Definition im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_gpt0_pwm_init" || { echo "FEHLER: GPT0-PWM-Treiber nicht im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_lamp_set" || { echo "FEHLER: mcufw_lamp_set fehlt im Image"; exit 1; }
	@echo "== lamp pwm: $$($(NM) $(ELF) | grep -c " T mcufw_lamp_") Symbole"
	@echo "== vendor link: $$($(NM) $(ELF) | grep -c " T GPIO_") Symbole aus $(notdir $(VENDOR_LIB))"

check-net: $(ELF)
	@for sym in wlan_init_lock_list wlan_init_adapter wlan_init_priv wlan_init_fw wlan_allocate_adapter; do \
		$(NM) $(ELF) | grep -q " T $$sym$$" \
			|| { echo "FEHLER: $$sym aus libwmsdk.a(mlan_init.o) nicht gelinkt"; exit 1; }; \
	done
	@for sym in CLK_Init boot_init; do \
		$(NM) $(ELF) | grep -q " T $$sym$$" \
			|| { echo "FEHLER: $$sym aus libwmsdk.a(mdev_startup.o) nicht gelinkt"; exit 1; }; \
	done
	@$(NM) $(ELF) | grep -q "T mcufw_miio_handle" || { echo "FEHLER: miIO-Listener nicht im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_wlan_bringup" || { echo "FEHLER: mcufw_wlan_bringup fehlt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_selftest" || { echo "FEHLER: mcufw_selftest fehlt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_state_set_power" || { echo "FEHLER: mcufw_state_set_power fehlt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_pwm_set" || { echo "FEHLER: mcufw_pwm_set fehlt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_pwm_hsv" || { echo "FEHLER: mcufw_pwm_hsv fehlt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_pwm_hal_write" || { echo "FEHLER: Register-Seam mcufw_pwm_hal_write fehlt"; exit 1; }
	@! $(NM) $(ELF) | grep -q "mcufw_pwm_hal_stub_" || { echo "FEHLER: Host-Stub im Target-Image gelinkt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T vTaskStartScheduler" || { echo "FEHLER: FreeRTOS vTaskStartScheduler nicht gelinkt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T udp_new" || { echo "FEHLER: lwIP udp_new nicht gelinkt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T tcp_new" || { echo "FEHLER: lwIP tcp_new nicht gelinkt"; exit 1; }
	@undef=$$($(NM) -u $(ELF) | wc -l); test "$$undef" = "0" \
		|| { echo "FEHLER: $$undef undefinierte Symbole im ELF:"; $(NM) -u $(ELF); exit 1; }
	@echo "== rtos: $$($(NM) $(ELF) | grep -cE " T (vTask|xTask|pvPort)") FreeRTOS-Symbole"
	@echo "== lwip: $$($(NM) $(ELF) | grep -cE " T (udp_|tcp_|pbuf_|netif_|dhcp_)") lwIP-Symbole"
	@echo "== undefiniert: 0"

# --- phase 3.8: single-writer guard --------------------------------------
# mcufw_lamp_set() may be called from exactly ONE place in the application
# sources: the bounded boot diagnosis in main.c.  Any second call site (e.g. a
# reintroduced ramp in the runtime loop) fails the build here.
check-owner:
	@bad=0; \
	for f in main.c $(SRC)/*.c; do \
		case "$$f" in *mcufw_gpt.c) continue;; esac; \
		n=$$(grep -E 'mcufw_lamp_set[[:space:]]*\(' "$$f" | grep -vE '^[[:space:]]*(/\*|\*|//)' | wc -l); \
		if [ "$$n" != "0" ]; then \
			echo "   owner: $$f -> $$n call site(s)"; \
			if [ "$$f" != "main.c" ] || [ "$$n" != "1" ]; then \
				echo "FEHLER: mcufw_lamp_set() in $$f ($$n Aufrufe) -- erlaubt ist genau 1 Aufruf in main.c (Boot-Diagnose)"; \
				bad=1; \
			fi; \
		fi; \
	done; \
	test "$$bad" = "0" || exit 1; \
	n=$$(grep -E 'mcufw_lamp_set[[:space:]]*\(' main.c | grep -vE '^[[:space:]]*(/\*|\*|//)' | wc -l); \
	test "$$n" = "1" \
		|| { echo "FEHLER: Boot-Sweep-Aufruf von mcufw_lamp_set() fehlt in main.c (gueltige Aufrufe: $$n)"; exit 1; }; \
	echo "== owner: mcufw_lamp_set 1 Aufrufstelle (main.c boot-ramp), Dauerpfad schreibt nur mcufw_pwm"

check-owner-elf: $(ELF)
	@$(NM) $(ELF) | grep -q "T mcufw_pwm_service" || { echo "FEHLER: mcufw_pwm_service fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_runtime_tick" || { echo "FEHLER: mcufw_runtime_tick fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_net_pump_udp" || { echo "FEHLER: UDP-Pump fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_net_pump_tcp" || { echo "FEHLER: TCP-Pump fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_net_lwip_bind" || { echo "FEHLER: lwIP-Bindung fehlt im Image"; exit 1; }
	@! $(NM) $(ELF) | grep -q "mcufw_net_host" || { echo "FEHLER: Host-Transport im Target-Image gelinkt"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_config_load" || { echo "FEHLER: mcufw_config_load fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_config_save" || { echo "FEHLER: mcufw_config_save fehlt im Image"; exit 1; }
	@$(NM) $(ELF) | grep -q "T mcufw_config_hal_write" || { echo "FEHLER: Config-Seam mcufw_config_hal_write fehlt"; exit 1; }
	@! $(NM) $(ELF) | grep -q "mcufw_config_hal_stub_" || { echo "FEHLER: Host-Config-Stub im Target-Image gelinkt"; exit 1; }
	@echo "== owner-elf: runtime-owner + beide Netz-Pumps + Config-Seam im Image, keine Host-Stubs"

# --- phase 4: config backend guard ---------------------------------------
# Der Default-Build darf KEIN flash_drv_* referenzieren (Sektor unbewiesen).
# Ein Flash-Build muss sie dagegen wirklich linken -- beides wird geprueft.
check-config: $(ELF)
	@if [ "$(MCUFW_CONFIG_BACKEND)" = "flash" ]; then \
		$(NM) $(ELF) | grep -q "T flash_drv_write" \
			|| { echo "FEHLER: Backend=flash, aber flash_drv_write nicht gelinkt"; exit 1; }; \
		echo "== config: Backend flash (flash_drv_* gelinkt, Sektor-Nachweis Phase 5 offen)"; \
	else \
		if $(OBJDUMP) -d $(ELF) | grep -A1 "<mcufw_config_hal_read>:" | grep -q "flash_drv_"; then \
			echo "FEHLER: Backend=stub, aber mcufw_config_hal_read ruft flash_drv_* auf"; exit 1; \
		fi; \
		if $(OBJDUMP) -d $(ELF) | grep -A1 "<mcufw_config_hal_write>:" | grep -q "flash_drv_"; then \
			echo "FEHLER: Backend=stub, aber mcufw_config_hal_write ruft flash_drv_* auf"; exit 1; \
		fi; \
		echo "== config: Backend stub-no-persist (deklariert, kein Flash-Zugriff im Config-Pfad)"; \
	fi

# --- phase 4b.2: vector table verification ---------------------------------
check-vectors: $(ELF)
	@echo "== check-vectors: verifying ISR vector table layout"
	@$(NM) $(ELF) | grep -q "T SVC_IRQHandler" || { echo "FEHLER: SVC_IRQHandler nicht als T-Symbol"; exit 1; }
	@$(NM) $(ELF) | grep -q "T PendSV_IRQHandler" || { echo "FEHLER: PendSV_IRQHandler nicht als T-Symbol"; exit 1; }
	@$(NM) $(ELF) | grep -q "T SysTick_IRQHandler" || { echo "FEHLER: SysTick_IRQHandler nicht als T-Symbol"; exit 1; }
	@$(NM) $(ELF) | grep -q "T SDIO_IRQHandler" || { echo "FEHLER: SDIO_IRQHandler nicht als T-Symbol"; exit 1; }
	@$(NM) $(ELF) | grep -q "T WIFIWKUP_IRQHandler" || { echo "FEHLER: WIFIWKUP_IRQHandler nicht als T-Symbol"; exit 1; }
	@$(NM) $(ELF) | grep -q "T DMA_IRQHandler" || { echo "FEHLER: DMA_IRQHandler nicht als T-Symbol"; exit 1; }
	@$(NM) $(ELF) | grep -q "T QSPI_IRQHandler" || { echo "FEHLER: QSPI_IRQHandler nicht als T-Symbol"; exit 1; }
	@vec_info=$$($(NM) -S $(ELF) | awk '/ mcufw_vectors$$/{print $$1 ":" strtonum("0x"$$2)}'); \
	  test "$$vec_info" = "00100000:316" || { echo "FEHLER: mcufw_vectors=$$vec_info != 00100000:316 (79*4)"; exit 1; }
	@! $(NM) $(ELF) | grep -q "mcufw_rollback_hal_test_" || { echo "FEHLER: Host-Rollback-Stub im Target-Image gelinkt"; exit 1; }
	@echo "== check-vectors: PASS (mcufw_vectors @ 0x00100000 = 316 B / 79 Slots, kein Host-Rollback-Stub)"

container: $(ELF) check-vectors check-vendor check-net check-owner check-owner-elf check-config
	@test -x $(AXF2FW) || { echo "FEHLER: $(AXF2FW) fehlt"; exit 1; }
	@rm -f $(CONTAINER)
	cd $(BUILD) && $(AXF2FW) mcufw.elf mcufw_MRVL.bin
	@test -f $(CONTAINER) || { echo "FEHLER: axf2firmware hat $(CONTAINER) nicht erzeugt"; exit 1; }
	@echo "== container"
	@ls -l $(CONTAINER)
	@sha256sum $(CONTAINER)

HOST_SRCS := $(SRC)/mcufw_crypto.c $(SRC)/mcufw_miio.c $(SRC)/mcufw_selftest.c \
             $(SRC)/mcufw_lan_rpc.c $(SRC)/mcufw_mqtt.c $(SRC)/mcufw_cf.c \
             $(SRC)/mcufw_state.c $(SRC)/mcufw_pwm.c \
             $(SRC)/mcufw_pwm_hal_host.c \
             $(SRC)/mcufw_net.c $(SRC)/mcufw_net_host.c \
             $(SRC)/mcufw_runtime.c \
             $(SRC)/mcufw_config.c $(SRC)/mcufw_config_hal_host.c \
             $(SRC)/mcufw_rollback.c $(SRC)/mcufw_rollback_hal_host.c \
             $(SRC)/mcufw_wlan.c \
             $(ROOT)/host_test.c

host-test:
	@mkdir -p $(BUILD)
	$(CC_HOST) -O2 -g -std=c11 -Wall -Wextra -Werror -I$(SRC) \
		-DHOST_TEST \
		$(HOST_SRCS) -o $(BUILD)/host_test
	$(BUILD)/host_test

CC_HOST ?= gcc

inspect: $(ELF)
	@$(READELF) -SW $(ELF) | sed -n '1,40p'

clean:
	rm -rf $(BUILD)
