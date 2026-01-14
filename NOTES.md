# Notes on Medium Code Model Large Binary Support

## Problem Statement
At Meta, very large static binaries on x86-64 suffer from relocation overflows with the medium code model. The goal is to push the medium code model to support binaries where sections may exceed 2GiB.

## Completed Work

### 1. Synthetic Tests (commit ce2f241e20c3)
Created tests demonstrating various relocation overflow scenarios:
- `x86-64-text-overflow.s` - R_X86_64_PLT32 overflow when call target is >2GiB away
- `x86-64-eh-frame-overflow.s` - .eh_frame FDE initial_location overflow
- `x86-64-eh-frame-hdr-overflow.s` - .eh_frame relocation before eh_frame_hdr overflow
- `x86-64-eh-frame-hdr-table-overflow.s` - .eh_frame_hdr table entry overflow
- `x86-64-got-overflow.s` - GOT access overflow
- `x86-64-lsda-overflow.s` - LSDA pointer overflow

### 2. eh_frame_hdr sdata8 Support (commit 3beb2f6d5725)
Added `--eh-frame-hdr-format=sdata8` option:
- New `EhFrameHdrFormat` enum in Config.h
- New `--eh-frame-hdr-format=` option in Options.td
- Driver parsing in Driver.cpp
- SyntheticSections.cpp updated to generate sdata8 format
- Header uses 20 bytes (vs 12 for sdata4)
- Table entries use 16 bytes (vs 8 for sdata4)
- Error message suggests using sdata8 on overflow
- Test in x86-64-eh-frame-hdr-sdata8.s

Note: sdata8 format requires runtime support in libunwind/libgcc_s.

### 3. Implement Thunks for x86-64 (commit 8a3530457384)
Range extension thunks for x86-64 when .text > 2GiB:
- Added `X86_64LongBranchThunk` class in Thunks.cpp
- Thunk uses `movabs $target, %r11; jmp *%r11` sequence (13 bytes)
- Overrode `needsThunk()` in X86_64 target to detect >2GiB branches
- Overrode `inBranchRange()` for 32-bit signed range checking
- Set `getThunkSectionSpacing()` to 1GiB intervals
- Set `needsThunks = true` in X86_64 constructor
- Updated Relocations.cpp to preserve addend for x86-64 thunks
- Tests: x86-64-thunks.s, updated x86-64-text-overflow.s

Key files modified:
- lld/ELF/Thunks.cpp - Added X86_64LongBranchThunk
- lld/ELF/Arch/X86_64.cpp - Added needsThunk(), inBranchRange(), getThunkSectionSpacing()
- lld/ELF/Relocations.cpp - Preserve addend for x86-64

### 4. GOT Overflow Handling for call/jmp (commit 456ff9e0ae6f)
For call/jmp through GOT when GOT is >2GiB away:
- Extended `needsThunk()` to handle `R_RELAX_GOT_PC` (relaxed GOT access)
- Added `R_RELAX_GOT_PC` case to `fromPlt()` to convert to R_PC for thunks
- Modified Writer.cpp to call both `createThunks()` AND `relaxOnce()`
- Only creates thunks for function symbols (via `isFunc()`) to distinguish call/jmp from mov
- Only creates thunks for `R_X86_64_PC32` on function symbols

Key insight: When the linker decides to relax `call *foo@GOTPCREL(%rip)` to a direct
call, but the target is >2GiB away, we can use thunks instead of failing. For
`mov foo@GOTPCREL(%rip), %reg` (data access), thunks cannot help since we need
the address value, not control flow - these still error if GOT is out of range.

Tests:
- `x86-64-got-call-overflow.s` - Demonstrates thunk usage for GOT call overflow
- Updated `x86-64-got-overflow.s` - Documents that mov case still fails

### 5. Multiple GOT Support (commit d56427c0c912)
Infrastructure added for multiple GOT support when primary GOT is >2GiB from some code:
- Added `X86_64SecondaryGotSection` class in SyntheticSections.h/cpp
- Added `X86_64MultiGotManager` class to track GOT accesses and create secondary GOTs
- Manager is initialized in createSyntheticSections() for EM_X86_64
- Detection logic added in X86_64::relaxOnce() to find R_GOT_PC overflow
- Relocation resolution updated in InputSection::getRelocTargetVA()
- Test: x86-64-multi-got.s

Key files modified:
- lld/ELF/SyntheticSections.h - Added X86_64SecondaryGotSection, X86_64MultiGotManager
- lld/ELF/SyntheticSections.cpp - Implementation of secondary GOT classes
- lld/ELF/Config.h - Added x86_64MultiGot to InStruct
- lld/ELF/Arch/X86_64.cpp - Added overflow detection in relaxOnce()
- lld/ELF/InputSection.cpp - Updated R_GOT_PC handling for multi-GOT

Status: Basic infrastructure in place. For production use, additional work needed on:
- Dynamic relocation duplication for secondary GOT entries
- Proper section placement algorithms
- More comprehensive testing with actual large binaries

### 6. Compiler Changes: Medium Code Model Exception Handling (uncommitted)
Modified LLVM codegen to use sdata8 (64-bit) encodings for exception handling data
when using the medium code model on x86-64:

**TargetLoweringObjectFileImpl.cpp changes:**
- `PersonalityEncoding`: Changed to use `DW_EH_PE_sdata8` for medium code model (not just large)
- `LSDAEncoding`: Changed to use `DW_EH_PE_sdata8` for medium code model
- `TTypeEncoding`: Changed to use `DW_EH_PE_sdata8` for medium code model
- For non-PIC mode, medium code model now uses `DW_EH_PE_absptr` (64-bit absolute)

**TargetLoweringObjectFile.cpp changes:**
- `Initialize()`: Now passes `UseLargeEncodings=true` to `initMCObjectFileInfo()` for
  both medium and large code models, ensuring FDE/CIE data uses 64-bit encodings

**Effect:**
When compiling with `-mcmodel=medium`, the compiler now generates:
- 64-bit PC-relative LSDA pointers in FDEs (instead of 32-bit)
- 64-bit PC-relative personality function references (instead of 32-bit)
- 64-bit type table pointers in LSDA (instead of 32-bit)
- 64-bit FDE initial_location fields (instead of 32-bit)

This eliminates relocation overflows in `.eh_frame` and `.gcc_except_table` sections
for binaries where code/data exceeds 2GiB in size.

Key files modified:
- llvm/lib/CodeGen/TargetLoweringObjectFileImpl.cpp - Exception handling encodings
- llvm/lib/Target/TargetLoweringObjectFile.cpp - FDE/CIE encoding initialization

Note: These changes require corresponding runtime support in libunwind/libgcc_s to
properly parse the 64-bit encoded exception handling data.

### 7. Jump Table 64-bit Encoding for Medium Code Model (uncommitted)
Modified the compiler to use 64-bit jump table entries for medium code model:

**X86ISelLoweringCall.cpp changes:**
- `getJumpTableEncoding()` now returns `EK_LabelDifference64` for medium code model
  (previously only for large code model)
- Jump table entries now use R_X86_64_PC64 instead of R_X86_64_PC32

**Effect:**
When compiling switch statements with `-mcmodel=medium`, the jump table entries
use 64-bit PC-relative offsets. This prevents overflow when .rodata (containing
the jump table) is more than 2GiB from .text.

Key file modified:
- llvm/lib/Target/X86/X86ISelLoweringCall.cpp

Note: The jump table base address load still uses 32-bit PC-relative addressing
(`leaq table(%rip), %rax`). For extremely large binaries where both .text AND
.rodata exceed 2GiB, use `-fno-jump-tables` as a workaround.

### 8. Constant Pool and Local Data GOTOFF Addressing (uncommitted)
Modified the compiler to use GOTOFF addressing for constant pools, jump tables, and labels
when using medium code model with `LargeDataThreshold=0`:

**Problem:**
When using `-mcmodel=medium -mlarge-data-threshold=0`, local data like constant pools,
jump tables, and labels still used RIP-relative addressing (`MO_NO_FLAG`). This causes
`R_X86_64_PC32` relocations that overflow when the constant pool is >2GiB from the code.

**X86Subtarget.cpp changes:**
- Modified `classifyLocalReference()` to check for `CM == CodeModel::Medium && TM.getLargeDataThreshold() == 0`
- When this condition is true and `GV == nullptr` (constant pools, jump tables, labels), return `MO_GOTOFF`

**TargetMachine.h changes:**
- Added `getLargeDataThreshold()` getter to access the LargeDataThreshold value

**Effect:**
When compiling with `-mcmodel=medium -mlarge-data-threshold=0 -fPIC`:
```asm
# Before (RIP-relative, can overflow):
movsd .LCPI0_0(%rip), %xmm0

# After (GOTOFF, no overflow):
leaq _GLOBAL_OFFSET_TABLE_(%rip), %rax
movsd .LCPI0_0@GOTOFF(%rax), %xmm0
```

This applies to:
- Floating point constants (materialized in constant pool)
- Vector constants
- Jump table base addresses
- Internal labels

Key files modified:
- llvm/include/llvm/Target/TargetMachine.h - Added getLargeDataThreshold() getter
- llvm/lib/Target/X86/X86Subtarget.cpp - Modified classifyLocalReference() for GOTOFF

### 9. End-to-End Integration Tests (uncommitted)
Added end-to-end tests that compile C/C++ code with clang and link with lld:

- `e2e-medium-mcmodel-basic.test` - Tests thunks with multiple object files
- `e2e-medium-mcmodel-exceptions.test` - Tests 64-bit exception handling encodings
- `e2e-multiple-objects.test` - Tests thunks across multiple translation units
- `e2e-jump-tables.test` - Tests 64-bit jump table entries

Test location: `lld/test/ELF/large-mcmodel/Integration/`

## Future Improvements / Remaining Work

### Runtime Support for sdata8 Exception Handling
The compiler and linker changes produce exception handling data with 64-bit encodings.
For this to work at runtime, libunwind and/or libgcc_s need updates to parse:
- 64-bit FDE initial_location (sdata8 in eh_frame_hdr)
- 64-bit LSDA pointers
- 64-bit personality function pointers
- 64-bit TType entries

### Production Multi-GOT Support
The current multi-GOT infrastructure is proof-of-concept. Production use needs:
- Dynamic relocation handling for secondary GOT entries
- Optimal placement algorithms for secondary GOTs
- Testing with actual large binaries

### TLS Handling
Thread-Local Storage has inherent 32-bit limitations (DTPOFF, Local Exec offsets).
Not typically a bottleneck since TLS data is usually small.

## Key Architecture Observations
- X86-64 uses RIP-relative addressing with 32-bit displacements
- R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_GOTPCRELX all have 2GiB range limit
- Thunks now supported for x86-64 (text section range extension)
- AArch64 thunk implementation was good reference (128MB range -> long thunks)
