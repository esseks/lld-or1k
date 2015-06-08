//===--------- lib/ReaderWriter/ELF/OR1K/OR1KLinkingContext.cpp -----------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "OR1KTargetHandler.h"
#include "OR1KRelocationPass.h"
#include "OR1KLinkingContext.h"

using lld::elf::OR1KLinkingContext;

std::unique_ptr<lld::ELFLinkingContext>
lld::elf::createOR1KLinkingContext(llvm::Triple triple) {
  if (triple.getArch() == llvm::Triple::or1k ||
      triple.getArch() == llvm::Triple::or1kle) {
    return llvm::make_unique<OR1KLinkingContext>(triple);
  }

  return nullptr;
}

using lld::elf::createOR1KTargetHandler;

OR1KLinkingContext::OR1KLinkingContext(llvm::Triple triple)
    : lld::ELFLinkingContext(triple, createOR1KTargetHandler(triple, *this)) {}

lld::StringRef OR1KLinkingContext::entrySymbolName() const {
  if (_outputELFType == llvm::ELF::ET_EXEC && _entrySymbolName.empty()) {
    return "_or1k_start";
  }

  return _entrySymbolName;
}

void OR1KLinkingContext::addPasses(lld::PassManager &pm) {
  auto pass = lld::elf::createOR1KRelocationPass(*this);
  if (pass)
    pm.add(std::move(pass));
  ELFLinkingContext::addPasses(pm);
}

// Needed for the following macro magic
using namespace llvm::ELF;

static const lld::Registry::KindStrings kindStrings[] = {
    #define ELF_RELOC(name, value) LLD_KIND_STRING_ENTRY(name),
    #include "llvm/Support/ELFRelocs/OR1K.def"
    #undef ELF_RELOC
    LLD_KIND_STRING_END
};

void OR1KLinkingContext::registerRelocationNames(lld::Registry &registry) {
  registry.addKindTable(lld::Reference::KindNamespace::ELF,
                        lld::Reference::KindArch::OR1K, kindStrings);
}

bool OR1KLinkingContext::isRelativeReloc(lld::Reference const &r) const {
  if (r.kindNamespace() != lld::Reference::KindNamespace::ELF) {
    return false;
  }

  assert(r.kindArch() == lld::Reference::KindArch::OR1K);

  switch (r.kindValue()) {
  case llvm::ELF::R_OR1K_INSN_REL_26:
  case llvm::ELF::R_OR1K_32_PCREL:
  case llvm::ELF::R_OR1K_16_PCREL:
  case llvm::ELF::R_OR1K_8_PCREL:
  case llvm::ELF::R_OR1K_GOTPC_HI16:
  case llvm::ELF::R_OR1K_GOTPC_LO16:
  case llvm::ELF::R_OR1K_RELATIVE:
    return true;
  default:
    return false;
  }
}

bool OR1KLinkingContext::isCopyRelocation(lld::Reference const &r) const {
  if (r.kindNamespace() != lld::Reference::KindNamespace::ELF) {
    return false;
  }

  assert(r.kindArch() == lld::Reference::KindArch::OR1K);

  switch (r.kindValue()) {
  case llvm::ELF::R_OR1K_COPY:
    return true;
  default:
    return false;
  }
}

bool OR1KLinkingContext::isPLTRelocation(lld::Reference const &r) const {
  if (r.kindNamespace() != lld::Reference::KindNamespace::ELF) {
    return false;
  }

  assert(r.kindArch() == lld::Reference::KindArch::OR1K);

  switch (r.kindValue()) { // TODO Check
  case llvm::ELF::R_OR1K_PLT26:
    return true;
  default:
    return false;
  }
}
