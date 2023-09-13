#ifndef LLVM_MC_MCSQELFOBJECTWRITER_H
#define LLVM_MC_MCSQELFOBJECTWRITER_H

#include "llvm/MC/MCObjectWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCSectionELF.h"
#include <memory>

namespace llvm {
class MCAssembler;
class MCContext;
class MCFixup;
class MCSymbol;
class MCSymbolELF;
class MCValue;

struct SQELFRelocationEntry {
  uint64_t Offset; // Where is the relocation.
  const MCSymbolELF *Symbol; // The symbol to relocate with.
  unsigned Type;   // The type of the relocation.
  uint64_t Addend; // The addend to use.
  const MCSymbolELF *OriginalSymbol; // The original value of Symbol if we changed it.
  uint64_t OriginalAddend; // The original value of addend.

  SQELFRelocationEntry(uint64_t Offset, const MCSymbolELF *Symbol, unsigned Type,
                     uint64_t Addend, const MCSymbolELF *OriginalSymbol,
                     uint64_t OriginalAddend)
      : Offset(Offset), Symbol(Symbol), Type(Type), Addend(Addend),
        OriginalSymbol(OriginalSymbol), OriginalAddend(OriginalAddend) {}

  void print(raw_ostream &Out) const {
    Out << "Off=" << Offset << ", Sym=" << Symbol << ", Type=" << Type
        << ", Addend=" << Addend << ", OriginalSymbol=" << OriginalSymbol
        << ", OriginalAddend=" << OriginalAddend;
  }

  LLVM_DUMP_METHOD void dump() const { print(errs()); }
};

class MCSQELFObjectTargetWriter : public MCObjectTargetWriter {
  const uint8_t OSABI;
  const uint8_t ABIVersion;
  const uint16_t EMachine;
  const unsigned HasRelocationAddend : 1;
  const unsigned Is64Bit : 1;

public:
  explicit MCSQELFObjectTargetWriter(bool Is64Bit_, uint8_t OSABI_,
                                                 uint16_t EMachine_,
                                                 bool HasRelocationAddend_,
                                                 uint8_t ABIVersion_ = 0);
  virtual ~MCSQELFObjectTargetWriter();

  Triple::ObjectFormatType getFormat() const override { return Triple::SQELF; }

  static bool classof(const MCObjectTargetWriter *W) {
    return W->getFormat() == Triple::SQELF;
  }
  
  virtual unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                              const MCFixup &Fixup, bool IsPCRel) const = 0;

  virtual bool needsRelocateWithSymbol(const MCSymbol &Sym,
                                       unsigned Type) const;
                                       
  virtual void addTargetSectionFlags(MCContext &Ctx, MCSectionELF &Sec);
  bool hasRelocationAddend() const { return HasRelocationAddend; }

};

/// Construct a new SQELF writer instance.
///
/// \param MOTW - The target specific Wasm writer subclass.
/// \param OS - The stream to write to.
/// \returns The constructed object writer.
std::unique_ptr<MCObjectWriter>
createSQELFObjectWriter(std::unique_ptr<MCSQELFObjectTargetWriter> MOTW,
                        raw_pwrite_stream &OS, bool IsLittleEndian);

} // namespace llvm

#endif // LLVM_MC_MCSQELFOBJECTWRITER_H
