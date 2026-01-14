# REQUIRES: x86
# Test that demonstrates R_X86_64_PLT32/R_X86_64_PC32 relocation overflow
# when .text section exceeds 2GiB.
#
# This test uses a linker script to place a function more than 2GiB away
# from its call site, demonstrating the need for range extension thunks.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o
# RUN: not ld.lld %t/main.o -T %t/script.lds -o /dev/null 2>&1 | FileCheck %s

# CHECK: error: {{.*}}main.o:(.text+{{.*}}): relocation R_X86_64_PLT32 out of range

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    call distant_func
    ret

.section .text.distant,"ax",@progbits
.globl distant_func
.type distant_func, @function
distant_func:
    ret

#--- script.lds
SECTIONS {
  . = 0x200000;
  .text : { *(.text) }
  /* Place distant function over 2GiB away */
  . = 0x200000 + 0x80000100;
  .text.distant : { *(.text.distant) }
}
