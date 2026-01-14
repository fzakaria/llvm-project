# REQUIRES: x86
# Test that demonstrates .eh_frame relocation overflow.
#
# The .eh_frame section FDE entries use R_X86_64_PC32 relocations to
# point to their associated code. When code is more than 2GiB away,
# this relocation overflows.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o
# RUN: not ld.lld %t/main.o -T %t/script.lds -o /dev/null 2>&1 | FileCheck %s

# CHECK: error: {{.*}}:(.eh_frame+{{.*}}): relocation R_X86_64_PC32 out of range

#--- main.s
.text
.globl _start
.type _start, @function
_start:
    .cfi_startproc
    .cfi_lsda 0, _ex
    push %rbp
    .cfi_def_cfa_offset 16
    nop
    pop %rbp
    ret
    .cfi_endproc

.data
_ex:
    .word 0

#--- script.lds
PHDRS {
    eh PT_LOAD;
    text PT_LOAD;
}
SECTIONS {
    . = 0x10000;
    .eh_frame_hdr : { *(.eh_frame_hdr*) } :eh
    .eh_frame : { *(.eh_frame) } :eh
    /* Place .text more than 2GiB away from eh_frame */
    . = 0xF00000000;
    .text : { *(.text*) } :text
    .data : { *(.data) } :text
}
