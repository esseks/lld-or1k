//===- lib/ReaderWriter/ELF/OR1K/OR1KTargetHandler.h ----------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_TARGET_HANDLER_H

#include "OR1KLinkingContext.h"
#include "OR1KRelocationHandler.h"
#include "OR1KVectorsSection.h"

#include "ELFReader.h"
#include "TargetLayout.h"
#include "lld/Core/Simple.h"

namespace lld {
namespace elf {

class OR1KLinkingContext;

// TODO check max align
typedef llvm::object::ELFType<llvm::support::big, 2, false> OR1KBEType;
typedef llvm::object::ELFType<llvm::support::little, 2, false> OR1KLEType;

template <typename ELFTy> class OR1KTargetLayout : public TargetLayout<ELFTy> {
public:
  enum OR1KSectionOrder { ORDER_VECTORS = 1 };

  OR1KTargetLayout(OR1KLinkingContext &ctx) : TargetLayout<ELFTy>(ctx) {}

  AtomSection<ELFTy> *createSection(
      StringRef name, int32_t contentType,
      DefinedAtom::ContentPermissions contentPermissions,
      typename TargetLayout<ELFTy>::SectionOrder sectionOrder) override {
    if (name == ".vectors") {
      return new (_alloc) OR1KVectorsSection<ELFTy>(this->_ctx);
    }
    return TargetLayout<ELFTy>::createSection(name, contentType,
                                              contentPermissions, sectionOrder);
  }

  typename TargetLayout<ELFTy>::SectionOrder
  getSectionOrder(StringRef name, int32_t contentType,
                  int32_t contentPermissions) override {
    if (name == ".vectors")
      return ORDER_VECTORS;
    return TargetLayout<ELFTy>::getSectionOrder(name, contentType,
                                                contentPermissions);
  }

  typename TargetLayout<ELFTy>::SegmentType
  getSegmentType(Section<ELFTy> *section) const override {
    if (section->order() == ORDER_VECTORS)
      return llvm::ELF::PT_LOAD;
    return TargetLayout<ELFTy>::getSegmentType(section);
  }

  uint64_t getGOTSymAddr() {
    if (!_gotSymAtom.hasValue()) {
      _gotSymAtom = this->findAbsoluteAtom("_GLOBAL_OFFSET_TABLE_");
    }

    if (*_gotSymAtom)
      return (*_gotSymAtom)->_virtualAddr;
    return 0;
  }

private:
  llvm::BumpPtrAllocator _alloc;
  llvm::Optional<AtomLayout *> _gotSymAtom;
};

template <typename ELFTy> class OR1KTargetHandler final : public TargetHandler {
private:
  typedef ELFReader<ELFTy, OR1KLinkingContext, ELFFile> ObjReader;
  typedef ELFReader<ELFTy, OR1KLinkingContext, DynamicFile> DSOReader;

public:
  OR1KTargetHandler(OR1KLinkingContext &ctx)
      : _ctx(ctx), _targetLayout(new OR1KTargetLayout<ELFTy>(ctx)),
        _relocationHandler(
            createOR1KRelocationHandler<ELFTy>(_ctx, *_targetLayout)) {}

  TargetRelocationHandler const &getRelocationHandler() const override {
    return *_relocationHandler;
  }

  std::unique_ptr<Reader> getObjReader() override {
    return llvm::make_unique<ObjReader>(_ctx);
  }

  std::unique_ptr<Reader> getDSOReader() override {
    return llvm::make_unique<DSOReader>(_ctx);
  }

  std::unique_ptr<Writer> getWriter() override;

protected:
  OR1KLinkingContext &_ctx;
  std::unique_ptr<OR1KTargetLayout<ELFTy>> _targetLayout;
  std::unique_ptr<lld::elf::TargetRelocationHandler> _relocationHandler;
};

std::unique_ptr<TargetHandler>
createOR1KTargetHandler(llvm::Triple const &triple, OR1KLinkingContext &ctx);

} // end namespace elf
} // end namespace lld

#endif
