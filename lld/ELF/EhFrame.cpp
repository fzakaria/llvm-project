//===- EhFrame.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// .eh_frame section contains information on how to unwind the stack when
// an exception is thrown. The section consists of sequence of CIE and FDE
// records. The linker needs to merge CIEs and associate FDEs to CIEs.
// That means the linker has to understand the format of the section.
//
// This file contains a few utility functions to read .eh_frame contents.
//
//===----------------------------------------------------------------------===//

#include "EhFrame.h"
#include "Config.h"
#include "DWARF.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSections.h"
#include "Relocations.h"
#include "Target.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::dwarf;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

namespace {
class EhReader {
public:
  EhReader(InputSectionBase *s, ArrayRef<uint8_t> d, bool is64Bit = false)
      : isec(s), d(d), is64Bit(is64Bit) {}
  uint8_t getFdeEncoding();
  bool hasLSDA();
  CieAugmentationInfo getCieAugmentationInfo();

private:
  template <class P> void errOn(const P *loc, const Twine &msg) {
    Ctx &ctx = isec->file->ctx;
    Err(ctx) << "corrupted .eh_frame: " << msg << "\n>>> defined in "
             << isec->getObjMsg((const uint8_t *)loc - isec->content().data());
  }

  uint8_t readByte();
  void skipBytes(size_t count);
  StringRef readString();
  void skipLeb128();
  void skipAugP();
  StringRef getAugmentation();

  InputSectionBase *isec;
  ArrayRef<uint8_t> d;
  bool is64Bit;
};
} // namespace

// Read a byte and advance D by one byte.
uint8_t EhReader::readByte() {
  if (d.empty()) {
    errOn(d.data(), "unexpected end of CIE");
    return 0;
  }
  uint8_t b = d.front();
  d = d.slice(1);
  return b;
}

void EhReader::skipBytes(size_t count) {
  if (d.size() < count)
    errOn(d.data(), "CIE is too small");
  else
    d = d.slice(count);
}

// Read a null-terminated string.
StringRef EhReader::readString() {
  const uint8_t *end = llvm::find(d, '\0');
  if (end == d.end()) {
    errOn(d.data(), "corrupted CIE (failed to read string)");
    return {};
  }
  StringRef s = toStringRef(d.slice(0, end - d.begin()));
  d = d.slice(s.size() + 1);
  return s;
}

// Skip an integer encoded in the LEB128 format.
// Actual number is not of interest because only the runtime needs it.
// But we need to be at least able to skip it so that we can read
// the field that follows a LEB128 number.
void EhReader::skipLeb128() {
  const uint8_t *errPos = d.data();
  while (!d.empty()) {
    uint8_t val = d.front();
    d = d.slice(1);
    if ((val & 0x80) == 0)
      return;
  }
  errOn(errPos, "corrupted CIE (failed to read LEB128)");
}

void EhReader::skipAugP() {
  uint8_t enc = readByte();
  if ((enc & 0xf0) == DW_EH_PE_aligned)
    return errOn(d.data() - 1, "DW_EH_PE_aligned encoding is not supported");
  size_t size = getSizeForEncoding(enc, isec->getCtx().arg.is64);
  if (size == 0)
    return errOn(d.data() - 1, "unknown FDE encoding");
  if (size >= d.size())
    return errOn(d.data() - 1, "corrupted CIE");
  d = d.slice(size);
}

CieAugmentationInfo EhReader::getCieAugmentationInfo() {
  CieAugmentationInfo result;
  size_t startSize = d.size();

  // Skip length (4 bytes) + CIE ID (4 bytes)
  if (d.size() < 8)
    return result;
  d = d.slice(8);

  // Read version
  if (d.empty())
    return result;
  int version = d.front();
  d = d.slice(1);
  if (version != 1 && version != 3)
    return result;

  // Find end of augmentation string
  const uint8_t *end = llvm::find(d, '\0');
  if (end == d.end())
    return result;
  StringRef aug = toStringRef(d.slice(0, end - d.begin()));
  d = d.slice(aug.size() + 1);

  // Skip code alignment factor (ULEB128)
  unsigned n = 0;
  decodeULEB128(d.data(), &n, d.data() + d.size());
  if (n == 0)
    return result;
  d = d.slice(n);

  // Skip data alignment factor (SLEB128)
  n = 0;
  decodeSLEB128(d.data(), &n, d.data() + d.size());
  if (n == 0)
    return result;
  d = d.slice(n);

  // Skip return address register
  if (version == 1) {
    if (d.empty())
      return result;
    d = d.slice(1);
  } else {
    n = 0;
    decodeULEB128(d.data(), &n, d.data() + d.size());
    if (n == 0)
      return result;
    d = d.slice(n);
  }

  // Process augmentation data - extract all info in one pass
  for (char c : aug) {
    if (d.empty())
      return result;
    if (c == 'z') {
      // Skip augmentation data length (ULEB128)
      n = 0;
      decodeULEB128(d.data(), &n, d.data() + d.size());
      if (n == 0)
        return result;
      d = d.slice(n);
    } else if (c == 'L') {
      // LSDA encoding
      result.hasLsdaEncoding = true;
      result.lsdaEncodingOffset = startSize - d.size();
      result.lsdaEncoding = d.front();
      d = d.slice(1);
    } else if (c == 'P') {
      // Personality encoding + pointer
      result.hasPersonality = true;
      result.personalityEncodingOffset = startSize - d.size();
      result.personalityEncoding = d.front();
      d = d.slice(1);
      result.personalityPointerOffset = startSize - d.size();
      result.personalityPointerSize =
          getSizeForEncoding(result.personalityEncoding, is64Bit);
      if (d.size() < result.personalityPointerSize)
        return result;
      d = d.slice(result.personalityPointerSize);
    } else if (c == 'R') {
      // FDE encoding
      result.hasFdeEncoding = true;
      result.fdeEncodingOffset = startSize - d.size();
      result.fdeEncoding = d.front();
      d = d.slice(1);
    }
  }

  result.valid = true;
  return result;
}

uint8_t elf::getFdeEncoding(EhSectionPiece *p) {
  return EhReader(p->sec, p->data()).getFdeEncoding();
}

CieAugmentationInfo elf::parseCieAugmentation(EhSectionPiece *p, bool is64Bit) {
  return EhReader(p->sec, p->data(), is64Bit).getCieAugmentationInfo();
}

bool elf::hasLSDA(const EhSectionPiece &p) {
  return EhReader(p.sec, p.data()).hasLSDA();
}

StringRef EhReader::getAugmentation() {
  skipBytes(8);
  int version = readByte();
  if (version != 1 && version != 3) {
    errOn(d.data() - 1,
          "FDE version 1 or 3 expected, but got " + Twine(version));
    return {};
  }

  StringRef aug = readString();

  // Skip code and data alignment factors.
  skipLeb128();
  skipLeb128();

  // Skip the return address register. In CIE version 1 this is a single
  // byte. In CIE version 3 this is an unsigned LEB128.
  if (version == 1)
    readByte();
  else
    skipLeb128();
  return aug;
}

uint8_t EhReader::getFdeEncoding() {
  // We only care about an 'R' value, but other records may precede an 'R'
  // record. Unfortunately records are not in TLV (type-length-value) format,
  // so we need to teach the linker how to skip records for each type.
  StringRef aug = getAugmentation();
  for (char c : aug) {
    if (c == 'R')
      return readByte();
    if (c == 'z')
      skipLeb128();
    else if (c == 'L')
      readByte();
    else if (c == 'P')
      skipAugP();
    else if (c != 'B' && c != 'S' && c != 'G') {
      errOn(aug.data(), "unknown .eh_frame augmentation string: " + aug);
      break;
    }
  }
  return DW_EH_PE_absptr;
}

bool EhReader::hasLSDA() {
  StringRef aug = getAugmentation();
  for (char c : aug) {
    if (c == 'L')
      return true;
    if (c == 'z')
      skipLeb128();
    else if (c == 'P')
      skipAugP();
    else if (c == 'R')
      readByte();
    else if (c != 'B' && c != 'S' && c != 'G') {
      errOn(aug.data(), "unknown .eh_frame augmentation string: " + aug);
      break;
    }
  }
  return false;
}
