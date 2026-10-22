/* Application slot behind NI's 12 KB DFU bootloader.
   0x0801B800: NI knob names (kept so stock firmware restores intact).
   0x0801E800 and 0x0801F000: settings pages (settings_store.c).
   0x0801F800: NI settings, calibration, serial number (never written).
   0x0801FC00: bootloader flag page; never linked into. */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x08003000, LENGTH = 0x18800  /* up to 0x0801B800 (NI knob names) */
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 8K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
