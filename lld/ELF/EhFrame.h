//===- EhFrame.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_EHFRAME_H
#define LLD_ELF_EHFRAME_H

namespace lld::elf {
struct EhSectionPiece;
struct Ctx;

uint8_t getFdeEncoding(EhSectionPiece *p);
bool hasLSDA(const EhSectionPiece &p);

//===----------------------------------------------------------------------===//
// CIE Augmentation Parsing for Reverse Relaxation
//
// The CIE augmentation string specifies what additional data is present:
// - 'z': Augmentation data length follows (ULEB128)
// - 'P': Personality encoding (1 byte) + personality pointer (variable)
// - 'L': LSDA encoding (1 byte)
// - 'R': FDE pointer encoding (1 byte)
//
// CieAugmentationInfo provides a unified structure containing all augmentation
// data extracted in a single parse of the CIE.
//===----------------------------------------------------------------------===//

// Combined information about all CIE augmentation data.
struct CieAugmentationInfo {
  bool valid = false; // Whether parsing succeeded

  // 'R' - FDE pointer encoding
  bool hasFdeEncoding = false;
  size_t fdeEncodingOffset = 0;
  uint8_t fdeEncoding = 0;

  // 'L' - LSDA encoding
  bool hasLsdaEncoding = false;
  size_t lsdaEncodingOffset = 0;
  uint8_t lsdaEncoding = 0;

  // 'P' - Personality
  bool hasPersonality = false;
  size_t personalityEncodingOffset = 0;
  uint8_t personalityEncoding = 0;
  size_t personalityPointerOffset = 0;
  size_t personalityPointerSize = 0;
};

// Parse CIE augmentation and return all augmentation information.
CieAugmentationInfo parseCieAugmentation(EhSectionPiece *cie, bool is64Bit);

} // namespace lld::elf

#endif
