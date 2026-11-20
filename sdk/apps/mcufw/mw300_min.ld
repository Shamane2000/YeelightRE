/*
 * mw300_min.ld -- linker script for the standalone mcufw (88MW300/88MW302).
 *
 * Dual-SRAM layout matching EZ-Connect-Lite `mw300-xip.ld` while pinning
 * segment load addresses to match vendor color1_0061:
 *   .isr_vector + XIP-critical drivers -> SRAM0 @ 0x00100000 (_init_size = 0x3210)
 *   .text / .rodata                    -> XIP   @ 0x1f0032d8
 *   .data                              -> SRAM1 @ 0x20000040
 *   primary FreeRTOS heap              -> SRAM0 (_init_size .. _keystore_start)
 *   secondary FreeRTOS heap            -> SRAM1 (_ebss .. _main_stack_start)
 *   main MSP stack (2 KiB)             -> SRAM1 (_main_stack_start .. _estack)
 */

ENTRY(Reset_IRQHandler)

_estack = 0x20020000;            /* end of 128K SRAM1                     */
_main_stack_size = 0x800;        /* 2 KiB main stack for boot/ISRs        */
_main_stack_start = _estack - _main_stack_size;
_text_offset = 0xc8;             /* sizeof(img_hdr) + 3 * sizeof(seg_hdr) */
_flashc_mem_start = 0x1f000000;  /* flash controller memory window        */
_init_size = 0x3210;             /* vendor .init occupancy (color1_0061)  */
_keystore_size = 0x1000;         /* 4 KiB keystore reserved at top SRAM0  */
_keystore_start = ORIGIN(SRAM0) + LENGTH(SRAM0) - _keystore_size;

MEMORY
{
	SRAM0 (rwx) : ORIGIN = 0x00100000, LENGTH = 384K
	SRAM1 (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
	NVRAM (rw)  : ORIGIN = 0x480C0000, LENGTH = 4K
	FLASHC (rw) : ORIGIN = 0x20020000, LENGTH = 32K
}

SECTIONS
{
	.init :
	{
		. = ALIGN(256);
		KEEP(*(.isr_vector))
		KEEP(*(.text.Default_IRQHandler*))
		KEEP(*(.text.HardFault_IRQHandler*))
		*mw300_flash.o (.text .text.* .rodata .rodata.*)
		*mw300_flashc.o (.text .text.* .rodata .rodata.*)
		*mw300_qspi.o (.text .text.* .rodata .rodata.*)
		*mw300_pmu.o (.text .text.* .rodata .rodata.*)
		*(.ram .ram.*)
		. = ALIGN(4);
		_init_used = .;
		/* pad to the vendor .init occupancy; fails loudly if mcufw's own
		 * .init ever grows past it (ld: "cannot move location counter
		 * backwards"). */
		. = ORIGIN(SRAM0) + _init_size;
	} > SRAM0

	.text (_flashc_mem_start + _text_offset + _init_size) :
	{
		. = ALIGN(4);
		KEEP(*(.text.Reset_IRQHandler))
		*(.text .text.* .gnu.linkonce.t.*)
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		. = ALIGN(8);
		_etext = .;
	}

	.data 0x20000040 :
	{
		. = ALIGN(4);
		_sdata = .;
		*(.data)
		*(.data.*)
		. = ALIGN(4);
		_edata = .;
	} > SRAM1
	_sidata = LOADADDR(.data);

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_sbss = .;
		*(.bss)
		*(.bss.*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} > SRAM1

	/* Primary FreeRTOS heap lives in unused SRAM0 (between padded .init and
	 * keystore). heap_4.c uses _heap_start.._heap_end directly, leaving
	 * SRAM1 below _main_stack_start untouched at prvHeapInit(). */
	_bss = _sbss;
	_heap_start = ORIGIN(SRAM0) + _init_size;
	_heap_end = _keystore_start;
	_heap_2_start = _ebss;
	_heap_2_end = _main_stack_start;

	.nvram (NOLOAD) :
	{
		_nvram_begin = .;
		_nvram_start = .;
		KEEP(*(.nvram))
		KEEP(*(.nvram.*))
		. = _nvram_start + 256;
		. = ALIGN(4);
		_nvram_end = .;
	} > NVRAM

	/DISCARD/ :
	{
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		*(.ARM.extab* .gnu.linkonce.armextab.*)
		*(.eh_frame*)
		*(.comment)
	}
}

ASSERT(SIZEOF(.init) == _init_size, "mcufw .init does not match _init_size")
ASSERT(ADDR(.text) == 0x1f0032d8, "mcufw .text is not at the color1_0061 address")
ASSERT(ADDR(.data) == 0x20000040, "mcufw .data is not at the color1_0061 address")
ASSERT(_ebss <= _main_stack_start, "mcufw .bss collides with main stack")
ASSERT(_heap_end > _heap_start, "invalid SRAM0 heap")
ASSERT(SIZEOF(.nvram) == 256, "mcufw .nvram is not 256 bytes")
ASSERT(_nvram_begin == 0x480c0000, "mcufw .nvram is not at 0x480c0000")
