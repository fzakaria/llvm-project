# REQUIRES: aarch64

## Test that reverse relaxation for .gcc_except_table is NOT performed on
## non-x86_64 targets. Currently, reverse relaxation is only implemented for
## x86_64 because it requires architecture-specific relocation type mappings.
##
## On AArch64, when .gcc_except_table type table entries would overflow,
## the linker should emit the standard relocation overflow error rather than
## attempting to expand the entries.

# RUN: rm -rf %t && mkdir %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o a.o

## Place typeinfo more than 2GB away - this should cause a relocation overflow
## error on AArch64 since reverse relaxation is not supported.
# RUN: echo "SECTIONS { \
# RUN:   . = 0x1000; \
# RUN:   .gcc_except_table : { *(.gcc_except_table) } \
# RUN:   .rodata.typeinfo 0x100002000 : { *(.rodata.typeinfo) } \
# RUN: }" > large.lds

# RUN: not ld.lld -T large.lds a.o -o out 2>&1 | FileCheck %s

# CHECK: relocation R_AARCH64_PREL32 out of range

.text
.global _start
.type _start, @function
_start:
  ret

// .gcc_except_table containing LSDA with type table
.section .gcc_except_table,"a",@progbits
.balign 4
.Lexception_table:
  .byte 0xff                      // @LPStart encoding: omit
  .byte 0x1b                      // @TType encoding: pcrel sdata4
  .uleb128 .Lttbase - .Lttbaseoff // @TType base offset
.Lttbaseoff:
  .byte 0x01                      // Call site encoding: uleb128
  .uleb128 0                      // Call site table length (empty)
.balign 4
  // Type table entry - will overflow on AArch64
  .word _ZTIi - .
.Lttbase:

// Typeinfo symbol
.section .rodata.typeinfo,"a",@progbits
.global _ZTIi
_ZTIi:
  .xword 0
