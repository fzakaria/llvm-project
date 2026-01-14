# REQUIRES: x86
# Test for --eh-frame-hdr-format=sdata8 option.
#
# This tests the linker's ability to generate a .eh_frame_hdr section using
# 64-bit (sdata8) format for the binary search table entries. This is needed
# for binaries where .text is more than 2GiB from .eh_frame_hdr.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %t/main.s -o %t/main.o

## Test 1: Default sdata4 format fails with far-apart sections
# RUN: not ld.lld %t/main.o -T %t/script.lds --eh-frame-hdr -o /dev/null 2>&1 | FileCheck %s --check-prefix=OVERFLOW

# OVERFLOW: error: {{.*}}: PC offset is too large
# OVERFLOW: consider using --eh-frame-hdr-format=sdata8

## Test 2: sdata8 format succeeds with far-apart sections
# RUN: ld.lld %t/main.o -T %t/script.lds --eh-frame-hdr --eh-frame-hdr-format=sdata8 -o %t/out

## Test 3: Verify the eh_frame_hdr uses sdata8 encoding
# RUN: llvm-readelf -x .eh_frame_hdr %t/out | FileCheck %s --check-prefix=SDATA8

# SDATA8: Hex dump of section '.eh_frame_hdr':
# First byte is version (0x01)
# Second byte is eh_frame_ptr_enc (0x1c = DW_EH_PE_pcrel | DW_EH_PE_sdata8)
# Third byte is fde_count_enc (0x03 = DW_EH_PE_udata4)
# Fourth byte is table_enc (0x3c = DW_EH_PE_datarel | DW_EH_PE_sdata8)
# SDATA8: 0x{{[0-9a-f]+}} 011c033c

## Test 4: Check section size is correct for sdata8 (20 + 16*numFdes)
# With 1 FDE: header (20) + 1 * 16 = 36 bytes = 0x24
# RUN: llvm-readelf -S %t/out | FileCheck %s --check-prefix=SIZE

# The size field shows 000024 (36 bytes)
# SIZE: .eh_frame_hdr
# SIZE-SAME: 000024

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
