#ifndef LLVM_BINARYFORMAT_SQELF_H
#define LLVM_BINARYFORMAT_SQELF_H

#include "llvm/MC/MCInst.h"
#include "llvm/Support/raw_ostream.h"
#include <sqlite3.h>

namespace llvm {
namespace BinaryFormat {
class SQELF {

public:
  typedef struct Rela {
    uint64_t r_offset; // Location (file byte offset, or program virtual addr).
    uint64_t r_info;   // Symbol table index and type of relocation to apply.
    int64_t r_addend;  // Compute value for relocatable field by adding this.
  } Rela;

  typedef struct Ins {
    uint64_t address;
    llvm::StringRef mnemonic;
    llvm::MCOperand operand1;
    llvm::MCOperand operand2;
    llvm::MCOperand operand3;
  } Ins;

  SQELF();
  virtual ~SQELF();

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const SQELF &BF);

private:
  sqlite3 *DB;
};
} // namespace BinaryFormat
} // namespace llvm

#endif // LLVM_BINARYFORMAT_SQELF_H