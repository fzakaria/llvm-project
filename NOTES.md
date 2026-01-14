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

## Remaining Work

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

## Future Improvements

### LSDA Pointer Handling
The LSDA (Language-Specific Data Area) pointer in FDEs uses 32-bit PC-relative encoding.
This is set by the compiler, not the linker. Workarounds:
- Place .gcc_except_table near .eh_frame via linker script
- Compiler changes to use sdata8 encoding for large binaries

### TLS Handling
Thread-Local Storage has inherent 32-bit limitations (DTPOFF, Local Exec offsets).
Not typically a bottleneck since TLS data is usually small.

## Key Architecture Observations
- X86-64 uses RIP-relative addressing with 32-bit displacements
- R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_GOTPCRELX all have 2GiB range limit
- Thunks now supported for x86-64 (text section range extension)
- AArch64 thunk implementation was good reference (128MB range -> long thunks)
