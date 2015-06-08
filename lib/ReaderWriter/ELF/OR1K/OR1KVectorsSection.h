//===- lib/ReaderWriter/ELF/OR1K/OR1KVectorsSection.h ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_VECTORS_SECTION_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_VECTORS_SECTION_H

#include "OR1KLinkingContext.h"
#include "OR1KTargetHandler.h"
#include "TargetLayout.h"

namespace lld {
namespace elf {

template <typename ELFTy> class OR1KVectorsSection : public AtomSection<ELFTy> {
public:
  OR1KVectorsSection(ELFLinkingContext const &ctx)
      : AtomSection<ELFTy>(ctx, ".vectors", DefinedAtom::typeCode,
                           DefinedAtom::permR_X,
                           OR1KTargetLayout<ELFTy>::ORDER_VECTORS) {}

  bool hasOutputSegment() const override { return true; }

  void assignVirtualAddress(uint64_t) override {
    // .vectors section sits at virtual address 0
    AtomSection<ELFTy>::assignVirtualAddress(0);
  }

  void assignFileOffsets(uint64_t offset) override {
    assert(!this->atoms().empty() && ".vectors section should not be empty");

    const lld::AtomLayout *paddingLayout = this->atoms().front();
    const lld::DefinedAtom *paddingAtom =
        llvm::cast<lld::DefinedAtom>(paddingLayout->_atom);

    assert(offset < paddingAtom->size() && "ELF header too big");
    assert(paddingLayout->_fileOffset == 0
        && paddingLayout->_virtualAddr == 0
        && ".vectors padding is not the first atom in file!");

    // Simply remove the padding atom. All the other offsets are ok.
    this->_atoms.erase(this->_atoms.begin());
  }
};

} // namespace elf
} // namespace lld

#endif
