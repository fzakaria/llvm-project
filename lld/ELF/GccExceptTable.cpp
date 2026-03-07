//===- GccExceptTable.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// .gcc_except_table section contains Language-Specific Data Area (LSDA)
// for C++ exception handling. This file provides parsing and manipulation
// utilities for reverse relaxation when type table entries would overflow
// 32-bit PC-relative relocations.
//
//===----------------------------------------------------------------------===//

#include "GccExceptTable.h"
#include "Config.h"
#include "DWARF.h"
#include "InputSection.h"
#include "OutputSections.h"
#include "Relocations.h"
#include "Symbols.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::dwarf;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

// Parse an LSDA (Language-Specific Data Area) from .gcc_except_table using
// DWARFDataExtractor for proper DWARF encoding handling.
//
// LSDA format:
//   @LPStart encoding (1 byte)
//   @LPStart value (variable, if encoding != DW_EH_PE_omit)
//   @TType encoding (1 byte)
//   @TType base offset (ULEB128, if encoding != DW_EH_PE_omit)
//   Call site encoding (1 byte)
//   Call site table length (ULEB128)
//   Call site table entries
//   Action table entries
//   Type table entries (growing backwards from @TType base)
std::optional<LSDAParsed> elf::parseLSDA(ArrayRef<uint8_t> data, bool is64Bit) {
  if (data.empty())
    return std::nullopt;

  LSDAParsed result;

  // Use DWARFDataExtractor for proper DWARF encoding parsing
  DWARFDataExtractor extractor(
      StringRef(reinterpret_cast<const char *>(data.data()), data.size()),
      /*isLittleEndian=*/true, /*AddressSize=*/is64Bit ? 8 : 4);
  uint64_t offset = 0;

  // @LPStart encoding (1 byte)
  result.lpStartEncodingOffset = offset;
  uint8_t lpStartEnc = extractor.getU8(&offset);

  // Skip @LPStart value if present
  if (lpStartEnc != DW_EH_PE_omit) {
    // Use getEncodedPointer to properly handle the encoding
    // We don't care about the actual value, just need to skip it
    std::optional<uint64_t> lpStart =
        extractor.getEncodedPointer(&offset, lpStartEnc, offset);
    if (!lpStart)
      return std::nullopt;
  }

  // @TType encoding (1 byte)
  if (!extractor.isValidOffset(offset))
    return std::nullopt;
  result.ttypeEncodingOffset = offset;
  result.ttypeEncoding = extractor.getU8(&offset);

  // @TType base offset (ULEB128, if encoding != DW_EH_PE_omit)
  if (result.ttypeEncoding != DW_EH_PE_omit) {
    result.ttypeBaseOffsetStart = offset;
    result.ttypeBaseOffset = extractor.getULEB128(&offset);
    result.ttypeBaseOffsetEnd = offset;
  }

  // Call site encoding (1 byte)
  if (!extractor.isValidOffset(offset))
    return std::nullopt;
  result.callSiteEncodingOffset = offset;
  result.callSiteEncoding = extractor.getU8(&offset);

  // Call site table length (ULEB128)
  if (!extractor.isValidOffset(offset))
    return std::nullopt;
  result.callSiteTableLengthOffset = offset;
  uint64_t callSiteLen = extractor.getULEB128(&offset);

  // Call site table start and end
  result.callSiteTableStart = offset;
  result.callSiteTableEnd = offset + callSiteLen;

  if (result.callSiteTableEnd > data.size())
    return std::nullopt;

  // Action table starts after call site table
  result.actionTableStart = result.callSiteTableEnd;

  // Type table base: this is where type entries are counted backwards from
  // ttypeBase = position after @TType base offset ULEB128 + ttypeBaseOffset
  if (result.ttypeEncoding != DW_EH_PE_omit) {
    result.ttypeBase = result.ttypeBaseOffsetEnd + result.ttypeBaseOffset;
    result.typeTableEntrySize =
        getSizeForEncoding(result.ttypeEncoding, is64Bit);
  }

  return result;
}

// Perform reverse relaxation on a .gcc_except_table section.
// If any type table relocation would overflow 32-bit, we need to:
// 1. Change TType encoding from sdata4 to sdata8
// 2. Update the @TType base offset to account for larger entries
// 3. Update all type table relocations from R_X86_64_PC32 to R_X86_64_PC64
// 4. Expand the section data to accommodate 8-byte type entries
//
// NOTE: This is currently x86_64-specific. Other architectures would need
// their own relocation type mappings (e.g., R_AARCH64_PREL32 ->
// R_AARCH64_PREL64).
//
// CONVERGENCE GUARANTEE:
// This function is idempotent and guarantees loop convergence because:
// 1. We only expand sections using 4-byte type encoding (sdata4/udata4)
// 2. After expansion, the encoding becomes sdata8, which makes the check
//    `getSizeForEncoding(lsda.ttypeEncoding, ...) != 4` return early
// 3. Therefore, a section is expanded at most once, preventing infinite loops
//    in the address assignment convergence loop in Writer.cpp
//
// Returns true if the section was modified.
bool elf::reverseRelaxGccExceptTable(Ctx &ctx, InputSection *sec) {
  // Only supported on x86_64 currently.
  // Other architectures would need different relocation type handling.
  if (ctx.arg.emachine != EM_X86_64)
    return false;

  if (!sec || sec->content().empty())
    return false;

  std::optional<LSDAParsed> maybeLsda = parseLSDA(sec->content(), ctx.arg.is64);
  if (!maybeLsda)
    return false;

  LSDAParsed &lsda = *maybeLsda;

  // Only process if using 4-byte type encoding (sdata4 or udata4).
  // Skip indirect encodings (DW_EH_PE_indirect = 0x80) as they point to
  // pointers rather than values, and require different handling.
  if (lsda.ttypeEncoding == DW_EH_PE_omit ||
      (lsda.ttypeEncoding & DW_EH_PE_indirect) ||
      getSizeForEncoding(lsda.ttypeEncoding, ctx.arg.is64) != 4)
    return false;

  // Check if any relocation in the type table would overflow 32 bits.
  //
  // Type table layout (entries grow backwards from ttypeBase):
  //
  //   actionTableStart
  //        |
  //        v
  //   +----+----+----+----+
  //   |....|typ3|typ2|typ1|  <-- ttypeBase
  //   +----+----+----+----+
  //        ^              ^
  //        |              |
  //   ttypeBase - 3*4   ttypeBase - 1*4
  //
  // Entry at index i is located at offset: ttypeBase - i * entrySize
  bool needsExpansion = false;
  size_t typeTableEnd = lsda.ttypeBase;
  size_t maxTypeIndex = 0;

  // Count relocations in the type table region and check for overflow
  for (const Relocation &rel : sec->relocs()) {
    // Type table entries are stored backwards from ttypeBase
    // Entry at index i is at offset (ttypeBase - i * entrySize)
    // So relocations with offset < ttypeBase and offset >= actionTableStart
    // are potentially in the type table
    if (rel.offset < lsda.actionTableStart || rel.offset >= typeTableEnd)
      continue;

    // Only check PC32 relocations (could overflow)
    if (rel.type != R_X86_64_PC32)
      continue;

    // Calculate the type index from the relocation offset.
    // distFromBase = ttypeBase - rel.offset
    // Since original entries are 4 bytes (sdata4) and grow backwards:
    //   typeIndex = distFromBase / 4
    size_t distFromBase = typeTableEnd - rel.offset;
    if (distFromBase % 4 != 0 || distFromBase == 0)
      continue;

    size_t idx = distFromBase / 4;
    if (idx > maxTypeIndex)
      maxTypeIndex = idx;

    // Check if the relocation target would overflow 32 bits
    if (!rel.sym)
      continue;

    uint64_t secAddr = 0;
    if (sec->getOutputSection())
      secAddr = sec->getOutputSection()->addr + sec->outSecOff;

    int64_t targetAddr = rel.sym->getVA(ctx) + rel.addend;
    int64_t pcRel = targetAddr - (secAddr + rel.offset);

    if (!llvm::isInt<32>(pcRel))
      needsExpansion = true;
  }

  if (!needsExpansion)
    return false;

  // Need to expand the LSDA:
  // - Change ttypeEncoding from sdata4 to sdata8
  // - Update @TType base offset (ULEB128) to point further ahead
  // - Expand each 4-byte type entry to 8 bytes
  // - Update relocation types from R_X86_64_PC32 to R_X86_64_PC64

  lsda.numTypeEntries = maxTypeIndex;

  // Calculate size increase: each type entry grows by 4 bytes (8 - 4 = 4)
  size_t extraBytes = lsda.numTypeEntries * 4;

  // Create expanded data buffer
  ArrayRef<uint8_t> origData = sec->content();
  SmallVector<uint8_t, 256> expanded;
  expanded.reserve(origData.size() + extraBytes);

  // Copy up to @TType encoding byte
  size_t copyEnd = lsda.ttypeEncodingOffset;
  expanded.append(origData.begin(), origData.begin() + copyEnd);

  // Write updated @TType encoding (change size encoding from sdata4 to sdata8).
  // Preserve the high nibble (application: pcrel, etc.) and replace low nibble.
  // We already excluded indirect encodings above, so no need to worry about
  // them.
  uint8_t newEncoding = (lsda.ttypeEncoding & 0xF0) | DW_EH_PE_sdata8;
  expanded.push_back(newEncoding);

  // Calculate new @TType base offset
  // The offset must be increased by the extra bytes from type table expansion
  uint64_t newTtypeOffset = lsda.ttypeBaseOffset + extraBytes;

  // Write new @TType base offset as ULEB128
  size_t origUlebSize = lsda.ttypeBaseOffsetEnd - lsda.ttypeBaseOffsetStart;
  uint8_t ulebBuf[10];
  unsigned ulebSize = encodeULEB128(newTtypeOffset, ulebBuf);
  expanded.append(ulebBuf, ulebBuf + ulebSize);

  // Track ULEB128 size change for offset adjustments
  int64_t ulebSizeDiff = (int64_t)ulebSize - (int64_t)origUlebSize;

  // Copy from after original ULEB128 up to the type table
  // This includes: call site encoding, call site table length, call site table,
  // and action table
  size_t srcOffset = lsda.ttypeBaseOffsetEnd;
  size_t copyLen = lsda.ttypeBase - srcOffset;
  expanded.append(origData.begin() + srcOffset,
                  origData.begin() + srcOffset + copyLen);

  // Expand type table entries from 4 to 8 bytes
  // Type table grows backwards from ttypeBase, so we process from highest index
  for (size_t i = lsda.numTypeEntries; i > 0; --i) {
    // Original entry location: ttypeBase - i * 4 (sdata4 = 4 bytes)
    size_t origOffset = lsda.ttypeBase - i * 4;
    int32_t val32 = 0;
    if (origOffset + 4 <= origData.size()) {
      val32 = read32le(origData.data() + origOffset);
    }
    // Sign-extend to 64-bit
    int64_t val64 = val32;
    uint8_t buf[8];
    write64le(buf, val64);
    expanded.append(buf, buf + 8);
  }

  // Copy any trailing data after the type table
  if (lsda.ttypeBase < origData.size()) {
    expanded.append(origData.begin() + lsda.ttypeBase, origData.end());
  }

  // Update relocations in two passes for clarity:
  // Pass 1: Identify type table relocations and compute their new positions
  // Pass 2: Apply all offset adjustments
  //
  // This avoids the fragile pattern of mutating offsets while computing
  // adjusted positions, which could cause bugs if the iteration order changes.

  // Pre-compute original type table bounds (before any offset adjustments)
  size_t origTypeTableStart = lsda.ttypeBase - lsda.numTypeEntries * 4;
  size_t newTypeTableBase =
      lsda.ttypeBase + ulebSizeDiff + lsda.numTypeEntries * 4;

  // Track which relocations are in the type table and their type indices
  struct RelocUpdate {
    size_t idx;       // Index in relocs array
    size_t typeIdx;   // Type index (1-based, growing backwards from base)
    bool isTypeTable; // Whether this is a type table relocation
  };
  SmallVector<RelocUpdate, 8> updates;

  // Pass 1: Identify relocations and their categories
  for (size_t i = 0; i < sec->relocs().size(); ++i) {
    const Relocation &rel = sec->relocs()[i];
    RelocUpdate update = {i, 0, false};

    // Check if this relocation is in the type table region (using original
    // offsets)
    if (rel.offset >= origTypeTableStart && rel.offset < lsda.ttypeBase) {
      size_t distFromBase = lsda.ttypeBase - rel.offset;
      if (distFromBase % 4 == 0 && distFromBase > 0) {
        update.typeIdx = distFromBase / 4;
        update.isTypeTable = true;
      }
    }
    updates.push_back(update);
  }

  // Pass 2: Apply offset adjustments
  for (const RelocUpdate &update : updates) {
    Relocation &rel = sec->relocs()[update.idx];

    if (update.isTypeTable) {
      // Type table relocation: compute new position in expanded table
      // New entry offset = newTypeTableBase - typeIdx * 8 (sdata8 = 8 bytes)
      rel.offset = newTypeTableBase - update.typeIdx * 8;

      // Update relocation type from PC32 to PC64
      if (rel.type == R_X86_64_PC32) {
        rel.type = R_X86_64_PC64;
      }
    } else if (rel.offset >= lsda.ttypeBaseOffsetEnd) {
      // Non-type-table relocation after the ULEB128: adjust for size change
      rel.offset += ulebSizeDiff;
    }
    // Relocations before ttypeBaseOffsetEnd don't need adjustment
  }

  // Store expanded data in the section
  uint8_t *newData = ctx.bAlloc.Allocate<uint8_t>(expanded.size());
  memcpy(newData, expanded.data(), expanded.size());
  sec->content_ = newData;
  sec->size = expanded.size();

  return true;
}
