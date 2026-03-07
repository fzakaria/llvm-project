//===- EhFrameReverseRelax.cpp --------------------------------------------===//
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
//===----------------------------------------------------------------------===//

#include "EhFrameReverseRelax.h"
#include "Config.h"
#include "DWARF.h"
#include "InputSection.h"
#include "Relocations.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::dwarf;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

using llvm::support::endian::read32le;

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

namespace {

/// Check if a PC-relative value would overflow a 32-bit signed integer.
bool wouldOverflow32Bit(int64_t pcRel) { return !llvm::isInt<32>(pcRel); }

/// Get the size of FDE encoding, with fallback to default.
size_t getFdeEncodingSize(const CieAugmentationInfo &info, bool is64Bit) {
  if (info.hasFdeEncoding)
    return getSizeForEncoding(info.fdeEncoding, is64Bit);
  return is64Bit ? 8 : 4;
}

/// Compute PC-relative value from a relocation target.
int64_t computePcRelative(Ctx &ctx, const Symbol *sym, int64_t addend,
                          uint64_t pcAddr) {
  if (!sym)
    return 0;
  int64_t targetAddr = sym->getVA(ctx) + addend;
  return targetAddr - pcAddr;
}

} // namespace

//===----------------------------------------------------------------------===//
// Overflow Detection - FDE Initial Location
//===----------------------------------------------------------------------===//

/// Check a single FDE for initial_location overflow.
static bool checkSingleFdeOverflow(Ctx &ctx, EhSectionPiece *fde,
                                   uint64_t ehFrameAddr) {
  if (fde->firstRelocation == (unsigned)-1)
    return false;

  auto *isec = cast<EhInputSection>(fde->sec);
  const auto &reloc = isec->rels[fde->firstRelocation];
  if (!isa<Defined>(reloc.sym))
    return false;

  uint64_t initialLocAddr = ehFrameAddr + fde->outputOff + 8;
  int64_t pcRel =
      computePcRelative(ctx, reloc.sym, reloc.addend, initialLocAddr);

  return wouldOverflow32Bit(pcRel);
}

bool elf::checkFdeInitialLocationOverflow(Ctx &ctx, CieRecord *rec,
                                          uint64_t ehFrameAddr) {
  CieAugmentationInfo info = parseCieAugmentation(rec->cie, ctx.arg.is64);
  if (!info.hasFdeEncoding)
    return false;

  if (getSizeForEncoding(info.fdeEncoding, ctx.arg.is64) != 4)
    return false;

  for (EhSectionPiece *fde : rec->fdes) {
    if (checkSingleFdeOverflow(ctx, fde, ehFrameAddr))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Overflow Detection - Personality Pointer
//===----------------------------------------------------------------------===//

/// Find the personality relocation in a CIE and check for overflow.
static bool checkPersonalityRelocationOverflow(Ctx &ctx, CieRecord *rec,
                                               const CieAugmentationInfo &info,
                                               uint64_t ehFrameAddr) {
  auto *isec = cast<EhInputSection>(rec->cie->sec);

  for (const Relocation &rel : isec->relocs()) {
    if (rel.offset < rec->cie->inputOff ||
        rel.offset >= rec->cie->inputOff + rec->cie->size)
      continue;

    size_t relOffInCie = rel.offset - rec->cie->inputOff;
    if (relOffInCie != info.personalityPointerOffset || !rel.sym)
      continue;

    uint64_t ptrAddr =
        ehFrameAddr + rec->cie->outputOff + info.personalityPointerOffset;
    int64_t pcRel = computePcRelative(ctx, rel.sym, rel.addend, ptrAddr);

    return wouldOverflow32Bit(pcRel);
  }
  return false;
}

bool elf::checkPersonalityPointerOverflow(Ctx &ctx, CieRecord *rec,
                                          uint64_t ehFrameAddr, bool is64Bit) {
  CieAugmentationInfo info = parseCieAugmentation(rec->cie, is64Bit);
  if (!info.hasPersonality || info.personalityPointerSize != 4)
    return false;

  return checkPersonalityRelocationOverflow(ctx, rec, info, ehFrameAddr);
}

//===----------------------------------------------------------------------===//
// Overflow Detection - LSDA Pointer
//===----------------------------------------------------------------------===//

/// Check a single FDE for LSDA pointer overflow.
static bool checkSingleFdeLsdaOverflow(Ctx &ctx, EhSectionPiece *fde,
                                       uint64_t ehFrameAddr,
                                       size_t minLsdaOffset) {
  auto *isec = cast<EhInputSection>(fde->sec);

  for (const Relocation &rel : isec->relocs()) {
    if (rel.offset < fde->inputOff || rel.offset >= fde->inputOff + fde->size)
      continue;

    size_t relOffInFde = rel.offset - fde->inputOff;
    // LSDA is in augmentation data, after header fields.
    if (relOffInFde <= minLsdaOffset || !rel.sym)
      continue;

    uint64_t lsdaAddr = ehFrameAddr + fde->outputOff + relOffInFde;
    int64_t pcRel = computePcRelative(ctx, rel.sym, rel.addend, lsdaAddr);

    if (wouldOverflow32Bit(pcRel))
      return true;
  }
  return false;
}

bool elf::checkLsdaPointerOverflow(Ctx &ctx, CieRecord *rec,
                                   uint64_t ehFrameAddr) {
  CieAugmentationInfo info = parseCieAugmentation(rec->cie, ctx.arg.is64);
  if (!info.hasLsdaEncoding ||
      getSizeForEncoding(info.lsdaEncoding, ctx.arg.is64) != 4)
    return false;

  // Compute the size of fields before augmentation data in FDE:
  //   4 bytes: length field
  //   4 bytes: CIE pointer
  //   N bytes: initial_location (size from FDE encoding)
  //   N bytes: address_range (same size as initial_location)
  // Then: ULEB128 augmentation data length + augmentation data (including LSDA)
  size_t fdeEncodingSize = getFdeEncodingSize(info, ctx.arg.is64);
  size_t minLsdaOffset = 4 + 4 + fdeEncodingSize + fdeEncodingSize;

  for (EhSectionPiece *fde : rec->fdes) {
    if (checkSingleFdeLsdaOverflow(ctx, fde, ehFrameAddr, minLsdaOffset))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Relocation Skip Logic
//===----------------------------------------------------------------------===//

bool elf::shouldSkipCieRelocation(Ctx &ctx, CieRecord *rec,
                                  size_t relOffInCie) {
  if (!rec->needsPersonality64Bit)
    return false;

  CieAugmentationInfo info = parseCieAugmentation(rec->cie, ctx.arg.is64);
  return info.hasPersonality && info.personalityPointerSize == 4 &&
         relOffInCie == info.personalityPointerOffset;
}

bool elf::shouldSkipFdeRelocation(Ctx &ctx, CieRecord *rec,
                                  size_t relOffsetInFde) {
  CieAugmentationInfo info = parseCieAugmentation(rec->cie, ctx.arg.is64);

  // Calculate the size of initial_location/address_range based on FDE encoding
  size_t fdeFieldSize = getFdeEncodingSize(info, ctx.arg.is64);

  // initial_location starts at offset 8 (after length + CIE pointer)
  constexpr size_t kInitialLocStart = 8;
  size_t initialLocEnd = kInitialLocStart + fdeFieldSize;

  // Check if this is the initial_location relocation
  if (rec->needs64BitEncoding && relOffsetInFde >= kInitialLocStart &&
      relOffsetInFde < initialLocEnd)
    return true;

  // address_range comes after initial_location
  size_t addressRangeEnd = initialLocEnd + fdeFieldSize;

  // LSDA pointer is in augmentation data, after address_range + ULEB128 length
  // Any relocation past the address_range is potentially an LSDA pointer
  if (rec->needsLsda64Bit && relOffsetInFde >= addressRangeEnd)
    return true;

  return false;
}

//===----------------------------------------------------------------------===//
// CIE/FDE Writers - Basic Operations
//===----------------------------------------------------------------------===//

void elf::writeCieFde(Ctx &ctx, uint8_t *buf, ArrayRef<uint8_t> d) {
  memcpy(buf, d.data(), d.size());
  // Fix the size field. -4 since size does not include the size field itself.
  write32(ctx, buf, d.size() - 4);
}

int64_t elf::computePersonalityValue(Ctx &ctx, EhSectionPiece *cie,
                                     uint64_t ehFrameAddr,
                                     const CieAugmentationInfo &info) {
  auto *isec = cast<EhInputSection>(cie->sec);
  for (const Relocation &rel : isec->relocs()) {
    if (rel.offset < cie->inputOff || rel.offset >= cie->inputOff + cie->size)
      continue;

    size_t relOffInCie = rel.offset - cie->inputOff;
    if (relOffInCie == info.personalityPointerOffset && rel.sym) {
      uint64_t ptrAddr =
          ehFrameAddr + cie->outputOff + info.personalityPointerOffset;
      return computePcRelative(ctx, rel.sym, rel.addend, ptrAddr);
    }
  }
  return 0;
}

int64_t elf::computeInitialLocation(Ctx &ctx, EhSectionPiece *fde,
                                    uint64_t initialLocFieldAddr) {
  auto *isec = cast<EhInputSection>(fde->sec);
  if (fde->firstRelocation == (unsigned)-1)
    return 0;

  const auto &reloc = isec->rels[fde->firstRelocation];
  if (!isa<Defined>(reloc.sym))
    return 0;

  return computePcRelative(ctx, reloc.sym, reloc.addend, initialLocFieldAddr);
}

//===----------------------------------------------------------------------===//
// CIE Writers - Personality Expansion
//===----------------------------------------------------------------------===//

/// Copy CIE data and update personality encoding for 64-bit.
static void writeCiePersonalityExpansion(Ctx &ctx, uint8_t *buf,
                                         ArrayRef<uint8_t> d,
                                         const CieAugmentationInfo &info,
                                         int64_t personalityVal,
                                         size_t newSize) {
  // Copy everything up to personality pointer
  memcpy(buf, d.data(), info.personalityPointerOffset);

  // Update personality encoding: sdata4 -> sdata8
  buf[info.personalityEncodingOffset] =
      (info.personalityEncoding & 0xF0) | DW_EH_PE_sdata8;

  // Write 8-byte personality pointer
  write64(ctx, buf + info.personalityPointerOffset, personalityVal);

  // Copy the rest after original personality pointer
  size_t afterPersonality =
      info.personalityPointerOffset + info.personalityPointerSize;
  size_t restSize = d.size() - afterPersonality;
  if (restSize > 0)
    memcpy(buf + info.personalityPointerOffset + 8, d.data() + afterPersonality,
           restSize);

  // Update length field
  write32(ctx, buf, newSize - 4);
}

/// Update FDE encoding if present after personality expansion.
static void updateFdeEncodingAfterPersonalityExpansion(
    uint8_t *buf, const CieAugmentationInfo &info, bool is64Bit) {
  if (!info.hasFdeEncoding)
    return;

  size_t offsetAdjust =
      (info.fdeEncodingOffset > info.personalityPointerOffset) ? 4 : 0;
  size_t newFdeEncOffset = info.fdeEncodingOffset + offsetAdjust;
  uint8_t enc = buf[newFdeEncOffset];
  if (getSizeForEncoding(enc, is64Bit) == 4)
    buf[newFdeEncOffset] = (enc & 0xF0) | DW_EH_PE_sdata8;
}

size_t elf::writeCieWithExpandedPersonality(Ctx &ctx, uint8_t *buf,
                                            EhSectionPiece *cie,
                                            uint64_t ehFrameAddr,
                                            const CieAugmentationInfo &info) {
  ArrayRef<uint8_t> d = cie->data();
  size_t newSize = d.size() + 4;

  int64_t personalityVal = computePersonalityValue(ctx, cie, ehFrameAddr, info);
  writeCiePersonalityExpansion(ctx, buf, d, info, personalityVal, newSize);
  updateFdeEncodingAfterPersonalityExpansion(buf, info, ctx.arg.is64);

  return newSize;
}

//===----------------------------------------------------------------------===//
// CIE Writers - Unified Writer
//===----------------------------------------------------------------------===//

size_t elf::writeCie(Ctx &ctx, uint8_t *buf, EhSectionPiece *cie,
                     uint64_t ehFrameAddr, bool expand64BitFdeEnc,
                     bool expandPersonality) {
  ArrayRef<uint8_t> d = cie->data();

  // If no expansion needed, just copy
  if (!expand64BitFdeEnc && !expandPersonality) {
    writeCieFde(ctx, buf, d);
    return d.size();
  }

  // Parse augmentation info once - needed for either expansion type
  CieAugmentationInfo info = parseCieAugmentation(cie, ctx.arg.is64);

  // Check if personality expansion is actually needed
  if (expandPersonality && info.hasPersonality &&
      info.personalityPointerSize == 4)
    return writeCieWithExpandedPersonality(ctx, buf, cie, ehFrameAddr, info);

  // Just need to change FDE encoding (no personality expansion)
  memcpy(buf, d.data(), d.size());
  write32(ctx, buf, d.size() - 4);
  if (expand64BitFdeEnc && info.hasFdeEncoding) {
    uint8_t enc = buf[info.fdeEncodingOffset];
    if (getSizeForEncoding(enc, ctx.arg.is64) == 4)
      buf[info.fdeEncodingOffset] = (enc & 0xF0) | DW_EH_PE_sdata8;
  }

  return d.size();
}

//===----------------------------------------------------------------------===//
// FDE Writers - Pointer Expansion
//===----------------------------------------------------------------------===//

size_t elf::writeExpanded64BitPointers(Ctx &ctx, uint8_t *buf,
                                       EhSectionPiece *fde,
                                       uint64_t ehFrameAddr,
                                       size_t fdeOutputOff,
                                       uint32_t origAddrRange) {
  uint64_t initialLocFieldAddr = ehFrameAddr + fdeOutputOff + 8;
  int64_t initialLoc = computeInitialLocation(ctx, fde, initialLocFieldAddr);

  write64(ctx, buf, initialLoc);
  write64(ctx, buf + 8, origAddrRange);
  return 16;
}

std::pair<size_t, int64_t>
elf::findLsdaRelocation(Ctx &ctx, EhSectionPiece *fde, uint64_t lsdaFieldAddr) {
  auto *isec = cast<EhInputSection>(fde->sec);

  for (const Relocation &rel : isec->relocs()) {
    if (rel.offset < fde->inputOff || rel.offset >= fde->inputOff + fde->size)
      continue;

    size_t relOffInFde = rel.offset - fde->inputOff;
    // LSDA is typically after initial_location (offset 8) and address_range
    if (relOffInFde > 12 && rel.sym) {
      int64_t lsdaVal =
          computePcRelative(ctx, rel.sym, rel.addend, lsdaFieldAddr);
      return {relOffInFde, lsdaVal};
    }
  }
  return {0, 0};
}

void elf::writeExpandedLsda(Ctx &ctx, uint8_t *buf, ArrayRef<uint8_t> d,
                            size_t &writeOff, size_t &readOff,
                            size_t lsdaRelOff, int64_t lsdaVal) {
  // Copy data before LSDA pointer
  size_t beforeLsda = lsdaRelOff - readOff;
  if (beforeLsda > 0) {
    memcpy(buf + writeOff, d.data() + readOff, beforeLsda);
    writeOff += beforeLsda;
    readOff += beforeLsda;
  }

  // Write 8-byte LSDA pointer
  write64(ctx, buf + writeOff, lsdaVal);
  writeOff += 8;
  readOff += 4; // Skip original 4-byte LSDA

  // Copy remaining data
  size_t restSize = d.size() - readOff;
  if (restSize > 0)
    memcpy(buf + writeOff, d.data() + readOff, restSize);
}

//===----------------------------------------------------------------------===//
// FDE Writers - Unified Writer
//===----------------------------------------------------------------------===//

/// Write FDE header (length and CIE pointer).
static void writeFdeHeader(Ctx &ctx, uint8_t *buf, size_t newSize,
                           size_t fdeOutputOff, size_t cieOffset) {
  write32(ctx, buf, newSize - 4);
  write32(ctx, buf + 4, fdeOutputOff + 4 - cieOffset);
}

/// Handle initial_location and address_range fields in FDE.
static void writeFdeLocationFields(Ctx &ctx, uint8_t *buf, ArrayRef<uint8_t> d,
                                   EhSectionPiece *fde, uint64_t ehFrameAddr,
                                   size_t fdeOutputOff, bool expand64BitPointers,
                                   size_t &writeOff, size_t &readOff) {
  if (expand64BitPointers) {
    uint32_t origAddrRange = read32le(d.data() + 12);
    writeExpanded64BitPointers(ctx, buf + writeOff, fde, ehFrameAddr,
                               fdeOutputOff, origAddrRange);
    writeOff += 16;
    readOff += 8;
  } else {
    memcpy(buf + writeOff, d.data() + readOff, 8);
    writeOff += 8;
    readOff += 8;
  }
}

void elf::writeFde(Ctx &ctx, uint8_t *buf, EhSectionPiece *fde,
                   size_t cieOffset, size_t fdeOutputOff, uint64_t ehFrameAddr,
                   bool expand64BitPointers, bool expandLsda,
                   const CieAugmentationInfo &lsdaInfo) {
  ArrayRef<uint8_t> d = fde->data();

  // If no expansion needed, just copy and fix CIE pointer
  if (!expand64BitPointers && !expandLsda) {
    writeCieFde(ctx, buf, d);
    write32(ctx, buf + 4, fdeOutputOff + 4 - cieOffset);
    return;
  }

  constexpr size_t kMinFdeSize = 16;
  if (d.size() < kMinFdeSize) {
    writeCieFde(ctx, buf, d);
    write32(ctx, buf + 4, fdeOutputOff + 4 - cieOffset);
    return;
  }

  // Calculate size increases
  size_t initLocExtra = expand64BitPointers ? 8 : 0;
  size_t lsdaExtra = (expandLsda && lsdaInfo.hasLsdaEncoding) ? 4 : 0;
  size_t newSize = d.size() + initLocExtra + lsdaExtra;

  // Write header
  writeFdeHeader(ctx, buf, newSize, fdeOutputOff, cieOffset);

  size_t writeOff = 8;
  size_t readOff = 8;

  // Handle initial_location and address_range
  writeFdeLocationFields(ctx, buf, d, fde, ehFrameAddr, fdeOutputOff,
                         expand64BitPointers, writeOff, readOff);

  // Handle LSDA expansion if needed
  if (expandLsda && lsdaInfo.hasLsdaEncoding) {
    uint64_t lsdaFieldAddr = ehFrameAddr + fdeOutputOff + writeOff;
    auto [lsdaRelOff, lsdaVal] = findLsdaRelocation(ctx, fde, lsdaFieldAddr);

    if (lsdaRelOff > 0 && lsdaRelOff >= readOff) {
      writeExpandedLsda(ctx, buf, d, writeOff, readOff, lsdaRelOff, lsdaVal);
      return;
    }
  }

  // Copy remaining data
  size_t restSize = d.size() - readOff;
  if (restSize > 0)
    memcpy(buf + writeOff, d.data() + readOff, restSize);
}
