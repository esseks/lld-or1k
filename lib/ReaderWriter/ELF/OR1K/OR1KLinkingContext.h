//===--------- lib/ReaderWriter/ELF/OR1K/OR1KLinkingContext.h -------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_LINKING_CONTEXT_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_LINKING_CONTEXT_H

#include "lld/ReaderWriter/ELFLinkingContext.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/ELF.h"


namespace lld {
namespace elf {

class OR1KLinkingContext final : public ELFLinkingContext {
public:
    static const int machine = llvm::ELF::EM_OPENRISC;

    OR1KLinkingContext(llvm::Triple);

    void registerRelocationNames(Registry &r) override;
    void addPasses(PassManager &) override;

    bool isRelativeReloc(const Reference &r) const override;
    bool isCopyRelocation(const Reference &r) const override;
    bool isPLTRelocation(const Reference &r) const override;

    StringRef getDefaultInterpreter() const override {
        return "/usr/lib/ld.so.1";
    }

    StringRef entrySymbolName() const override;
};

} // end namespace elf
} // end namespace lld

#endif
