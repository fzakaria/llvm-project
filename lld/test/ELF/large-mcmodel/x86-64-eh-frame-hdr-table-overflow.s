# REQUIRES: x86
# Test specifically for .eh_frame_hdr table entry overflow.
#
# The .eh_frame_hdr table uses sdata4 (32-bit signed) format for the binary
# search table entries (pc_rel, fde_va_rel). When a function's PC is more
# than 2GiB away from the .eh_frame_hdr section, this overflow occurs.
#
# This test places eh_frame close to text (so eh_frame internal relocations
# work), but puts eh_frame_hdr far away so only the header table overflows.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o
# RUN: not ld.lld %t/main.o -T %t/script.lds --eh-frame-hdr -o /dev/null 2>&1 | FileCheck %s

# CHECK: error: {{.*}}: PC offset is too large

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    .cfi_startproc
    push %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    mov %rsp, %rbp
    .cfi_def_cfa_register %rbp
    nop
    pop %rbp
    .cfi_def_cfa %rsp, 8
    ret
    .cfi_endproc

#--- script.lds
PHDRS {
    hdr PT_LOAD;
    text PT_LOAD;
}
SECTIONS {
    /* Place eh_frame_hdr at a low address */
    . = 0x10000;
    .eh_frame_hdr : { *(.eh_frame_hdr*) } :hdr

    /* Place .text and .eh_frame more than 2GiB away from eh_frame_hdr
       but close to each other so eh_frame's internal relocations work */
    . = 0xF00000000;
    .text : { *(.text*) } :text
    .eh_frame : { *(.eh_frame) } :text
}
