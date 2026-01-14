# REQUIRES: x86
# Test that demonstrates x86-64 thunks handling R_X86_64_PLT32 relocations
# when .text section exceeds 2GiB.
#
# This test uses a linker script to place a function more than 2GiB away
# from its call site. The linker should use range extension thunks.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o

## Test: Linking should succeed with thunks
# RUN: ld.lld %t/main.o -T %t/script.lds -o %t/out

## Verify the thunk was created
# RUN: llvm-objdump -d %t/out | FileCheck %s --check-prefix=THUNK

# THUNK: <__x86_64_thunk_distant_func>:
# THUNK-NEXT: movabsq
# THUNK-NEXT: jmpq *%r11

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
