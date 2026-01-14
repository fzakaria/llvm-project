# REQUIRES: x86
# Test demonstrating gcc_except_table (LSDA) relocation overflow.
#
# The .gcc_except_table section is referenced from .eh_frame LSDA pointers.
# When the LSDA is more than 2GiB away from the .eh_frame section,
# the relocation overflows.

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
    # Reference LSDA (Language Specific Data Area) for exception handling
    .cfi_lsda 0x1b, .LLSDA0
    push %rbp
    .cfi_def_cfa_offset 16
    nop
    pop %rbp
    ret
    .cfi_endproc

.section .gcc_except_table,"a",@progbits
.LLSDA0:
    .byte 0xff    # Landing pad base encoding: omit
    .byte 0xff    # Type table encoding: omit
    .byte 0x01    # Call site table length
    .byte 0       # Padding

#--- script.lds
PHDRS {
    eh PT_LOAD;
    text PT_LOAD;
    lsda PT_LOAD;
}
SECTIONS {
    . = 0x10000;
    .eh_frame_hdr : { *(.eh_frame_hdr*) } :eh
    .eh_frame : { *(.eh_frame) } :eh
    . = 0x100000;
    .text : { *(.text*) } :text
    /* Place LSDA more than 2GiB away from eh_frame */
    . = 0xF00000000;
    .gcc_except_table : { *(.gcc_except_table*) } :lsda
}
