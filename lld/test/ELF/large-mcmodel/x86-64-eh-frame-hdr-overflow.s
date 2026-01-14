# REQUIRES: x86
# Test that demonstrates .eh_frame relocation overflow when .text is placed
# more than 2GiB from .eh_frame. The eh_frame_hdr section also uses sdata4
# (32-bit signed) format, but the eh_frame FDE initial_location field overflows
# first.
#
# The .eh_frame_hdr overflow would be: "PC offset is too large" but the
# .eh_frame relocation fails first with R_X86_64_PC32 out of range.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o
# RUN: not ld.lld %t/main.o -T %t/script.lds --eh-frame-hdr -o /dev/null 2>&1 | FileCheck %s

# The eh_frame relocation to .text overflows first
# CHECK: error: {{.*}}:(.eh_frame+{{.*}}): relocation R_X86_64_PC32 out of range

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
    eh PT_LOAD;
    text PT_LOAD;
}
SECTIONS {
    . = 0x10000;
    .eh_frame_hdr : { *(.eh_frame_hdr*) } :eh
    .eh_frame : { *(.eh_frame) } :eh
    /* Place .text more than 2GiB away from eh_frame_hdr */
    . = 0xF00000000;
    .text : { *(.text*) } :text
}
