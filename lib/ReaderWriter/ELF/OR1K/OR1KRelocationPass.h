//===- lib/ReaderWriter/ELF/OR1K/OR1KRelocationPass.h ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_RELOCATION_PASS_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_RELOCATION_PASS_H

#include <memory>

namespace lld {

class Pass;

namespace elf {

class OR1KLinkingContext;

std::unique_ptr<Pass> createOR1KRelocationPass(OR1KLinkingContext &ctx);

} // namespace elf
} // namespace lld

#endif
