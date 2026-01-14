# REQUIRES: x86
# Test x86-64 range extension thunks for large binaries.
#
# When .text exceeds 2GiB, RIP-relative call/jump instructions can overflow
# their 32-bit signed displacement. The linker automatically inserts thunks
# to handle this case.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o

## Test: Linking should succeed with thunks
# RUN: ld.lld %t/main.o -T %t/script.lds -o %t/out

## Verify the thunk was created and contains the expected code
# RUN: llvm-objdump -d %t/out | FileCheck %s --check-prefix=THUNK

# THUNK: <__x86_64_thunk_far_func>:
# THUNK-NEXT: movabsq
# THUNK-NEXT: jmpq *%r11

## Verify the call goes through the thunk
# RUN: llvm-objdump -d %t/out | FileCheck %s --check-prefix=CALL

# CALL: <_start>:
# CALL: callq {{.*}}<__x86_64_thunk_far_func>

## Verify the thunk target is correct (far_func should be at 0x100000000)
# RUN: llvm-nm %t/out | FileCheck %s --check-prefix=SYMS

# SYMS: 0000000100000000 T far_func

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    call far_func
    ret

.section .text.far, "ax"
.globl far_func
.type far_func, @function
far_func:
    ret

#--- script.lds
# This linker script places far_func more than 2GiB away from _start,
# triggering thunk creation
SECTIONS {
    . = 0x10000;
    .text : { *(.text) }
    . = 0x100000000;
    .text.far : { *(.text.far) }
}
