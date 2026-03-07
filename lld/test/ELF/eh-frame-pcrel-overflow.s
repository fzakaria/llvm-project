# REQUIRES: x86

# Test that when .eh_frame and .text are more than 2GB apart, the linker
# expands FDE initial_location from 4 bytes to 8 bytes by modifying the
# CIE's 'R' augmentation byte from DW_EH_PE_sdata4 to DW_EH_PE_sdata8.
#
# This is "reverse relaxation" - expanding relocations that would overflow.

# RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %s -o %t.o

# Place .text at a high address (>2GB from .eh_frame) using a linker script.
# RUN: echo "SECTIONS { \
# RUN:   .eh_frame 0x1000 : { *(.eh_frame) } \
# RUN:   .text 0x100001000 : { *(.text) } \
# RUN: }" > %t.script

# RUN: ld.lld --eh-frame-hdr --script %t.script %t.o -o %t

# Check that the CIE now uses 64-bit encoding (0x1C = DW_EH_PE_pcrel | DW_EH_PE_sdata8)
# instead of 32-bit encoding (0x1B = DW_EH_PE_pcrel | DW_EH_PE_sdata4)
# RUN: llvm-readobj -S --section-data %t | FileCheck %s

# CHECK:      Section {
# CHECK:        Name: .eh_frame
# CHECK-NEXT:   Type: SHT_PROGBITS
# CHECK-NEXT:   Flags [
# CHECK-NEXT:     SHF_ALLOC
# CHECK-NEXT:   ]
# CHECK-NEXT:   Address: 0x1000
# The CIE should have 0x1C (sdata8) instead of 0x1B (sdata4) in the encoding byte
# CHECK:        SectionData (
# CIE data at offset 0x00: length, id, version, aug "zR", alignments
# CHECK:          0000: 10000000 00000000 017A5200 01010101
# encoding byte 0x1C at start of next line, followed by FDE
# CHECK-NEXT:     0010: 1C{{.*}}

.text
.global _start
_start:
  nop

.global func2
func2:
  nop

.section .eh_frame,"a",@unwind
  # CIE 1 - uses sdata4 encoding, will be expanded
  .long 16         # Size
  .long 0x00       # ID (0 = CIE)
  .byte 0x01       # Version

  .byte 0x7A       # Augmentation string: "zR"
  .byte 0x52
  .byte 0x00

  .byte 0x01       # Code alignment factor (LEB128)
  .byte 0x01       # Data alignment factor (LEB128)
  .byte 0x01       # Return address register (LEB128)

  .byte 0x01       # Augmentation data length (LEB128)
  .byte 0x1B       # DW_EH_PE_pcrel | DW_EH_PE_sdata4 (will be changed to 0x1C)

  .byte 0x00       # Padding
  .byte 0x00
  .byte 0x00

  # FDE 1 - references _start (far away)
  .long 12         # Size
  .long 24         # ID (offset to CIE = current_pos - CIE_pos)
fde1:
  .long _start - fde1   # initial_location (PC-relative) - will overflow with sdata4
  .long 1              # address_range
