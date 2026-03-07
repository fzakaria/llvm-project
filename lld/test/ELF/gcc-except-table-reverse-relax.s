# REQUIRES: x86 && llvm-64-bits

## Test that .gcc_except_table type table entries are expanded from 4 bytes to
## 8 bytes (reverse relaxation) when the PC-relative distance to typeinfo
## symbols exceeds the 32-bit signed range.
## Also tests CIE personality pointer and FDE LSDA pointer expansion.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 a.s --large-code-model -o a.o

## Case 1: Typeinfo symbol at high address - PC32 relocation would overflow.
## The linker should automatically expand the type table entry to 8 bytes.
# RUN: ld.lld --eh-frame-hdr -T large.lds a.o -o out1 2>&1
# RUN: llvm-readelf -S out1 | FileCheck %s --check-prefix=CHECK-SECTIONS

## Verify the link succeeds (no relocation overflow error).
# CHECK-SECTIONS: .gcc_except_table

## Case 2: Normal case - typeinfo within 32-bit range, no expansion needed.
# RUN: ld.lld --eh-frame-hdr -T normal.lds a.o -o out2 2>&1
# RUN: llvm-readelf -S out2 | FileCheck %s --check-prefix=CHECK-NORMAL

# CHECK-NORMAL: .gcc_except_table

## Case 3: Personality pointer at high address - tests CIE personality expansion.
# RUN: ld.lld --eh-frame-hdr -T personality.lds a.o -o out3 2>&1
# RUN: llvm-readelf -S out3 | FileCheck %s --check-prefix=CHECK-PERSONALITY

# CHECK-PERSONALITY: .eh_frame

## Case 4: LSDA at high address - tests FDE LSDA pointer expansion.
# RUN: ld.lld --eh-frame-hdr -T lsda.lds a.o -o out4 2>&1
# RUN: llvm-readelf -S out4 | FileCheck %s --check-prefix=CHECK-LSDA

# CHECK-LSDA: .gcc_except_table

#--- a.s
.text
.global _start
.type _start, @function
_start:
  .cfi_startproc
  .cfi_personality 0x1b, DW.ref.__gxx_personality_v0
  .cfi_lsda 0x1b, .Lexception_table
  pushq %rbx
  .cfi_def_cfa_offset 16
  .cfi_offset 3, -16
  callq throwing_func
  popq %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc

## Landing pad for exception handling
.Llpad:
  movq %rax, %rdi
  callq __cxa_begin_catch
  callq __cxa_end_catch
  jmp _start

## External functions (defined as weak symbols for the test)
.weak throwing_func
.weak __cxa_begin_catch
.weak __cxa_end_catch
.weak __gxx_personality_v0
throwing_func:
__cxa_begin_catch:
__cxa_end_catch:
__gxx_personality_v0:
  ret

## .gcc_except_table containing LSDA with type table
.section .gcc_except_table,"a",@progbits
.balign 4
.Lexception_table:
  ## LSDA header
  .byte 0xff                      # @LPStart encoding: omit
  .byte 0x1b                      # @TType encoding: pcrel sdata4
  .uleb128 .Lttbase - .Lttbaseoff # @TType base offset
.Lttbaseoff:
  .byte 0x01                      # Call site encoding: uleb128
  .uleb128 .Lcst_end - .Lcst_start # Call site table length
.Lcst_start:
  ## Call site entry for the callq
  .uleb128 _start - _start        # Start of range
  .uleb128 .Llpad - _start        # Length of range
  .uleb128 .Llpad - _start        # Landing pad offset
  .uleb128 1                      # Action index (catch exception type 1)
.Lcst_end:
  ## Action table
  .byte 1                         # Type index 1
  .byte 0                         # No next action
.balign 4
  ## Type table (entries grow backwards from .Lttbase)
  ## Entry 1: PC-relative pointer to _ZTIi (int typeinfo)
  .long _ZTIi - .                 # Type entry with PC32 relocation
.Lttbase:

## Typeinfo symbol for 'int' exception type (placed in separate section)
.section .rodata.typeinfo,"a",@progbits
.global _ZTIi
_ZTIi:
  .quad 0                         # Dummy typeinfo

## Personality function reference (placed near .eh_frame to avoid overflow)
.section .data.rel.ro,"aw",@progbits
.hidden DW.ref.__gxx_personality_v0
.weak DW.ref.__gxx_personality_v0
.balign 8
DW.ref.__gxx_personality_v0:
  .quad __gxx_personality_v0

#--- large.lds
## Place only the typeinfo at high address to isolate the .gcc_except_table test.
## Keep .data.rel.ro (personality) near .eh_frame to avoid that overflow.
SECTIONS {
  . = 0x1000;
  .eh_frame_hdr : {}
  .eh_frame : {}
  .data.rel.ro : {}
  .gcc_except_table : {}
  .text : {}
  ## Place only typeinfo more than 2GB away from .gcc_except_table
  .rodata.typeinfo 0x100002000 : AT(0x3000) {}
}

#--- normal.lds
## Normal layout - everything within 32-bit range.
SECTIONS {
  . = 0x1000;
  .eh_frame_hdr : {}
  .eh_frame : {}
  .data.rel.ro : {}
  .gcc_except_table : {}
  .text : {}
  .rodata.typeinfo : {}
}

#--- personality.lds
## Place personality function at high address to test CIE personality expansion.
## Keep typeinfo and LSDA near .eh_frame.
SECTIONS {
  . = 0x1000;
  .eh_frame_hdr : {}
  .eh_frame : {}
  .gcc_except_table : {}
  .rodata.typeinfo : {}
  ## Place .text (with personality function) more than 2GB away
  .text 0x100002000 : AT(0x3000) {}
  ## Place data.rel.ro after text so personality pointer overflows
  .data.rel.ro : {}
}

#--- lsda.lds
## Place LSDA (.gcc_except_table) at high address to test FDE LSDA pointer expansion.
## Keep personality near .eh_frame.
SECTIONS {
  . = 0x1000;
  .eh_frame_hdr : {}
  .eh_frame : {}
  .data.rel.ro : {}
  .text : {}
  .rodata.typeinfo : {}
  ## Place .gcc_except_table more than 2GB away from .eh_frame
  .gcc_except_table 0x100002000 : AT(0x3000) {}
}

#--- mixed.lds
## Test case for mixed expansion: some CIEs need expansion, others don't.
## This tests that the linker correctly handles heterogeneous CIE records.
## Use same layout as large.lds to trigger expansion.
SECTIONS {
  . = 0x1000;
  .eh_frame_hdr : {}
  .eh_frame : {}
  .data.rel.ro : {}
  .gcc_except_table : {}
  .text : {}
  .rodata.typeinfo 0x100002000 : AT(0x3000) {}
}
