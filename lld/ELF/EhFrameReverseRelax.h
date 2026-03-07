//===- EhFrameReverseRelax.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reverse relaxation for .eh_frame section.
//
// When sections are placed more than 2GB apart, PC-relative relocations
// using 32-bit signed values can overflow. This file implements "reverse
// relaxation" - automatic expansion of 32-bit encodings to 64-bit when
// overflow would occur.
//
// Specifically handles:
// 1. FDE initial_location overflow: Expands from 4 to 8 bytes
// 2. CIE personality pointer overflow: Expands from 4 to 8 bytes
// 3. FDE LSDA pointer overflow: Expands from 4 to 8 bytes
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_EHFRAMEREVERSERELAXA_H
#define LLD_ELF_EHFRAMEREVERSERELAXA_H

#include "EhFrame.h"
#include "InputSection.h"
#include <cstddef>
#include <cstdint>

namespace lld::elf {

struct Ctx;
struct CieRecord;
class EhInputSection;
struct EhSectionPiece;

//===----------------------------------------------------------------------===//
// Overflow Detection
//
// These functions check if various .eh_frame fields would overflow when
// using 32-bit PC-relative encodings due to large distances between sections.
//===----------------------------------------------------------------------===//

/// Check if any FDE in the CIE record has an initial_location that would
/// overflow a 32-bit PC-relative encoding.
bool checkFdeInitialLocationOverflow(Ctx &ctx, CieRecord *rec,
                                     uint64_t ehFrameAddr);

/// Check if the CIE's personality pointer would overflow a 32-bit
/// PC-relative encoding.
bool checkPersonalityPointerOverflow(Ctx &ctx, CieRecord *rec,
                                     uint64_t ehFrameAddr, bool is64Bit);

/// Check if any FDE's LSDA pointer in the CIE record would overflow
/// a 32-bit PC-relative encoding.
bool checkLsdaPointerOverflow(Ctx &ctx, CieRecord *rec, uint64_t ehFrameAddr);

//===----------------------------------------------------------------------===//
// Relocation Skip Logic
//
// When reverse relaxation expands fields, the original relocations targeting
// those fields must be skipped (they're re-written by expansion code).
//===----------------------------------------------------------------------===//

/// Check if a relocation at the given offset within a CIE should be skipped
/// because it targets a personality pointer that will be expanded.
bool shouldSkipCieRelocation(Ctx &ctx, CieRecord *rec, size_t relOffInCie);

/// Check if a relocation at the given offset within an FDE should be skipped
/// because it targets a field (initial_location or LSDA) that will be expanded.
bool shouldSkipFdeRelocation(Ctx &ctx, CieRecord *rec, size_t relOffsetInFde);

//===----------------------------------------------------------------------===//
// CIE/FDE Writers
//
// These functions write CIE and FDE records, handling expansion when needed.
//===----------------------------------------------------------------------===//

/// Write a CIE/FDE with the standard format (no expansion).
void writeCieFde(Ctx &ctx, uint8_t *buf, ArrayRef<uint8_t> d);

/// Compute the 64-bit personality pointer value from relocations.
int64_t computePersonalityValue(Ctx &ctx, EhSectionPiece *cie,
                                uint64_t ehFrameAddr,
                                const CieAugmentationInfo &info);

/// Write a CIE with expanded personality pointer (4 -> 8 bytes).
/// Returns the new CIE size.
size_t writeCieWithExpandedPersonality(Ctx &ctx, uint8_t *buf,
                                       EhSectionPiece *cie,
                                       uint64_t ehFrameAddr,
                                       const CieAugmentationInfo &info);

/// Unified CIE writer that handles all expansion cases.
/// - expand64BitFdeEnc: Change 'R' augmentation from sdata4 to sdata8
/// - expandPersonality: Expand personality pointer from 4 to 8 bytes
/// Returns the new CIE size.
size_t writeCie(Ctx &ctx, uint8_t *buf, EhSectionPiece *cie,
                uint64_t ehFrameAddr, bool expand64BitFdeEnc,
                bool expandPersonality);

/// Compute the 64-bit initial_location value from the FDE's relocation.
int64_t computeInitialLocation(Ctx &ctx, EhSectionPiece *fde,
                               uint64_t initialLocFieldAddr);

/// Write expanded 64-bit initial_location and address_range fields.
/// Returns the number of bytes written (16).
size_t writeExpanded64BitPointers(Ctx &ctx, uint8_t *buf, EhSectionPiece *fde,
                                  uint64_t ehFrameAddr, size_t fdeOutputOff,
                                  uint32_t origAddrRange);

/// Find the LSDA relocation within an FDE and compute its expanded value.
/// Returns a pair of (relocation offset within FDE, computed LSDA value).
std::pair<size_t, int64_t> findLsdaRelocation(Ctx &ctx, EhSectionPiece *fde,
                                              uint64_t lsdaFieldAddr);

/// Write the FDE with expanded LSDA pointer.
void writeExpandedLsda(Ctx &ctx, uint8_t *buf, ArrayRef<uint8_t> d,
                       size_t &writeOff, size_t &readOff, size_t lsdaRelOff,
                       int64_t lsdaVal);

/// Unified FDE writer that handles all expansion cases.
/// - expand64BitPointers: Expand initial_location and address_range to 8 bytes
/// - expandLsda: Expand LSDA pointer from 4 to 8 bytes
void writeFde(Ctx &ctx, uint8_t *buf, EhSectionPiece *fde, size_t cieOffset,
              size_t fdeOutputOff, uint64_t ehFrameAddr,
              bool expand64BitPointers, bool expandLsda,
              const CieAugmentationInfo &lsdaInfo);

} // namespace lld::elf

#endif // LLD_ELF_EHFRAMEREVERSERELAXA_H
