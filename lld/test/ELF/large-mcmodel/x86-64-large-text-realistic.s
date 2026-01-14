# REQUIRES: x86
# Test demonstrating .text overflow in a more realistic scenario where we
# generate large .text sections with actual padding instructions.
#
# This test creates multiple text sections with explicit padding to simulate
# a binary where .text exceeds 2GiB.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o
# RUN: not ld.lld %t/main.o -T %t/script.lds -o /dev/null 2>&1 | FileCheck %s

# CHECK: error: {{.*}}main.o:(.text.start+{{.*}}): relocation R_X86_64_PLT32 out of range

#--- main.s
# Place call site near the start
.section .text.start,"ax",@progbits
.globl _start
.type _start, @function
_start:
    # This call must cross more than 2GiB to reach target
    call target_func
    ret

# Create a large gap
.section .text.padding1,"ax",@progbits
.fill 0x40000000, 1, 0x90   # 1GiB of NOPs

.section .text.padding2,"ax",@progbits
.fill 0x40000000, 1, 0x90   # Another 1GiB of NOPs

# Place target function after the gap
.section .text.end,"ax",@progbits
.globl target_func
.type target_func, @function
target_func:
    ret

#--- script.lds
SECTIONS {
    . = 0x200000;
    .text : {
        *(.text.start)
        *(.text.padding1)
        *(.text.padding2)
        *(.text.end)
    }
}
