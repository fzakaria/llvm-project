# Medium Code Model Support for Massive Binaries in LLD

This document describes the architectural changes made to LLVM's LLD linker to support linking very large static binaries (>2GiB) on x86-64 using the medium code model.

## Table of Contents

1. [Background](#background)
2. [The Problem](#the-problem)
3. [X86-64 Addressing and Relocations](#x86-64-addressing-and-relocations)
4. [Solutions Implemented](#solutions-implemented)
   - [Range Extension Thunks](#range-extension-thunks)
   - [sdata8 Format for .eh_frame_hdr](#sdata8-format-for-eh_frame_hdr)
   - [GOT Call Overflow Handling](#got-call-overflow-handling)
5. [Remaining Limitations](#remaining-limitations)
6. [Memory Layout Considerations](#memory-layout-considerations)
7. [Testing](#testing)

---

## Background

### Code Models in x86-64

The x86-64 architecture supports several code models that make different assumptions about code and data placement:

| Code Model | Text/Data Assumption | Typical Use Case |
|------------|---------------------|------------------|
| **Small** | Everything within 2GiB | Default, most programs |
| **Medium** | Code within 2GiB, data anywhere | Large data sections |
| **Large** | Everything can be anywhere | Very large programs |

The **small code model** assumes all code and data fit within the low 2GiB of the address space. This allows efficient RIP-relative addressing with 32-bit displacements.

The **large code model** makes no assumptions and always uses 64-bit absolute addressing, but this comes with significant performance overhead (more instructions, more cache pressure).

The **medium code model** is a compromise: it assumes code fits within 2GiB but allows data to be placed anywhere. However, when code sections themselves exceed 2GiB (as happens with Meta's large monolithic binaries), the medium code model encounters relocation overflows.

---

## The Problem

When linking very large binaries where the `.text` section exceeds 2GiB, several types of relocations can overflow:

### 1. Branch Relocations (R_X86_64_PLT32, R_X86_64_PC32)

```
┌──────────────────────────────────────────────────────────────────┐
│                          Address Space                            │
├──────────────────────────────────────────────────────────────────┤
│  0x0                                                              │
│  ├─── .text (caller) ────┐                                       │
│  │    call far_func      │                                       │
│  │    ↓                  │                                       │
│  │    32-bit disp        │  ← Only supports ±2GiB range          │
│  │                       │                                       │
│  ├───────────────────────┤                                       │
│  │                       │                                       │
│  │   > 2GiB gap          │  ← OVERFLOW!                          │
│  │                       │                                       │
│  ├─── .text (callee) ────┤                                       │
│  │    far_func:          │                                       │
│  │    ret                │                                       │
│  └───────────────────────┘                                       │
│  0x100000000+                                                    │
└──────────────────────────────────────────────────────────────────┘
```

The `call` instruction uses a 32-bit signed displacement (`R_X86_64_PLT32`), which can only reach ±2GiB from the instruction.

### 2. .eh_frame_hdr Table Entries

The `.eh_frame_hdr` section contains a binary search table mapping PC addresses to FDE (Frame Description Entry) locations. Traditionally, these use `sdata4` (32-bit signed) encoding:

```
.eh_frame_hdr header (12 bytes):
  ┌─────────────────────────────────────────┐
  │ version (1)          │ eh_frame_ptr_enc │
  │ fde_count_enc        │ table_enc        │
  │ eh_frame_ptr (4 bytes, sdata4)          │
  │ fde_count (4 bytes)                     │
  └─────────────────────────────────────────┘

Binary search table (8 bytes per entry, sdata4):
  ┌────────────────────────────────────────┐
  │ PC offset (4 bytes)  │ FDE offset (4)  │
  ├────────────────────────────────────────┤
  │ PC offset (4 bytes)  │ FDE offset (4)  │
  └────────────────────────────────────────┘
```

When functions are more than 2GiB from `.eh_frame_hdr`, the PC offset overflows.

### 3. GOT Access Relocations

The Global Offset Table (GOT) is accessed via RIP-relative addressing:

```asm
movq foo@GOTPCREL(%rip), %rax   # Load address of foo from GOT
call *bar@GOTPCREL(%rip)        # Call bar through GOT
```

Both use 32-bit PC-relative offsets (`R_X86_64_GOTPCRELX`), limiting the GOT to be within 2GiB of the accessing code.

### 4. LSDA (Language-Specific Data Area) Pointers

The `.eh_frame` section contains pointers to LSDA entries in `.gcc_except_table`. These use PC-relative relocations that overflow when the exception data is far from the frame descriptors.

---

## X86-64 Addressing and Relocations

### Key Relocations Affected by Large Binaries

| Relocation | Description | Displacement | Range |
|------------|-------------|--------------|-------|
| `R_X86_64_PC32` | 32-bit PC-relative | 32-bit signed | ±2GiB |
| `R_X86_64_PLT32` | PLT-relative call/jmp | 32-bit signed | ±2GiB |
| `R_X86_64_GOTPCREL` | GOT entry PC-relative | 32-bit signed | ±2GiB |
| `R_X86_64_GOTPCRELX` | Relaxable GOT access | 32-bit signed | ±2GiB |
| `R_X86_64_REX_GOTPCRELX` | REX prefix GOT access | 32-bit signed | ±2GiB |

### RIP-Relative Addressing

x86-64's RIP-relative addressing is fundamental to position-independent code:

```
┌────────────────────────────────────────────────────────────────┐
│  Instruction:  call target                                     │
│                                                                 │
│  Encoding:     E8 xx xx xx xx                                  │
│                ↑  └───────────── 32-bit signed displacement    │
│                opcode                                           │
│                                                                 │
│  Calculation:  target = RIP + displacement                     │
│               where RIP points to instruction after call       │
└────────────────────────────────────────────────────────────────┘
```

The 32-bit signed displacement limits the range to approximately ±2GiB (specifically, -2,147,483,648 to +2,147,483,647 bytes).

---

## Solutions Implemented

### Range Extension Thunks

**Problem:** Direct calls/jumps cannot reach targets more than 2GiB away.

**Solution:** Insert intermediate "thunk" code that uses 64-bit absolute addressing.

```
Original (fails when target > 2GiB away):
┌──────────────────────────────────────────────────────────────────┐
│  caller:                                                          │
│      call far_func    ← R_X86_64_PLT32 overflow!                 │
└──────────────────────────────────────────────────────────────────┘

With thunk (works for any distance):
┌──────────────────────────────────────────────────────────────────┐
│  caller:                                                          │
│      call __x86_64_thunk_far_func   ← Within 2GiB                │
│                                                                   │
│  __x86_64_thunk_far_func:                                        │
│      movabs $far_func, %r11         ← 64-bit absolute load       │
│      jmp *%r11                      ← Indirect jump              │
└──────────────────────────────────────────────────────────────────┘
```

#### Thunk Implementation Details

The thunk uses a 13-byte sequence:

```asm
# X86_64LongBranchThunk (13 bytes total)
movabs $target, %r11    # 49 BB xx xx xx xx xx xx xx xx (10 bytes)
jmp *%r11               # 41 FF E3 (3 bytes)
```

Why `%r11`?
- Caller-saved register (not preserved across calls)
- Not used for parameter passing in the System V ABI
- Available even in leaf functions

#### Thunk Placement Strategy

Thunks are placed at 1GiB intervals throughout the `.text` section:

```
┌──────────────────────────────────────────────────────────────────┐
│                        .text section                              │
├──────────────────────────────────────────────────────────────────┤
│  0x00000000  │ code... │                                         │
│              │         │                                         │
│  0x40000000  │ ──────── THUNK SECTION ────────                   │
│              │ __x86_64_thunk_func1                              │
│              │ __x86_64_thunk_func2                              │
│              │         │                                         │
│  0x80000000  │ ──────── THUNK SECTION ────────                   │
│              │         │                                         │
│  0xC0000000  │ ──────── THUNK SECTION ────────                   │
│              │         │                                         │
└──────────────────────────────────────────────────────────────────┘
```

This ensures any code can reach either the target directly (if within 2GiB) or reach a thunk that can then jump anywhere.

#### Files Modified for Thunks

- `lld/ELF/Thunks.cpp` - Added `X86_64LongBranchThunk` class
- `lld/ELF/Arch/X86_64.cpp` - Added `needsThunk()`, `inBranchRange()`, `getThunkSectionSpacing()`
- `lld/ELF/Relocations.cpp` - Preserve addend for x86-64 thunk creation

---

### sdata8 Format for .eh_frame_hdr

**Problem:** `.eh_frame_hdr` table entries use 32-bit offsets, overflowing for functions >2GiB away.

**Solution:** New `--eh-frame-hdr-format=sdata8` option to use 64-bit offsets.

```
Traditional sdata4 format:
┌────────────────────────────────────────────────────────────────┐
│ Header (12 bytes):                                              │
│   version=1, eh_frame_ptr_enc, fde_count_enc, table_enc        │
│   eh_frame_ptr (4 bytes), fde_count (4 bytes)                  │
│                                                                 │
│ Table (8 bytes per entry):                                      │
│   pc_offset (4 bytes) | fde_offset (4 bytes)                   │
└────────────────────────────────────────────────────────────────┘

New sdata8 format:
┌────────────────────────────────────────────────────────────────┐
│ Header (20 bytes):                                              │
│   version=1, eh_frame_ptr_enc, fde_count_enc, table_enc        │
│   eh_frame_ptr (8 bytes), fde_count (4 bytes), padding (4)     │
│                                                                 │
│ Table (16 bytes per entry):                                     │
│   pc_offset (8 bytes) | fde_offset (8 bytes)                   │
└────────────────────────────────────────────────────────────────┘
```

#### Usage

```bash
ld.lld --eh-frame-hdr-format=sdata8 -o output input.o
```

#### Runtime Considerations

The sdata8 format requires runtime support in the unwinder (libunwind, libgcc_s). Most modern unwinders can handle this encoding, but it should be verified for your target environment.

#### Files Modified

- `lld/ELF/Config.h` - Added `EhFrameHdrFormat` enum
- `lld/ELF/Options.td` - Added `--eh-frame-hdr-format=` option
- `lld/ELF/Driver.cpp` - Option parsing
- `lld/ELF/SyntheticSections.cpp` - sdata8 header/table generation

---

### GOT Call Overflow Handling

**Problem:** When the linker relaxes indirect GOT calls to direct calls, and the target is >2GiB away, the relaxed call also overflows.

**Solution:** Detect GOT relaxation overflow and use thunks instead.

```
Original code:
    call *foo@GOTPCREL(%rip)    # Indirect call through GOT

Normal relaxation (target within range):
    addr32 call foo             # Direct call (optimized)

With thunk (target > 2GiB away):
    call __x86_64_thunk_foo     # Call thunk instead
```

#### How It Works

1. Linker attempts GOT relaxation (`R_RELAX_GOT_PC`)
2. `needsThunk()` checks if the target is within 32-bit range
3. If not, a thunk is created instead of relaxing to a direct call
4. The original instruction is modified to call the thunk

#### Important Distinction: Call vs. Load

This optimization only works for **function calls** (control flow transfer):

```asm
# CAN be handled with thunks:
call *foo@GOTPCREL(%rip)     # Control transfer - use thunk
jmp *bar@GOTPCREL(%rip)      # Control transfer - use thunk

# CANNOT be handled with thunks:
movq baz@GOTPCREL(%rip), %rax  # Data load - needs GOT within 2GiB
```

For `mov` instructions that load addresses, we need the actual GOT value, not a control flow redirect. These still require the GOT to be within 2GiB of the accessing code.

---

## Remaining Limitations

### 1. GOT Data Access

Loading addresses from the GOT (for data symbols) still requires the GOT to be within 2GiB:

```asm
movq large_array@GOTPCREL(%rip), %rax
```

**Solution:** Multiple GOT sections (implemented as proof-of-concept, see below).

### 2. Runtime Support for sdata8 Exception Data

The compiler changes to use sdata8 for medium code model exception handling require corresponding updates to the runtime unwinder (libunwind/libgcc_s). Most modern versions support sdata8 encoding, but older versions may not.

### 3. TLS Relocations

Thread-Local Storage (TLS) relocations may have similar 32-bit range limitations.

---

## Memory Layout Considerations

### Recommended Layout for Large Binaries

```
┌────────────────────────────────────────────────────────────────┐
│  Low addresses                                                  │
├────────────────────────────────────────────────────────────────┤
│  .text (with interleaved thunk sections)                       │
│    - Code at 0x200000                                          │
│    - Thunks every 1GiB                                         │
├────────────────────────────────────────────────────────────────┤
│  .rodata                                                        │
├────────────────────────────────────────────────────────────────┤
│  .eh_frame_hdr (with sdata8 format)                            │
├────────────────────────────────────────────────────────────────┤
│  .eh_frame                                                      │
├────────────────────────────────────────────────────────────────┤
│  .gcc_except_table (near .eh_frame)                            │
├────────────────────────────────────────────────────────────────┤
│  .got (place near code if possible)                            │
├────────────────────────────────────────────────────────────────┤
│  .data                                                          │
├────────────────────────────────────────────────────────────────┤
│  .bss                                                           │
├────────────────────────────────────────────────────────────────┤
│  High addresses                                                 │
└────────────────────────────────────────────────────────────────┘
```

### Example Linker Script

```
SECTIONS {
    . = 0x200000;

    /* Code with interleaved thunks (automatic) */
    .text : { *(.text*) }

    /* Exception handling data - keep together */
    .eh_frame_hdr : { *(.eh_frame_hdr*) }
    .eh_frame : { *(.eh_frame) KEEP(*(.eh_frame)) }
    .gcc_except_table : { *(.gcc_except_table*) }

    /* GOT - place to minimize distance from code */
    .got : { *(.got*) }
    .got.plt : { *(.got.plt*) }

    /* Read-only data */
    .rodata : { *(.rodata*) }

    /* Read-write data */
    .data : { *(.data*) }
    .bss : { *(.bss*) }
}
```

---

## Testing

### Test Suite Location

All tests are in `lld/test/ELF/large-mcmodel/`:

| Test File | Description |
|-----------|-------------|
| `x86-64-text-overflow.s` | PLT32 overflow and thunk solution |
| `x86-64-thunks.s` | Basic thunk functionality |
| `x86-64-eh-frame-hdr-overflow.s` | eh_frame_hdr sdata4 overflow |
| `x86-64-eh-frame-hdr-sdata8.s` | sdata8 format validation |
| `x86-64-eh-frame-hdr-table-overflow.s` | Table entry overflow |
| `x86-64-eh-frame-overflow.s` | FDE initial_location overflow |
| `x86-64-got-overflow.s` | GOT data access overflow |
| `x86-64-got-call-overflow.s` | GOT call thunk handling |
| `x86-64-lsda-overflow.s` | LSDA pointer overflow |

### Running Tests

```bash
llvm-lit lld/test/ELF/large-mcmodel/ -v
```

### Creating Synthetic Large Binaries

To test with actual large binaries, you can use linker scripts to simulate >2GiB gaps:

```
# Simulate 4GiB+ text section
SECTIONS {
    . = 0x10000;
    .text.start : { *(.text.start) }
    . = 0x100000000;   /* 4GiB offset */
    .text.far : { *(.text.far) }
}
```

---

## Appendix: Relocation Encoding Details

### R_X86_64_PLT32

Used for function calls that may go through the PLT:

```
call foo@PLT
jmp bar@PLT
```

Calculation: `S + A - P` where:
- S = symbol value (PLT entry or direct address)
- A = addend (typically -4)
- P = place of relocation

### R_X86_64_GOTPCRELX

Used for GOT-relative accesses that may be relaxed:

```
movq foo@GOTPCREL(%rip), %rax  # May relax to: lea foo(%rip), %rax
call *bar@GOTPCREL(%rip)       # May relax to: call bar
```

The linker can relax these to direct accesses when the symbol is defined locally, but this relaxation may then overflow if the target is >2GiB away.

### R_X86_64_PC32

Generic 32-bit PC-relative relocation:

```
lea foo(%rip), %rax
movl $offset, (%rip)
```

Used for both data access and some branch instructions.

---

## Summary

The changes described in this document extend the medium code model's capabilities to support binaries with `.text` sections exceeding 2GiB. The key techniques are:

1. **Thunks** for long-range function calls (automatic)
2. **sdata8 format** for `.eh_frame_hdr` (opt-in with `--eh-frame-hdr-format=sdata8`)
3. **GOT call relaxation with thunks** (automatic)
4. **Compiler sdata8 encodings** for medium code model exception handling (LSDA, personality, TType)
5. **Multiple GOT support** for data access when GOT is >2GiB away (proof-of-concept)
6. **64-bit jump tables** for medium code model to prevent overflow in switch statements

Together with careful linker script layout, these changes enable linking of very large monolithic binaries without resorting to the performance-impacting large code model.

---

### Compiler Changes: sdata8 for Medium Code Model Exception Handling

**Problem:** The LSDA pointer, personality encoding, and TType encoding in `.eh_frame` and `.gcc_except_table` use 32-bit PC-relative offsets, causing overflow when exception data is >2GiB from code.

**Solution:** Modified LLVM codegen to use 64-bit (sdata8) encodings for the medium code model on x86-64.

#### Changes to TargetLoweringObjectFileImpl.cpp

For x86-64 with medium code model in PIC mode:

```cpp
// Before: Only large code model used sdata8
PersonalityEncoding = dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel |
                      dwarf::DW_EH_PE_sdata4;
LSDAEncoding = dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_sdata4;
TTypeEncoding = dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel |
                dwarf::DW_EH_PE_sdata4;

// After: Medium and large code models use sdata8
PersonalityEncoding = dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel |
  (CM == CodeModel::Small
   ? dwarf::DW_EH_PE_sdata4 : dwarf::DW_EH_PE_sdata8);
LSDAEncoding = dwarf::DW_EH_PE_pcrel |
  (CM == CodeModel::Small
   ? dwarf::DW_EH_PE_sdata4 : dwarf::DW_EH_PE_sdata8);
TTypeEncoding = dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel |
  (CM == CodeModel::Small
   ? dwarf::DW_EH_PE_sdata4 : dwarf::DW_EH_PE_sdata8);
```

For non-PIC mode, medium code model now uses `DW_EH_PE_absptr` (64-bit absolute).

#### Changes to TargetLoweringObjectFile.cpp

```cpp
// Before: Only large code model used 64-bit FDE encodings
initMCObjectFileInfo(ctx, TM.isPositionIndependent(),
                     TM.getCodeModel() == CodeModel::Large);

// After: Medium and large both use 64-bit FDE encodings
CodeModel::Model CM = TM.getCodeModel();
bool UseLargeEncodings = (CM == CodeModel::Medium || CM == CodeModel::Large);
initMCObjectFileInfo(ctx, TM.isPositionIndependent(), UseLargeEncodings);
```

This ensures FDE initial_location uses 64-bit encoding for medium code model.

#### Effect on Generated Code

When compiling with `-mcmodel=medium`, the compiler now generates:

```
.eh_frame with 64-bit FDE:
┌────────────────────────────────────────────────────────────────┐
│ CIE:                                                            │
│   Personality pointer: 8 bytes (DW_EH_PE_sdata8)               │
│   Augmentation data: Encoding indicators for sdata8            │
├────────────────────────────────────────────────────────────────┤
│ FDE:                                                            │
│   Initial location: 8 bytes (sdata8)                           │
│   LSDA pointer: 8 bytes (DW_EH_PE_sdata8)                      │
└────────────────────────────────────────────────────────────────┘

.gcc_except_table with 64-bit TType:
┌────────────────────────────────────────────────────────────────┐
│ LSDA:                                                           │
│   TType encoding: DW_EH_PE_sdata8                              │
│   TType entries: 8 bytes each (pointers to catch type info)    │
└────────────────────────────────────────────────────────────────┘
```

#### Usage

```bash
# Compile with medium code model (now uses sdata8 for exception data)
clang -mcmodel=medium -fPIC -c source.cpp -o source.o

# Link with sdata8 eh_frame_hdr format
ld.lld --eh-frame-hdr-format=sdata8 source.o -o binary
```

#### Runtime Requirements

The sdata8 encodings require runtime support from the unwinder. The unwinder must be able to parse:
- 64-bit FDE initial_location values
- 64-bit LSDA pointers
- 64-bit personality function pointers
- 64-bit TType entries

Most modern libunwind and libgcc_s implementations support these encodings.

#### Files Modified

- `llvm/lib/CodeGen/TargetLoweringObjectFileImpl.cpp` - Exception encoding configuration
- `llvm/lib/Target/TargetLoweringObjectFile.cpp` - FDE/CIE encoding initialization

---

### Compiler Changes: 64-bit Jump Tables for Medium Code Model

**Problem:** Switch statements generate jump tables in `.rodata` with 32-bit PC-relative entries (`R_X86_64_PC32`). If `.rodata` is more than 2GiB from `.text`, these entries overflow.

**Solution:** Modified `X86TargetLowering::getJumpTableEncoding()` to use `EK_LabelDifference64` for medium code model.

#### Changes to X86ISelLoweringCall.cpp

```cpp
// Before: Only large code model used 64-bit jump table entries
if (isPositionIndependent() &&
    getTargetMachine().getCodeModel() == CodeModel::Large &&
    !Subtarget.isTargetCOFF())
  return MachineJumpTableInfo::EK_LabelDifference64;

// After: Medium and large code models use 64-bit entries
CodeModel::Model CM = getTargetMachine().getCodeModel();
if (isPositionIndependent() &&
    (CM == CodeModel::Medium || CM == CodeModel::Large) &&
    !Subtarget.isTargetCOFF())
  return MachineJumpTableInfo::EK_LabelDifference64;
```

#### Effect on Generated Code

Jump table entries now use `R_X86_64_PC64` instead of `R_X86_64_PC32`:

```
Before (medium code model):
.rodata:
  .long .LBB0_1 - .LJTI0_0   # 4 bytes, R_X86_64_PC32

After (medium code model):
.rodata:
  .quad .LBB0_1 - .LJTI0_0   # 8 bytes, R_X86_64_PC64
```

#### Remaining Limitation

The jump table base address load still uses 32-bit PC-relative addressing:

```asm
leaq .LJTI0_0(%rip), %rax   # R_X86_64_PC32 to .rodata
```

This means the jump table itself must be within 2GiB of the code that uses it. For extremely large binaries where both `.text` AND `.rodata` exceed 2GiB, use `-fno-jump-tables` as a workaround.

#### Files Modified

- `llvm/lib/Target/X86/X86ISelLoweringCall.cpp` - Jump table encoding selection

---

## Multiple GOT Support for Data Access

### The Problem

When code is more than 2GiB away from the GOT, data access instructions that load addresses through the GOT will fail:

```asm
# This instruction uses R_X86_64_REX_GOTPCRELX with 32-bit displacement
movq external_var@GOTPCREL(%rip), %rax  # FAILS if GOT is >2GiB away
```

Unlike call/jmp through GOT (which can use thunks), data access needs the actual address value, not control transfer. Thunks cannot help here.

### The Solution: Multiple GOT Sections

The solution is to create secondary GOT sections placed throughout the address space, ensuring that every code region has a GOT within 2GiB reach.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Address Space Layout                             │
├────────────────────────────────────────────────────────────────────────┤
│  0x00000000  │ .text (region 0)                                        │
│              │ ...code accessing GOT...                                 │
│              │                                                          │
│  0x40000000  │ .got.secondary.0  ← Secondary GOT for region 0          │
│              │ (copy of entries needed by region 0)                     │
│              │                                                          │
│  0x80000000  │ .text (region 1)                                        │
│              │ ...code accessing GOT...                                 │
│              │                                                          │
│  0xC0000000  │ .got.secondary.1  ← Secondary GOT for region 1          │
│              │ (copy of entries needed by region 1)                     │
│              │                                                          │
│  0x100000000 │ .text (region 2)                                        │
│              │ ...code accessing GOT...                                 │
│              │                                                          │
│  0x140000000 │ .got (primary)                                          │
│              │ All unique GOT entries                                   │
└────────────────────────────────────────────────────────────────────────┘
```

### Implementation Approach

1. **During relocation scanning**: Track which symbols need GOT entries and from which code regions they are accessed

2. **After initial layout**: Identify which code regions would overflow when accessing the primary GOT

3. **Create secondary GOTs**: For each region that cannot reach the primary GOT, create a secondary GOT section placed within 1.5GiB of that region

4. **Populate secondary GOTs**: Copy the needed GOT entries to each secondary GOT. Dynamic relocations must be duplicated as well.

5. **Redirect relocations**: Update the relocation expression to use the nearest GOT entry instead of the primary GOT

### Relocation Resolution for Multi-GOT

When calculating `R_GOT_PC`:

```
Standard: sym->getGotVA(ctx) + addend - P

Multi-GOT: getNearestGotEntry(sym, P) + addend - P
```

Where `getNearestGotEntry(sym, P)` returns the address of the GOT entry for `sym` that is closest to address `P`.

### Dynamic Relocations

Each secondary GOT entry needs the same dynamic relocation as the primary. For preemptible symbols:

```
Primary GOT:     R_X86_64_GLOB_DAT targeting symbol
Secondary GOT 0: R_X86_64_GLOB_DAT targeting symbol (duplicate)
Secondary GOT 1: R_X86_64_GLOB_DAT targeting symbol (duplicate)
...
```

The dynamic linker will populate all copies with the same resolved address.

### Files to Modify

- `lld/ELF/SyntheticSections.h` - Add `X86_64SecondaryGotSection` class
- `lld/ELF/SyntheticSections.cpp` - Implement secondary GOT creation
- `lld/ELF/Relocations.cpp` - Track GOT accesses by code region
- `lld/ELF/Writer.cpp` - Create secondary GOTs in `finalizeAddressDependentContent`
- `lld/ELF/InputSection.cpp` - Modify `getRelocTargetVA` to use nearest GOT

---

## Theoretical Limits Analysis

### What Determines Maximum Binary Size?

With the techniques described in this document, the theoretical limits are:

| Component | Limiting Factor | Theoretical Maximum |
|-----------|-----------------|---------------------|
| `.text` section | Thunk spacing (1GiB) | **Unlimited** (thunks placed at intervals) |
| Function calls | 64-bit thunk target | **Unlimited** |
| `.eh_frame_hdr` | sdata8 format | **~8 EiB** (64-bit signed offsets) |
| GOT access (call/jmp) | Thunks | **Unlimited** |
| GOT access (data) | Multiple GOTs | **Unlimited** (with secondary GOTs) |
| Single GOT | 32-bit index (unused on x86-64) | **~32 GiB** (4 billion 8-byte entries) |
| TLS access | 32-bit offset from TP | **~4 GiB** per thread |

### Practical Limits

While theoretically unlimited, practical limits exist:

1. **Address Space**: 47-bit user virtual address space (128 TiB) on most x86-64 systems

2. **Section Size**: ELF section headers use 64-bit sizes, allowing sections up to 16 EiB

3. **Number of Symbols**: Symbol table indices are 32-bit, limiting to ~4 billion symbols

4. **Memory Usage**: Linking very large binaries requires significant RAM

### Calculation: Maximum Code Size with Thunks

Given:
- Thunk spacing: 1 GiB
- Thunk size: 13 bytes
- Overhead per thunk: Negligible

For a 100 GiB binary:
- Thunk sections: ~100 (one per GiB)
- Unique thunks per section: Limited by functions called from that region
- Typical overhead: <0.1% of binary size

### Calculation: GOT Size Growth with Multiple GOTs

For a binary with:
- Primary GOT entries: N
- Code regions: R (each ~1.5 GiB apart)
- Average entries per secondary GOT: M (≤N)

Total GOT size = N × 8 bytes (primary) + R × M × 8 bytes (secondaries)

Worst case (all regions need all entries): R × N × 8 bytes
Typical case (locality of reference): R × (N/R) × 8 ≈ N × 8 bytes

GOT duplication is bounded by the number of code regions, not binary size.

---

## LSDA and Exception Table Handling

### The Problem (Now Solved)

The `.eh_frame` section contains CIE (Common Information Entry) and FDE (Frame Description Entry) records. FDEs may contain pointers to LSDA (Language-Specific Data Area) in `.gcc_except_table`.

Previously, the LSDA pointer encoding was `DW_EH_PE_pcrel | DW_EH_PE_sdata4`, meaning a 32-bit PC-relative offset. When `.gcc_except_table` was more than 2GiB from the FDE, this would overflow.

### Solution: Compiler Changes

With the compiler changes described above, x86-64 medium code model now uses:

- `DW_EH_PE_pcrel | DW_EH_PE_sdata8` for LSDA pointers (64-bit)
- `DW_EH_PE_pcrel | DW_EH_PE_sdata8` for personality function references (64-bit)
- `DW_EH_PE_sdata8` for TType entries (64-bit)
- 64-bit FDE initial_location

This eliminates the relocation overflow issue for exception handling data.

### Runtime Requirement

The sdata8 encodings require runtime support from the unwinder:

1. **libunwind**: Most modern versions support sdata8 encoding
2. **libgcc_s**: Support varies by version; check your target platform
3. **LLVM's libunwind**: Full support for all DWARF encoding types

---

## TLS (Thread-Local Storage) Analysis

### TLS Access Models

x86-64 supports four TLS access models:

| Model | Description | Range Limitation |
|-------|-------------|------------------|
| Local Exec | Direct offset from %fs | 32-bit signed offset from TP |
| Initial Exec | GOT-relative | Same as GOT access |
| Local Dynamic | Via `__tls_get_addr` | 32-bit for DTPOFF |
| General Dynamic | Via `__tls_get_addr` | 32-bit for DTPOFF |

### Limitations

1. **Local Exec**: The offset from the thread pointer (TP) must fit in 32 bits. This limits TLS data to ~4 GiB per thread.

2. **Initial Exec**: Limited by GOT access range (solved by Multiple GOTs)

3. **DTPOFF**: The Dynamic Thread Pointer Offset is 32-bit, limiting the TLS block to 4 GiB

### Implications

For massive binaries, TLS is not typically the bottleneck:
- Most TLS data is small (thread-local variables, errno, etc.)
- Large data should not be thread-local anyway

---

## Audit of Remaining Restrictions

### Sections Using 32-bit Offsets

| Section | Field | Encoding | Status |
|---------|-------|----------|--------|
| `.eh_frame` | Initial PC | sdata4 | **FIXED** - sdata8 with medium code model |
| `.eh_frame` | LSDA pointer | sdata4 | **FIXED** - sdata8 with medium code model |
| `.eh_frame` | Personality | sdata4 | **FIXED** - sdata8 with medium code model |
| `.gcc_except_table` | TType | sdata4 | **FIXED** - sdata8 with medium code model |
| `.eh_frame_hdr` | Table entries | sdata4 | **FIXED** - sdata8 with `--eh-frame-hdr-format=sdata8` |
| `.debug_*` | Various | DWARF varies | Non-ALLOC, no runtime impact |
| `.got` | Entry access | 32-bit GOTPCREL | **FIXED** - Multi-GOT proof-of-concept |
| `.plt` | Branch to GOT | 32-bit offset | Already uses GOT |

### Relocation Types That Could Overflow

| Relocation | Usage | Status |
|------------|-------|--------|
| `R_X86_64_PC32` | General PC-relative | **FIXED** - Thunks for calls |
| `R_X86_64_PLT32` | PLT calls | **FIXED** - Thunks |
| `R_X86_64_GOTPCREL` | GOT access | **FIXED** - Thunks + Multi-GOT |
| `R_X86_64_GOTPCRELX` | Relaxable GOT | **FIXED** - Thunks + Multi-GOT |
| `R_X86_64_REX_GOTPCRELX` | REX GOT | **FIXED** - Thunks + Multi-GOT |

### Summary of Remaining Work

1. **Production Multi-GOT**: Harden proof-of-concept for production use
2. **Runtime Testing**: Validate sdata8 exception handling with target unwinders
3. **Real-world Testing**: Validation with actual large binaries

---

## Conclusion

This document outlines a comprehensive approach to extending the x86-64 medium code model to support massive binaries. The key innovations are:

1. **Range Extension Thunks**: Handle unlimited code size by placing intermediate jump points
2. **sdata8 eh_frame_hdr**: Handle unlimited `.text` span for exception handling lookup
3. **Multiple GOTs**: Handle unlimited code spread for data access through GOT
4. **Thunks for GOT Calls**: Optimize indirect calls through far GOT entries
5. **Compiler sdata8 for Medium Code Model**: Exception handling data (LSDA, personality, TType, FDE) now uses 64-bit encodings

With these techniques, the medium code model can theoretically support binaries of arbitrary size, limited only by the x86-64 virtual address space (128 TiB on most systems).

The remaining requirement is runtime support in the unwinder (libunwind/libgcc_s) for parsing sdata8-encoded exception handling data. Most modern unwinders already support this.
