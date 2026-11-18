/*
 * mw300_min.ld -- linker script for the standalone mcufw (88MW300/88MW302).
 *
 * Derived from EZ-Connect-Lite `build/toolchains/GNU/mw300-xip.ld`
 * (Copyright (C) 2008-2014, Marvell International Ltd.) with two deliberate
 * changes:
 *
 *  1. No ROM-library data: upstream defines `.rom_data (NOLOAD)` sized by
 *     `_rom_data` from libwmsdk.a. mcufw links no vendor library, so `.data`
 *     is pinned directly at 0x20000040 -- the load address that the vendor
 *     image color1_0061 uses (container segment 3).
 *  2. `.init` is padded to `_init_size` = 0x3210 bytes. In the vendor build
 *     `.text` starts at `_flashc_mem_start + _text_offset + SIZEOF(.init)`;
 *     the vendor flash/QSPI driver fills .init to exactly 0x3210 bytes, which
 *     is why the vendor `.text` sits at 0x1f0032d8. Padding keeps that flash
 *     address stable for a much smaller mcufw, so the resulting container has
 *     the same three load addresses as color1_0061.
 *
 * The padding lives in SRAM0 (harmless, 384 KB available) and is zero-filled.
 */

ENTRY(Reset_IRQHandler)

_estack = 0x20020000;            /* end of 128K SRAM1                     */
_text_offset = 0xc8;             /* sizeof(img_hdr) + 3 * sizeof(seg_hdr) */
_flashc_mem_start = 0x1f000000;  /* flash controller memory window        */
_init_size = 0x3210;             /* vendor .init occupancy (color1_0061)  */

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

	/* FreeRTOS heap window expected by libwmsdk.a: everything from the end of
	 * .bss to the top of SRAM1.  The bump allocator in mcufw_shim.c is its only
	 * consumer and is inert until phase 5 (mcufw_os_ready() == 0). */
	_bss = _sbss;
	_heap_2_start = _ebss;
	_heap_2_end = _estack;
	_heap_start = _ebss;
	_heap_end = _estack;

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
ASSERT(_ebss <= _estack, "mcufw .bss overflows SRAM1")
ASSERT(SIZEOF(.nvram) == 256, "mcufw .nvram is not 256 bytes")
ASSERT(_nvram_begin == 0x480c0000, "mcufw .nvram is not at 0x480c0000")
