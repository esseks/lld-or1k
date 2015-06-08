//===- lib/ReaderWriter/ELF/OR1K/OR1KRelocationHandler.cpp ----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


#include "OR1KRelocationHandler.h"
#include "OR1KLinkingContext.h"
#include "OR1KTargetHandler.h"
#include "OR1KRelocationUtils.h"

#include "ELFReader.h"
#include "TargetLayout.h"
#include "llvm/Support/Endian.h"

typedef lld::Reference::Addend Addend;
using namespace lld::elf;


namespace {

template <typename ELFTy>
class OR1KRelocationHandler: public TargetRelocationHandler {
public:
    OR1KRelocationHandler(OR1KLinkingContext &ctx, OR1KTargetLayout<ELFTy> &layout):
        _ctx(ctx), _layout(layout) {}

    std::error_code applyRelocation(
        lld::elf::ELFWriter &writer, llvm::FileOutputBuffer &buf,
        lld::AtomLayout const& atom, lld::Reference const& ref
    ) const override;

private:
    OR1KLinkingContext &_ctx;
    OR1KTargetLayout<ELFTy> &_layout;
};

} // namespace


template <typename ELFTy>
std::error_code OR1KRelocationHandler<ELFTy>::applyRelocation(
    lld::elf::ELFWriter &writer, llvm::FileOutputBuffer &buf,
    lld::AtomLayout const& atom, lld::Reference const& ref) const
{
    if (ref.kindNamespace() != lld::Reference::KindNamespace::ELF) {
        return std::error_code();
    }

    assert(ref.kindArch() == lld::Reference::KindArch::OR1K);

    if (ref.kindValue() == R_OR1K_GOTPC_HI16 || ref.kindValue() == R_OR1K_GOTPC_HI16) {
        assert(ref.target()->name() == "_GLOBAL_OFFSET_TABLE_");
    }

    uint8_t *atomContent = buf.getBufferStart() + atom._fileOffset;
    uint8_t *location = atomContent + ref.offsetInAtom();
    const uint64_t relocation = atom._virtualAddr + ref.offsetInAtom(); // P
    const uint64_t target = writer.addressOfAtom(ref.target()); // S
    const Addend addend = ref.addend(); // A
    const uint64_t offsetInSection =
        static_cast<lld::elf::ELFReference<ELFTy> const&>(ref).offsetInSection();

    switch (ref.kindValue()) {
    case R_OR1K_NONE:
        break;

    case R_OR1K_RELATIVE:
    case R_OR1K_COPY:
    case R_OR1K_GLOB_DAT:
    case R_OR1K_JMP_SLOT:
        // Load time relocations, nothing to do here
        break;

    // endianness, type written, src shiftr, is relative, src mask, dest mask
    case R_OR1K_32:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 0, false, 0, 0xffffffff>
          (location, target, addend)
        ;
        break;
    case R_OR1K_16:
        reloc
          <ELFTy::TargetEndianness, uint16_t, 0, false, 0, 0xffff>
          (location, target, addend)
        ;
        break;
    case R_OR1K_8:
        reloc
          <ELFTy::TargetEndianness, uint8_t, 0, false, 0, 0xff>
          (location, target, addend)
        ;
        break;
    case R_OR1K_LO_16_IN_INSN:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 0, false, 0xffff0000, 0xffff>
          (location, target, addend)
        ;
        break;
    case R_OR1K_HI_16_IN_INSN:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 16, false, 0xffff0000, 0xffff>
          (location, target, addend)
        ;
        break;
    case R_OR1K_INSN_REL_26:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 2, true, 0xfc000000, 0x3ffffff>
          (location, target, addend, relocation)
        ;
        break;
    case R_OR1K_32_PCREL:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 0, true, 0, 0xffffffff>
          (location, target, addend, relocation - offsetInSection)
        ;
        break;
    case R_OR1K_16_PCREL:
        reloc
          <ELFTy::TargetEndianness, uint16_t, 0, true, 0, 0xffff>
          (location, target, addend, relocation - offsetInSection)
        ;
        break;
    case R_OR1K_8_PCREL:
        reloc
          <ELFTy::TargetEndianness, uint8_t, 0, true, 0, 0xff>
          (location, target, addend, relocation - offsetInSection)
        ;
        break;
    case R_OR1K_GOTPC_HI16:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 16, true, 0xffff0000, 0xffff>
          (location, target, addend, relocation)
        ;
        break;
    case R_OR1K_GOTPC_LO16:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 0, true, 0xffff0000, 0xffff>
          (location, target, addend, relocation)
        ;
        break;
    case R_OR1K_GOTOFF_HI16:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 16, true, 0xffff0000, 0xffff>
          (location, target, addend, _layout.getGOTSymAddr())
        ;
        break;
    case R_OR1K_GOTOFF_LO16:
        reloc
          <ELFTy::TargetEndianness, uint32_t, 16, true, 0xffff0000, 0xffff>
          (location, target, addend, _layout.getGOTSymAddr())
        ;
        break;
    case R_OR1K_GOT16: // TODO
    case R_OR1K_PLT26:
    default:
        return make_unhandled_reloc_error();
    }

    return std::error_code();
}


namespace lld {
namespace elf {

template <>
std::unique_ptr<TargetRelocationHandler>
createOR1KRelocationHandler(OR1KLinkingContext &ctx, OR1KTargetLayout<OR1KBEType> &layout) {
    return llvm::make_unique<OR1KRelocationHandler<OR1KBEType>>(ctx, layout);
}

template <>
std::unique_ptr<TargetRelocationHandler>
createOR1KRelocationHandler(OR1KLinkingContext &ctx, OR1KTargetLayout<OR1KLEType> &layout) {
    return llvm::make_unique<OR1KRelocationHandler<OR1KLEType>>(ctx, layout);
}

} // namespace elf
} // namespace lld

