//===- GccExceptTable.h ------------------------------------------*- C++
//-*-===//
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

#ifndef LLD_ELF_GCCEXCEPTTABLE_H
#define LLD_ELF_GCCEXCEPTTABLE_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include <optional>

namespace lld::elf {
class InputSection;
struct Ctx;

//===----------------------------------------------------------------------===//
// LSDA (Language-Specific Data Area) Parsing
//
// The LSDA format is documented in the Itanium C++ ABI Exception Handling spec.
// It contains:
// - Landing pad start pointer
// - Type table encoding and entries
// - Call site table
// - Action table
//===----------------------------------------------------------------------===//

// Parsed LSDA structure from .gcc_except_table.
struct LSDAParsed {
  // Offset of @LPStart encoding byte
  size_t lpStartEncodingOffset = 0;

  // Offset of @TType encoding byte
  size_t ttypeEncodingOffset = 0;

  // The @TType encoding value
  uint8_t ttypeEncoding = 0;

  // Offset of the @TType base offset ULEB128 field (if present)
  size_t ttypeBaseOffsetStart = 0;

  // The decoded @TType base offset value
  uint64_t ttypeBaseOffset = 0;

  // End of @TType base offset ULEB128 (where call site encoding starts)
  size_t ttypeBaseOffsetEnd = 0;

  // Call site table encoding byte offset
  size_t callSiteEncodingOffset = 0;
  uint8_t callSiteEncoding = 0;

  // Call site table bounds
  size_t callSiteTableLengthOffset = 0;
  size_t callSiteTableStart = 0;
  size_t callSiteTableEnd = 0;

  // Action table follows call site table
  size_t actionTableStart = 0;

  // Type table location (entries grow backwards from ttypeBase)
  // ttypeBase = callSiteTableStart + ttypeBaseOffset
  size_t ttypeBase = 0;

  // Size of each type entry (4 or 8 bytes)
  size_t typeTableEntrySize = 0;

  // Number of type entries (computed from relocations during expansion)
  size_t numTypeEntries = 0;
};

// Parse an LSDA from .gcc_except_table content.
// Returns std::nullopt if parsing fails, otherwise returns the parsed
// structure.
std::optional<LSDAParsed> parseLSDA(ArrayRef<uint8_t> data, bool is64Bit);

// Perform reverse relaxation on a .gcc_except_table section.
// If type table entries would overflow 32-bit, expand to 64-bit.
// Returns true if the section was modified.
bool reverseRelaxGccExceptTable(Ctx &ctx, InputSection *sec);

} // namespace lld::elf

#endif
