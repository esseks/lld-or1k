//===- lib/ReaderWriter/ELF/OR1K/OR1KDynamicLibraryWriter.h ---------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_DYNAMIC_LIBRARY_WRITER_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_DYNAMIC_LIBRARY_WRITER_H

#include "DynamicLibraryWriter.h"
#include "OR1KLinkingContext.h"

namespace lld {
namespace elf {

class OR1KGlobalOffsetTableAtom : public GlobalOffsetTableAtom {
public:
  OR1KGlobalOffsetTableAtom(const File &f) : GlobalOffsetTableAtom(f) {}

  Alignment alignment() const override { return 4; }
};

template <typename ELFTy>
class OR1KDynamicLibraryWriter : public DynamicLibraryWriter<ELFTy> {
public:
  OR1KDynamicLibraryWriter(OR1KLinkingContext &ctx, TargetLayout<ELFTy> &layout)
      : DynamicLibraryWriter<ELFTy>(ctx, layout) {}

protected:
  void
  createImplicitFiles(std::vector<std::unique_ptr<File>> &result) override {
    DynamicLibraryWriter<ELFTy>::createImplicitFiles(result);
    auto gotFile = llvm::make_unique<lld::SimpleFile>("GOTFile");
    gotFile->addAtom(*new (gotFile->allocator())
                         OR1KGlobalOffsetTableAtom(*gotFile));
    gotFile->addAtom(*new (gotFile->allocator()) DynamicAtom(*gotFile));
    result.push_back(std::move(gotFile));
  }
};

} // namespace elf
} // namespace lld

#endif
