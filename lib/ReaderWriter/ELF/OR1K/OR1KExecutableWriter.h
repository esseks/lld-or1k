//===- lib/ReaderWriter/ELF/OR1K/OR1KExecutableWriter.h -------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_EXECUTABLE_WRITER_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_EXECUTABLE_WRITER_H

#include "ExecutableWriter.h"
#include "OR1KLinkingContext.h"

namespace lld {
namespace elf {

template <class ELFTy>
class OR1KExecutableWriter : public ExecutableWriter<ELFTy> {
public:
  OR1KExecutableWriter(OR1KLinkingContext &ctx, TargetLayout<ELFTy> &layout)
      : ExecutableWriter<ELFTy>(ctx, layout) {}
};

} // namespace elf
} // namespace lld

#endif
