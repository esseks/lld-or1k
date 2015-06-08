//===- lib/ReaderWriter/ELF/OR1K/OR1KRelocationHandler.h ------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_RELOCATION_HANDLER_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_RELOCATION_HANDLER_H

#include "lld/ReaderWriter/ELFLinkingContext.h"


namespace lld {
namespace elf {

template <typename ELFTy> class OR1KTargetLayout;

class OR1KLinkingContext;

template <class ELFTy>
std::unique_ptr<TargetRelocationHandler>
createOR1KRelocationHandler(OR1KLinkingContext &ctx, OR1KTargetLayout<ELFTy> &layout);

} // namespace elf
} // namespace lld

#endif
