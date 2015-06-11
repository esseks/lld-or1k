//===--------- lib/ReaderWriter/ELF/OR1K/OR1KRelocationPass.cpp -----------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "OR1KRelocationPass.h"
#include "OR1KLinkingContext.h"

#include "Atoms.h"
#include "llvm/ADT/STLExtras.h"
#include <memory>

using namespace lld::elf;
using llvm::isa;
using llvm::dyn_cast_or_null;
using llvm::dyn_cast;

namespace {

#define DEF_ATOM(name, base, ...)                                              \
  static const uint8_t name##_content[] = {__VA_ARGS__};                       \
  class name : public lld::elf::base {                                         \
  public:                                                                      \
    using base::base;                                                          \
                                                                               \
    llvm::ArrayRef<uint8_t> rawContent() const override {                      \
      return llvm::makeArrayRef(name##_content);                               \
    }                                                                          \
                                                                               \
    lld::DefinedAtom::Alignment alignment() const override { return 4; }       \
  }

DEF_ATOM(
    OR1KGOTAtom, GOTAtom,
    0x00, 0x00, 0x00, 0x00
);

DEF_ATOM(
    OR1KPLT0Atom, PLT0Atom,
    0x19, 0x80, 0x00, 0x00, // l.movhi r12, 0    ; hi(.got+4)
    0xa9, 0x8c, 0x00, 0x00, // l.ori r12, r12, 0 ; lo(.got+4)
    0x85, 0xec, 0x00, 0x04, // l.lwz r15, 4(r12) ; *(.got+8)
    0x44, 0x00, 0x78, 0x00, // l.jr r15
    0x85, 0x8c, 0x00, 0x00  // l.lwz r12, 0(r12)
);

DEF_ATOM(
    OR1KPLTAtom, PLTAtom,
    0x19, 0x80, 0x00, 0x00, // l.movhi r12, 0    ; hi(got idx addr)
    0xa9, 0x8c, 0x00, 0x00, // l.ori r12, r12, 0 ; lo(got idx addr)
    0x85, 0x8c, 0x00, 0x00, // l.lwz r12, 0(r12)
    0x44, 0x00, 0x60, 0x00, // l.jr r12
    0xa9, 0x60, 0x00, 0x00  // l.ori r11, r0, 0  ; reloc offset
);

DEF_ATOM(
    OR1KPLT0PICAtom, PLT0Atom,
    0x85, 0x90, 0x00, 0x04, // l.lwz r12, 4(r16)
    0x85, 0xf0, 0x00, 0x08, // l.lwz r15, 8(r16)
    0x44, 0x00, 0x78, 0x00, // l.jr r15
    0x15, 0x00, 0x00, 0x00, // l.nop
    0x15, 0x00, 0x00, 0x00  // l.nop
);

DEF_ATOM(
    OR1KPLTPICAtom, PLTAtom,
    0x85, 0x90, 0x00, 0x00, // l.lwz r12, 0(r16) ; index in got
    0xa9, 0x60, 0x00, 0x00, // l.ori r11, r0, 0  ; reloc offset
    0x44, 0x00, 0x60, 0x00, // l.jr r12
    0x15, 0x00, 0x00, 0x00, // l.nop("TODO: support -r mode")
    0x15, 0x00, 0x00, 0x00  // l.nop
);

#undef DEF_ATOM

class ELFPassFile : public lld::SimpleFile {
public:
  ELFPassFile(lld::ELFLinkingContext const &eti) : SimpleFile("ELFPassFile") {
    setOrdinal(eti.getNextOrdinalAndIncrement());
  }

  llvm::BumpPtrAllocator _alloc;
};

template <typename Derived> class RelocationPass : public lld::Pass {
public:
  RelocationPass(const lld::ELFLinkingContext &ctx) : _file(ctx), _ctx(ctx) {}

  virtual void perform(std::unique_ptr<lld::SimpleFile> &mergedFile) override;

protected:
  void handleIFUNC(lld::Reference const &ref);

  lld::elf::PLTAtom *createPLTAtom(GOTAtom *ga) {
    return static_cast<Derived *>(this)->createPLTAtom(ga);
  }

  lld::elf::PLT0Atom *createPLT0Atom() {
    return static_cast<Derived *>(this)->createPLT0Atom();
  }

  lld::elf::PLTAtom const *getIFUNCPLTEntry(lld::DefinedAtom const *da);
  lld::elf::GOTAtom const *getNullGOT();
  lld::elf::GOTAtom const *getGOT(lld::DefinedAtom const *da);
  lld::elf::PLT0Atom const *getPLT0();
  lld::elf::PLTAtom const *getPLTEntry(lld::Atom const *a);
  lld::elf::ObjectAtom const *getObjectEntry(lld::SharedLibraryAtom const *a);
  lld::elf::GOTAtom const *getSharedGOT(lld::Atom const *a);

  ELFPassFile _file;
  const lld::ELFLinkingContext &_ctx;

  llvm::DenseMap<const lld::Atom *, lld::elf::GOTAtom *> _gotMap;
  llvm::DenseMap<const lld::Atom *, lld::elf::PLTAtom *> _pltMap;
  llvm::DenseMap<const lld::Atom *, lld::elf::ObjectAtom *> _objectMap;

  std::vector<lld::elf::GOTAtom *> _gotVector;
  std::vector<lld::elf::PLTAtom *> _pltVector;
  std::vector<lld::elf::ObjectAtom *> _objectVector;

  lld::elf::GOTAtom *_null = nullptr;
  lld::elf::PLT0Atom *_plt0 = nullptr;
  lld::elf::GOTAtom *_got0 = nullptr;
  lld::elf::GOTAtom *_got1 = nullptr;

private:
  void handleReference(lld::DefinedAtom const &atom, lld::Reference const &ref);
};

class DynamicRelocationPass : public RelocationPass<DynamicRelocationPass> {
public:
  DynamicRelocationPass(lld::ELFLinkingContext const &ctx)
      : RelocationPass(ctx) {}

  void handlePlain(lld::Reference const &ref);
  void handleVt(lld::Reference const &ref);
  void handleGOT(lld::Reference const &ref);
  void handlePLT(lld::Reference const &ref);

  lld::elf::PLTAtom *createPLTAtom(GOTAtom *ga);
  lld::elf::PLT0Atom *createPLT0Atom();
};

class StaticRelocationPass : public RelocationPass<StaticRelocationPass> {
public:
  StaticRelocationPass(lld::ELFLinkingContext const &ctx)
      : RelocationPass(ctx) {}

  void handlePlain(lld::Reference const &ref) { handleIFUNC(ref); }

  void handleVt(lld::Reference const &ref);
  void handleGOT(lld::Reference const &ref);
  void handlePLT(lld::Reference const &ref);

  lld::elf::PLTAtom *createPLTAtom(GOTAtom *ga);
  lld::elf::PLT0Atom *createPLT0Atom();
};

} // namespace

template <typename Derived>
void RelocationPass<Derived>::perform(
    std::unique_ptr<lld::SimpleFile> &mergedFile) {
  lld::ScopedTask task(lld::getDefaultDomain(), "OR1K GOT/PLT Pass");

  // 1. Examine all references in defined atoms

  for (lld::DefinedAtom const *atom : mergedFile->defined()) {
    for (lld::Reference const *ref : *atom) {
      handleReference(*atom, *ref);
    }
  }

  // 2. Add all created atoms to the linker file.

  uint64_t ordinal = 0;

  if (_plt0) {
    _plt0->setOrdinal(ordinal++);
    mergedFile->addAtom(*_plt0);
  }
  for (lld::elf::PLTAtom *plt : _pltVector) {
    plt->setOrdinal(ordinal++);
    mergedFile->addAtom(*plt);
  }
  if (_null) {
    _null->setOrdinal(ordinal++);
    mergedFile->addAtom(*_null);
  }
  if (_plt0) {
    _got0->setOrdinal(ordinal++);
    _got1->setOrdinal(ordinal++);
    mergedFile->addAtom(*_got0);
    mergedFile->addAtom(*_got1);
  }
  for (lld::elf::GOTAtom *got : _gotVector) {
    got->setOrdinal(ordinal++);
    mergedFile->addAtom(*got);
  }
  for (lld::elf::ObjectAtom *obj : _objectVector) {
    obj->setOrdinal(ordinal++);
    mergedFile->addAtom(*obj);
  }
}

/**
 * \brief reference handlers dispatcher.
 *
 * Based on the relocation type, it will invoke an appropriate handler.
 */
template <typename Derived>
void RelocationPass<Derived>::handleReference(const lld::DefinedAtom &atom,
                                              const lld::Reference &ref) {
  if (ref.kindNamespace() != lld::Reference::KindNamespace::ELF)
    return;
  assert(ref.kindArch() == lld::Reference::KindArch::OR1K);

  switch (ref.kindValue()) {
  case llvm::ELF::R_OR1K_NONE:
    break;

  case llvm::ELF::R_OR1K_32:
  case llvm::ELF::R_OR1K_16:
  case llvm::ELF::R_OR1K_8:
  case llvm::ELF::R_OR1K_LO_16_IN_INSN:
  case llvm::ELF::R_OR1K_HI_16_IN_INSN:
  case llvm::ELF::R_OR1K_INSN_REL_26:
  case llvm::ELF::R_OR1K_32_PCREL:
  case llvm::ELF::R_OR1K_16_PCREL:
  case llvm::ELF::R_OR1K_8_PCREL:
    static_cast<Derived *>(this)->handlePlain(ref);
    break;

  case llvm::ELF::R_OR1K_GOTPC_HI16:
  case llvm::ELF::R_OR1K_GOTPC_LO16:
  case llvm::ELF::R_OR1K_GOT16:
  case llvm::ELF::R_OR1K_GOTOFF_HI16:
  case llvm::ELF::R_OR1K_GOTOFF_LO16:
    static_cast<Derived *>(this)->handleGOT(ref);
    break;

  case llvm::ELF::R_OR1K_PLT26:
    static_cast<Derived *>(this)->handlePLT(ref);
    break;

  case llvm::ELF::R_OR1K_RELATIVE:
  case llvm::ELF::R_OR1K_COPY:
  case llvm::ELF::R_OR1K_GLOB_DAT:
  case llvm::ELF::R_OR1K_JMP_SLOT:
    // Load time relocations, nothing to do here
    break;

  default:
    llvm_unreachable("Unhandled relocation");
    break;
  }
}

template <typename Derived>
void RelocationPass<Derived>::handleIFUNC(lld::Reference const &ref) {
  auto target = dyn_cast_or_null<const lld::DefinedAtom>(ref.target());

  if (target && target->contentType() == lld::DefinedAtom::typeResolver) {
    const_cast<lld::Reference &>(ref).setTarget(getIFUNCPLTEntry(target));
  }
}

template <typename Derived>
lld::elf::PLTAtom const *RelocationPass<Derived>::getIFUNCPLTEntry(
    lld::DefinedAtom const *da) { // TODO split static/dinamico
  auto plt = _pltMap.find(da);
  if (plt != _pltMap.end())
    return plt->second;

  lld::elf::GOTAtom *ga = new (_file._alloc) OR1KGOTAtom(_file, ".got.plt");
  ga->addReferenceELF_OR1K(llvm::ELF::R_OR1K_RELATIVE, 0, da, 0);

  lld::elf::PLTAtom *pa = createPLTAtom(ga);

  #ifndef NDEBUG
  ga->_name = "__got_ifunc_";
  ga->_name += da->name();

  pa->_name = "__plt_ifunc_";
  pa->_name += da->name();
  #endif

  _gotMap[da] = ga;
  _gotVector.push_back(ga);

  _pltMap[da] = pa;
  _pltVector.push_back(pa);

  return pa;
}

template <typename Derived>
lld::elf::GOTAtom const *RelocationPass<Derived>::getNullGOT() {
  if (!_null) {
    _null = new (_file._alloc) OR1KGOTAtom(_file, ".got.plt");

    #ifndef NDEBUG
    _null->_name = "__got_null";
    #endif
  }

  return _null;
}

template <typename Derived>
lld::elf::GOTAtom const *
RelocationPass<Derived>::getGOT(const lld::DefinedAtom *da) {
  auto got = _gotMap.find(da);
  if (got != _gotMap.end())
    return got->second;

  auto g = new (_file._alloc) OR1KGOTAtom(_file, ".got");
  g->addReferenceELF_OR1K(llvm::ELF::R_OR1K_32, 0, da, 0);

  #ifndef NDEBUG
  g->_name = "__got_";
  g->_name += da->name();
  #endif

  _gotMap[da] = g;
  _gotVector.push_back(g);

  return g;
}

template <typename Derived>
lld::elf::PLT0Atom const *RelocationPass<Derived>::getPLT0() {
  if (_plt0)
    return _plt0;

  getNullGOT(); // Initialize the null entry

  _got0 = new (_file._alloc) OR1KGOTAtom(_file, ".got.plt");
  _got1 = new (_file._alloc) OR1KGOTAtom(_file, ".got.plt");

  _plt0 = createPLT0Atom();

  #ifndef NDEBUG
  _got0->_name = "__got0";
  _got1->_name = "__got1";
  #endif

  return _plt0;
}

template <typename Derived>
lld::elf::PLTAtom const *
RelocationPass<Derived>::getPLTEntry(lld::Atom const *a) {
  auto plt = _pltMap.find(a);
  if (plt != _pltMap.end())
    return plt->second;

  lld::elf::GOTAtom *ga = new (_file._alloc) OR1KGOTAtom(_file, ".got.plt");
  ga->addReferenceELF_OR1K(llvm::ELF::R_OR1K_32, 0, getPLT0(), 0);

  lld::elf::PLTAtom *pa = createPLTAtom(ga);

  #ifndef NDEBUG
  ga->_name = "__got_";
  ga->_name += a->name();
  pa->_name = "__plt_";
  pa->_name += a->name();
  #endif

  _gotMap[a] = ga;
  _gotVector.push_back(ga);

  _pltMap[a] = pa;
  _pltVector.push_back(pa);

  return pa;
}

template <typename Derived>
lld::elf::ObjectAtom const *
RelocationPass<Derived>::getObjectEntry(lld::SharedLibraryAtom const *a) {
  auto obj = _objectMap.find(a);
  if (obj != _objectMap.end())
    return obj->second;

  lld::elf::ObjectAtom *oa = new (_file._alloc) lld::elf::ObjectAtom(_file);

  // This needs to point to the atom that we just created.
  oa->addReferenceELF_OR1K(llvm::ELF::R_OR1K_COPY, 0, oa, 0);
  oa->_name = a->name();
  oa->_size = a->size();

  _objectMap[a] = oa;
  _objectVector.push_back(oa);

  return oa;
}

template <typename Derived>
lld::elf::GOTAtom const *
RelocationPass<Derived>::getSharedGOT(lld::Atom const *a) {
  auto got = _gotMap.find(a);
  if (got != _gotMap.end())
    return got->second;

  lld::elf::GOTAtom *g = new (_file._alloc) OR1KGOTAtom(_file, ".got");
  g->addReferenceELF_OR1K(llvm::ELF::R_OR1K_GLOB_DAT, 0, a, 0);

  #ifndef NDEBUG
  g->_name = "__got_";
  g->_name += a->name();
  #endif

  _gotMap[a] = g;
  _gotVector.push_back(g);

  return g;
}

std::unique_ptr<lld::Pass>
lld::elf::createOR1KRelocationPass(lld::elf::OR1KLinkingContext &ctx) {
  switch (ctx.getOutputELFType()) {
  case llvm::ELF::ET_EXEC:
    if (ctx.isDynamic()) {
      return llvm::make_unique<DynamicRelocationPass>(ctx);
    }
    return llvm::make_unique<StaticRelocationPass>(ctx);

  case llvm::ELF::ET_DYN:
    return llvm::make_unique<DynamicRelocationPass>(ctx);

  case llvm::ELF::ET_REL:
    return nullptr;

  default:
    llvm_unreachable("Unhandled output file type");
  }
}

static inline bool isSharedOrUndefined(lld::Atom const *atom) {
  return isa<lld::SharedLibraryAtom>(atom) || isa<lld::UndefinedAtom>(atom);
}

void StaticRelocationPass::handleGOT(lld::Reference const &ref) {
  if (llvm::isa<lld::UndefinedAtom>(ref.target())) {
    const_cast<lld::Reference &>(ref).setTarget(getNullGOT());
  }

  lld::DefinedAtom const *atom =
      llvm::dyn_cast_or_null<const lld::DefinedAtom>(ref.target());

  if (atom) {
    const_cast<lld::Reference &>(ref).setTarget(getGOT(atom));
  }
}

void StaticRelocationPass::handlePLT(lld::Reference const &ref) {
  // Static code doesn't need PLTs.
  const_cast<lld::Reference &>(ref)
      .setKindValue(llvm::ELF::R_OR1K_32_PCREL); // FIXME

  lld::DefinedAtom const *atom =
      llvm::dyn_cast_or_null<const lld::DefinedAtom>(ref.target());

  if (atom) {
    if (atom->contentType() == lld::DefinedAtom::typeResolver) {
      return handleIFUNC(ref);
    }
  }
}

lld::elf::PLTAtom *StaticRelocationPass::createPLTAtom(lld::elf::GOTAtom *ga) {
  lld::elf::PLTAtom *pa = new (_file._alloc) OR1KPLTAtom(_file, ".plt");
  pa->addReferenceELF_OR1K(llvm::ELF::R_OR1K_HI_16_IN_INSN, 0, ga, 0);
  pa->addReferenceELF_OR1K(llvm::ELF::R_OR1K_LO_16_IN_INSN, 4, ga, 0);
  pa->addReferenceELF_OR1K(llvm::ELF::R_OR1K_GOT16, 16, ga, 0);
  return pa;
}

lld::elf::PLT0Atom *StaticRelocationPass::createPLT0Atom() {
  lld::elf::PLT0Atom *pa0 = new (_file._alloc) OR1KPLT0Atom(_file);
  pa0->addReferenceELF_OR1K(llvm::ELF::R_OR1K_HI_16_IN_INSN, 0, _got0, 0);
  pa0->addReferenceELF_OR1K(llvm::ELF::R_OR1K_LO_16_IN_INSN, 4, _got0, 0);
  return pa0;
}

void DynamicRelocationPass::handlePlain(lld::Reference const &ref) {
  if (!ref.target())
    return;

  lld::SharedLibraryAtom const *atom =
      llvm::dyn_cast_or_null<const lld::SharedLibraryAtom>(ref.target());

  if (atom) {
    if (atom->type() == lld::SharedLibraryAtom::Type::Data) {
      const_cast<lld::Reference &>(ref).setTarget(getObjectEntry(atom));
    } else if (atom->type() == lld::SharedLibraryAtom::Type::Code) {
      const_cast<lld::Reference &>(ref).setTarget(getPLTEntry(atom));
    } else {
      llvm_unreachable("Invalid SharedLibraryAtom type");
    }
  } else {
    return handleIFUNC(ref);
  }
}

void DynamicRelocationPass::handleGOT(lld::Reference const &ref) {
  lld::DefinedAtom const *atom =
      dyn_cast_or_null<const lld::DefinedAtom>(ref.target());

  if (atom) {
    const_cast<lld::Reference &>(ref).setTarget(getGOT(atom));
  } else if (isSharedOrUndefined(ref.target())) {
    const_cast<lld::Reference &>(ref).setTarget(getSharedGOT(ref.target()));
  }
}

void DynamicRelocationPass::handlePLT(lld::Reference const &ref) {
  lld::DefinedAtom const *atom =
      dyn_cast_or_null<const lld::DefinedAtom>(ref.target());

  if (atom) {
    if (atom->contentType() == lld::DefinedAtom::typeResolver) {
      return handleIFUNC(ref);
    }
  }

  if (isSharedOrUndefined(ref.target())) {
    const_cast<lld::Reference &>(ref).setTarget(getPLTEntry(ref.target()));
  }
}

lld::elf::PLTAtom *DynamicRelocationPass::createPLTAtom(lld::elf::GOTAtom *ga) {
  lld::elf::PLTAtom *pa = new (_file._alloc) OR1KPLTPICAtom(_file, ".plt");
  pa->addReferenceELF_OR1K(llvm::ELF::R_OR1K_HI_16_IN_INSN, 0, ga, 0);
  pa->addReferenceELF_OR1K(llvm::ELF::R_OR1K_LO_16_IN_INSN, 4, ga, 0);
  return pa;
}

lld::elf::PLT0Atom *DynamicRelocationPass::createPLT0Atom() {
  lld::elf::PLT0Atom *pa0 = new (_file._alloc) OR1KPLT0PICAtom(_file);
  pa0->addReferenceELF_OR1K(llvm::ELF::R_OR1K_HI_16_IN_INSN, 0, _got0, 0);
  pa0->addReferenceELF_OR1K(llvm::ELF::R_OR1K_LO_16_IN_INSN, 4, _got0, 0);
  return pa0;
}
