//===- lib/ReaderWriter/ELF/OR1K/OR1KTargetHandler.cpp --------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "OR1KTargetHandler.h"
#include "OR1KDynamicLibraryWriter.h"
#include "OR1KExecutableWriter.h"
#include "OR1KLinkingContext.h"
#include "OR1KRelocationHandler.h"

#include "lld/ReaderWriter/ELFLinkingContext.h"


using namespace lld::elf;


std::unique_ptr<lld::TargetHandler> lld::elf::createOR1KTargetHandler(
    llvm::Triple const& triple, OR1KLinkingContext &ctx
) {
    if (triple.getArch() == llvm::Triple::or1k) {
        return llvm::make_unique<OR1KTargetHandler<OR1KBEType>>(ctx);
    } else if (triple.getArch() == llvm::Triple::or1kle) {
        return llvm::make_unique<OR1KTargetHandler<OR1KLEType>>(ctx);
    }

    return nullptr;
}


template <typename ELFTy>
std::unique_ptr<lld::Writer> OR1KTargetHandler<ELFTy>::getWriter() {
    switch (_ctx.getOutputELFType()) {
    case llvm::ELF::ET_EXEC:
        return llvm::make_unique<OR1KExecutableWriter<ELFTy>>(
            _ctx, *_targetLayout
        );

    case llvm::ELF::ET_DYN:
        return llvm::make_unique<OR1KDynamicLibraryWriter<ELFTy>>(
            _ctx, *_targetLayout
        );

    case llvm::ELF::ET_REL:
        llvm_unreachable("TODO: support -r mode");

    default:
        llvm_unreachable("unsupported output type");
    }
}
