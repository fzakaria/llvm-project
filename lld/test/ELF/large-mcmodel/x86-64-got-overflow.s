# REQUIRES: x86
# Test demonstrating GOT relocation overflow for data access.
#
# When using the medium code model, GOT-relative accesses use R_X86_64_GOTPCREL
# (32-bit PC-relative). When the GOT is more than 2GiB away from the code
# accessing it, the relocation overflows.
#
# Note: For call/jmp instructions through GOT, the linker can use thunks to
# handle overflow. But for mov instructions (loading addresses), thunks cannot
# help and the relocation must fail. This test demonstrates the mov case.
# See x86-64-got-call-overflow.s for the call case that uses thunks.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o
# RUN: not ld.lld --no-relax %t/main.o -T %t/script.lds -o /dev/null 2>&1 | FileCheck %s

# CHECK: error: {{.*}}main.o:(.text+{{.*}}): relocation R_X86_64_REX_GOTPCRELX out of range

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    # Access a global variable through GOT
    movq external_var@GOTPCREL(%rip), %rax
    movq (%rax), %rax
    ret

.data
.globl external_var
external_var:
    .quad 0x12345678

#--- script.lds
SECTIONS {
    . = 0x200000;
    .text : { *(.text*) }
    /* Place GOT more than 2GiB away from code */
    . = 0x200000 + 0x80000100;
    .got : { *(.got*) }
    .data : { *(.data*) }
}
