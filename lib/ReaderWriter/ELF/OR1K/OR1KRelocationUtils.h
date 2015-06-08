//===- lib/ReaderWriter/ELF/OR1K/OR1KRelocationUtils.h --------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_OR1K_OR1K_RELOCATION_UTILS_H
#define LLD_READER_WRITER_ELF_OR1K_OR1K_RELOCATION_UTILS_H

#include "llvm/Support/Endian.h"


namespace lld {
namespace elf {

template <typename T, llvm::support::endianness E> static inline
void write(uint8_t *p, T v);

template <> inline
void write<uint32_t, llvm::support::endianness::big>(uint8_t *p, uint32_t v) {
    llvm::support::endian::write32be(p, v);
}

template <> inline
void write<uint32_t, llvm::support::endianness::little>(uint8_t *p, uint32_t v) {
    llvm::support::endian::write32le(p, v);
}

template <> inline
void write<uint16_t, llvm::support::endianness::big>(uint8_t *p, uint16_t v) {
    llvm::support::endian::write16be(p, v);
}

template <> inline
void write<uint16_t, llvm::support::endianness::little>(uint8_t *p, uint16_t v) {
    llvm::support::endian::write32le(p, v);
}

template <> inline
void write<uint8_t, llvm::support::endianness::big>(uint8_t *p, uint8_t v) {
    *p = v;
}

template <> inline
void write<uint8_t, llvm::support::endianness::little>(uint8_t *p, uint8_t v) {
    *p = v;
}

template <typename T, llvm::support::endianness E> static inline
T read(uint8_t const* p);

template <> inline
uint32_t read<uint32_t, llvm::support::endianness::big>(uint8_t const* p) {
    return llvm::support::endian::read32be(p);
}

template <> inline
uint32_t read<uint32_t, llvm::support::endianness::little>(uint8_t const* p) {
    return llvm::support::endian::read32le(p);
}

template <
    llvm::support::endianness Endianness, typename OutType,
    unsigned rightShift, bool relative, uint32_t srcMask, uint32_t destMask
>
static inline
void reloc(uint8_t *location, uint64_t S, uint64_t A, uint64_t P=0) {
    assert((relative || (P == 0)) && "Absolute relocation but P is not 0");

    uint32_t low = ((S + A - (relative? P: 0)) >> rightShift) & destMask;
    if (srcMask != 0) {
        uint32_t high = read<uint32_t, Endianness>(location) & srcMask;
        write<OutType, Endianness>(location, high | low);
    } else {
        write<OutType, Endianness>(location, low);
    }
}

} // namespace elf
} // namespace lld

#endif
