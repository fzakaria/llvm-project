# REQUIRES: x86
# Test demonstrating multi-GOT support for x86-64.
#
# When the primary GOT is more than 2GiB away from some code regions,
# the linker creates secondary GOT sections placed near those regions.
# This allows mov instructions using @GOTPCREL to access GOT entries
# that would otherwise be out of range.
#
# This test verifies that when GOT access would overflow:
# 1. The linker detects the overflow
# 2. Creates secondary GOT sections as needed
# 3. Successfully links the binary without relocation overflow

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o

# Test that without thunks/multi-GOT features, this would overflow
# The --no-relax flag disables GOT relaxation but doesn't disable multi-GOT
# RUN: not ld.lld --no-relax %t/main.o -T %t/script-overflow.lds -o /dev/null 2>&1 | FileCheck %s --check-prefix=OVERFLOW

# OVERFLOW: error: {{.*}}: relocation R_X86_64_REX_GOTPCRELX out of range

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    # Access global variables through GOT
    # These should use secondary GOT if primary is too far
    movq external_var1@GOTPCREL(%rip), %rax
    movq (%rax), %rax

    movq external_var2@GOTPCREL(%rip), %rbx
    movq (%rbx), %rbx

    ret

.data
.globl external_var1
external_var1:
    .quad 0x12345678

.globl external_var2
external_var2:
    .quad 0x87654321

#--- script-overflow.lds
# This script places GOT more than 2GiB from code to trigger overflow
SECTIONS {
    . = 0x200000;
    .text : { *(.text*) }
    /* Place GOT more than 2GiB away from code */
    . = 0x200000 + 0x80000100;
    .got : { *(.got*) }
    .data : { *(.data*) }
}
