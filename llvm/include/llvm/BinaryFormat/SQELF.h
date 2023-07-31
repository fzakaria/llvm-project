#ifndef LLVM_BINARYFORMAT_SQELF_H
#define LLVM_BINARYFORMAT_SQELF_H

#include "llvm/Support/raw_ostream.h"
#include <sqlite3.h>

namespace llvm {
namespace BinaryFormat {
class SQELF {

public:
  typedef struct Rela {
    uint64_t id,      // Section Header index
    uint64_t r_offset; // Location (file byte offset, or program virtual addr).
    uint64_t r_info;  // Symbol table index and type of relocation to apply.
    int64_t r_addend; // Compute value for relocatable field by adding this.
  } Rela;

  typedef struct Ins {
    uint64_t id, // Section Header index
    uint32_t address;
    std::string mnemonic;
    std::string operand1;
    std::string operand2;
    std::string operand3;
  } Ins;

  typed def
  SQELF();
  SQELF(const Ins &I);
  virtual ~SQELF();

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const SQELF &BF);

private:
  sqlite3 *DB;
  Rela R;
  Ins I;
};
} // namespace BinaryFormat
} // namespace llvm

#endif // LLVM_BINARYFORMAT_SQELF_H