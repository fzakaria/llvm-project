# REQUIRES: x86
# Test that demonstrates thunk usage for call/jmp through GOT when GOT is out of range.
#
# When using the medium code model with large binaries, GOT-relative accesses
# (R_X86_64_GOTPCRELX) use 32-bit PC-relative addressing. When the GOT is more
# than 2GiB away from the code, these relocations overflow.
#
# For call/jmp instructions (call *foo@GOTPCREL(%rip)), the linker can use
# thunks to bypass the GOT entirely - the thunk loads the target address
# directly with movabs.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o

## Test: Linking should succeed with thunks for call *@GOTPCREL
# RUN: ld.lld %t/main.o -T %t/script.lds -o %t/out

## Verify the thunk was created
# RUN: llvm-objdump -d %t/out | FileCheck %s --check-prefix=THUNK

# THUNK: <__x86_64_thunk_distant_func>:
# THUNK-NEXT: movabsq
# THUNK-NEXT: jmpq *%r11

## Verify the call was converted from indirect to direct
# RUN: llvm-objdump -d %t/out | FileCheck %s --check-prefix=CALL

# CALL: <_start>:
# CALL: callq {{.*}}<__x86_64_thunk_distant_func>

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    # This is "call *distant_func@GOTPCREL(%rip)" - indirect call through GOT
    # When GOT is >2GiB away, this will use a thunk instead
    call *distant_func@GOTPCREL(%rip)
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
    /* Place distant function and GOT more than 2GiB away */
    . = 0x200000 + 0x80000100;
    .text.distant : { *(.text.distant) }
    .got : { *(.got*) }
}
