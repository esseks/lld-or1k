//===- lib/ReaderWriter/ELF/ELFFile.h -------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_FILE_H
#define LLD_READER_WRITER_ELF_FILE_H

#include "Atoms.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include <map>
#include <unordered_map>

namespace lld {

namespace elf {
/// \brief Read a binary, find out based on the symbol table contents what kind
/// of symbol it is and create corresponding atoms for it
template <class ELFT> class ELFFile : public SimpleFile {
  typedef llvm::object::Elf_Sym_Impl<ELFT> Elf_Sym;
  typedef llvm::object::Elf_Shdr_Impl<ELFT> Elf_Shdr;
  typedef llvm::object::Elf_Rel_Impl<ELFT, false> Elf_Rel;
  typedef llvm::object::Elf_Rel_Impl<ELFT, true> Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym_Iter Elf_Sym_Iter;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela_Iter Elf_Rela_Iter;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel_Iter Elf_Rel_Iter;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Word Elf_Word;

  // A Map is used to hold the atoms that have been divided up
  // after reading the section that contains Merge String attributes
  struct MergeSectionKey {
    const Elf_Shdr *_shdr;
    int64_t _offset;
  };

  struct MergeSectionEq {
    int64_t operator()(const MergeSectionKey &k) const {
      return llvm::hash_combine((int64_t)(k._shdr->sh_name),
                                (int64_t)k._offset);
    }
    bool operator()(const MergeSectionKey &lhs,
                    const MergeSectionKey &rhs) const {
      return ((lhs._shdr->sh_name == rhs._shdr->sh_name) &&
              (lhs._offset == rhs._offset));
    }
  };

  struct MergeString {
    MergeString(int64_t offset, StringRef str, const Elf_Shdr *shdr,
                StringRef sectionName)
        : _offset(offset), _string(str), _shdr(shdr),
          _sectionName(sectionName) {}
    // the offset of this atom
    int64_t _offset;
    // The content
    StringRef _string;
    // Section header
    const Elf_Shdr *_shdr;
    // Section name
    StringRef _sectionName;
  };

  // This is used to find the MergeAtom given a relocation
  // offset
  typedef std::vector<ELFMergeAtom<ELFT> *> MergeAtomsT;

  /// \brief find a merge atom given a offset
  ELFMergeAtom<ELFT> *findMergeAtom(const Elf_Shdr *shdr, int64_t offset) {
    auto it = std::find_if(_mergeAtoms.begin(), _mergeAtoms.end(),
                           [=](const ELFMergeAtom<ELFT> *a) {
                             int64_t off = a->offset();
                             return shdr->sh_name == a->section() &&
                                    offset >= off &&
                                    offset <= off + (int64_t)a->size();
                           });
    assert(it != _mergeAtoms.end());
    return *it;
  }

  typedef std::unordered_map<MergeSectionKey, DefinedAtom *, MergeSectionEq,
                             MergeSectionEq> MergedSectionMapT;
  typedef typename MergedSectionMapT::iterator MergedSectionMapIterT;

public:
  ELFFile(StringRef name, ELFLinkingContext &ctx)
      : SimpleFile(name), _ordinal(0),
        _doStringsMerge(ctx.mergeCommonStrings()), _useWrap(false), _ctx(ctx) {
    setLastError(std::error_code());
  }

  ELFFile(std::unique_ptr<MemoryBuffer> mb, ELFLinkingContext &ctx)
      : SimpleFile(mb->getBufferIdentifier()), _mb(std::move(mb)), _ordinal(0),
        _doStringsMerge(ctx.mergeCommonStrings()),
        _useWrap(ctx.wrapCalls().size()), _ctx(ctx) {}

  static bool canParse(file_magic magic) {
    return magic == file_magic::elf_relocatable;
  }

  virtual Reference::KindArch kindArch();

  /// \brief Create symbols from LinkingContext.
  std::error_code createAtomsFromContext();

  /// \brief Read input sections and populate necessary data structures
  /// to read them later and create atoms
  std::error_code createAtomizableSections();

  /// \brief Create mergeable atoms from sections that have the merge attribute
  /// set
  std::error_code createMergeableAtoms();

  /// \brief Add the symbols that the sections contain. The symbols will be
  /// converted to atoms for
  /// Undefined symbols, absolute symbols
  std::error_code createSymbolsFromAtomizableSections();

  /// \brief Create individual atoms
  std::error_code createAtoms();

  // Assuming sourceSymbol has a reference to targetSym, find an atom
  // for targetSym. Usually it's just the atom for targetSym.
  // However, if an atom is in a section group, we may want to return an
  // undefined atom for targetSym to let the resolver to resolve the
  // symbol. (It's because if targetSym is in a section group A, and the
  // group A is not linked in because other file already provides a
  // section group B, we want to resolve references to B, not to A.)
  Atom *findAtom(const Elf_Sym *sourceSym, const Elf_Sym *targetSym) {
    // Return the atom for targetSym if we can do so.
    Atom *target = _symbolToAtomMapping.lookup(targetSym);
    if (target->definition() != Atom::definitionRegular)
      return target;
    Atom::Scope scope = llvm::cast<DefinedAtom>(target)->scope();
    if (scope == DefinedAtom::scopeTranslationUnit)
      return target;
    if (!redirectReferenceUsingUndefAtom(sourceSym, targetSym))
      return target;

    // Otherwise, create a new undefined symbol and returns it.
    StringRef targetName = target->name();
    auto it = _undefAtomsForGroupChild.find(targetName);
    if (it != _undefAtomsForGroupChild.end())
      return it->getValue();
    auto atom = new (_readerStorage) SimpleUndefinedAtom(*this, targetName);
    _undefAtomsForGroupChild[targetName] = atom;
    addAtom(*atom);
    return atom;
  }

protected:
  ELFDefinedAtom<ELFT> *createDefinedAtomAndAssignRelocations(
      StringRef symbolName, StringRef sectionName, const Elf_Sym *symbol,
      const Elf_Shdr *section, ArrayRef<uint8_t> symContent,
      ArrayRef<uint8_t> secContent);

  std::error_code doParse() override;

  /// \brief Iterate over Elf_Rela relocations list and create references.
  virtual void createRelocationReferences(const Elf_Sym *symbol,
                                          ArrayRef<uint8_t> content,
                                          range<Elf_Rela_Iter> rels);

  /// \brief Iterate over Elf_Rel relocations list and create references.
  virtual void createRelocationReferences(const Elf_Sym *symbol,
                                          ArrayRef<uint8_t> symContent,
                                          ArrayRef<uint8_t> secContent,
                                          range<Elf_Rel_Iter> rels);

  /// \brief After all the Atoms and References are created, update each
  /// Reference's target with the Atom pointer it refers to.
  void updateReferences();

  /// \brief Update the reference if the access corresponds to a merge string
  /// section.
  void updateReferenceForMergeStringAccess(ELFReference<ELFT> *ref,
                                           const Elf_Sym *symbol,
                                           const Elf_Shdr *shdr);

  /// \brief Do we want to ignore the section. Ignored sections are
  /// not processed to create atoms
  bool isIgnoredSection(const Elf_Shdr *section);

  /// \brief Is the current section be treated as a mergeable string section.
  /// The contents of a mergeable string section are null-terminated strings.
  /// If the section have mergeable strings, the linker would need to split
  /// the section into multiple atoms and mark them mergeByContent.
  bool isMergeableStringSection(const Elf_Shdr *section);

  /// \brief Returns a new anonymous atom whose size is equal to the
  /// section size. That atom will be used to represent the entire
  /// section that have no symbols.
  ELFDefinedAtom<ELFT> *createSectionAtom(const Elf_Shdr *section,
                                          StringRef sectionName,
                                          ArrayRef<uint8_t> contents);

  /// Returns the symbol's content size. The nextSymbol should be null if the
  /// symbol is the last one in the section.
  uint64_t symbolContentSize(const Elf_Shdr *section,
                             const Elf_Sym *symbol,
                             const Elf_Sym *nextSymbol);

  void createEdge(ELFDefinedAtom<ELFT> *from, ELFDefinedAtom<ELFT> *to,
                  uint32_t edgeKind);

  /// Get the section name for a section.
  ErrorOr<StringRef> getSectionName(const Elf_Shdr *shdr) const {
    if (!shdr)
      return StringRef();
    return _objFile->getSectionName(shdr);
  }

  /// Determines if the section occupy memory space.
  bool sectionOccupiesMemorySpace(const Elf_Shdr *shdr) const {
    return (shdr->sh_type != llvm::ELF::SHT_NOBITS);
  }

  /// Return the section contents.
  ErrorOr<ArrayRef<uint8_t>> getSectionContents(const Elf_Shdr *shdr) const {
    if (!shdr || !sectionOccupiesMemorySpace(shdr))
      return ArrayRef<uint8_t>();
    return _objFile->getSectionContents(shdr);
  }

  /// Returns true if the symbol is a undefined symbol.
  bool isUndefinedSymbol(const Elf_Sym *sym) const {
    return (sym->st_shndx == llvm::ELF::SHN_UNDEF);
  }

  /// Determines if the target wants to create an atom for a section that has no
  /// symbol references.
  bool handleSectionWithNoSymbols(const Elf_Shdr *shdr,
                                  std::vector<Elf_Sym_Iter> &syms) const {
    return shdr && (shdr->sh_type == llvm::ELF::SHT_PROGBITS) && syms.empty();
  }

  /// Handle creation of atoms for .gnu.linkonce sections.
  std::error_code handleGnuLinkOnceSection(
      const Elf_Shdr *section,
      llvm::StringMap<std::vector<ELFDefinedAtom<ELFT> *>> &atomsForSection);

  // Handle COMDAT scetions.
  std::error_code handleSectionGroup(
      const Elf_Shdr *section,
      llvm::StringMap<std::vector<ELFDefinedAtom<ELFT> *>> &atomsForSection);

  /// Process the Undefined symbol and create an atom for it.
  ELFUndefinedAtom<ELFT> *createUndefinedAtom(StringRef symName,
                                              const Elf_Sym *sym) {
    return new (_readerStorage) ELFUndefinedAtom<ELFT>(*this, symName, sym);
  }

  /// Returns true if the symbol is a absolute symbol.
  bool isAbsoluteSymbol(const Elf_Sym *sym) const {
    return (sym->st_shndx == llvm::ELF::SHN_ABS);
  }

  /// Process the Absolute symbol and create an atom for it.
  ELFAbsoluteAtom<ELFT> *createAbsoluteAtom(StringRef symName,
                                            const Elf_Sym *sym, int64_t value) {
    return new (_readerStorage)
        ELFAbsoluteAtom<ELFT>(*this, symName, sym, value);
  }

  /// Returns true if the symbol is common symbol. A common symbol represents a
  /// tentive definition in C. It has name, size and alignment constraint, but
  /// actual storage has not yet been allocated. (The linker will allocate
  /// storage for them in the later pass after coalescing tentative symbols by
  /// name.)
  virtual bool isCommonSymbol(const Elf_Sym *symbol) const {
    return symbol->getType() == llvm::ELF::STT_COMMON ||
           symbol->st_shndx == llvm::ELF::SHN_COMMON;
  }

  /// Returns true if the section is a gnulinkonce section.
  bool isGnuLinkOnceSection(StringRef sectionName) const {
    return sectionName.startswith(".gnu.linkonce.");
  }

  /// Returns true if the section is a COMDAT group section.
  bool isGroupSection(const Elf_Shdr *shdr) const {
    return (shdr->sh_type == llvm::ELF::SHT_GROUP);
  }

  /// Returns true if the section is a member of some group.
  bool isSectionMemberOfGroup(const Elf_Shdr *shdr) const {
    return (shdr->sh_flags & llvm::ELF::SHF_GROUP);
  }

  /// Returns correct st_value for the symbol depending on the architecture.
  /// For most architectures it's just a regular st_value with no changes.
  virtual uint64_t getSymbolValue(const Elf_Sym *symbol) const {
    return symbol->st_value;
  }

  /// Returns initial addend
  virtual Reference::Addend getInitialAddend(ArrayRef<uint8_t> symContent,
                                  uint64_t symbolValue,
                                  const Elf_Rel& reference) const {
    return *(symContent.data() + reference.r_offset - symbolValue);
  }

  /// Process the common symbol and create an atom for it.
  virtual ELFCommonAtom<ELFT> *createCommonAtom(StringRef symName,
                                                const Elf_Sym *sym) {
    return new (_readerStorage) ELFCommonAtom<ELFT>(*this, symName, sym);
  }

  /// Returns true if the symbol is a defined symbol.
  virtual bool isDefinedSymbol(const Elf_Sym *sym) const {
    return (sym->getType() == llvm::ELF::STT_NOTYPE ||
            sym->getType() == llvm::ELF::STT_OBJECT ||
            sym->getType() == llvm::ELF::STT_FUNC ||
            sym->getType() == llvm::ELF::STT_GNU_IFUNC ||
            sym->getType() == llvm::ELF::STT_SECTION ||
            sym->getType() == llvm::ELF::STT_FILE ||
            sym->getType() == llvm::ELF::STT_TLS);
  }

  /// Creates an atom for a given defined symbol.
  virtual ELFDefinedAtom<ELFT> *
  createDefinedAtom(StringRef symName, StringRef sectionName,
                    const Elf_Sym *sym, const Elf_Shdr *sectionHdr,
                    ArrayRef<uint8_t> contentData, unsigned int referenceStart,
                    unsigned int referenceEnd,
                    std::vector<ELFReference<ELFT> *> &referenceList) {
    return new (_readerStorage) ELFDefinedAtom<ELFT>(
        *this, symName, sectionName, sym, sectionHdr, contentData,
        referenceStart, referenceEnd, referenceList);
  }

  /// Process the Merge string and create an atom for it.
  ELFMergeAtom<ELFT> *createMergedString(StringRef sectionName,
                                         const Elf_Shdr *sectionHdr,
                                         ArrayRef<uint8_t> contentData,
                                         unsigned int offset) {
    ELFMergeAtom<ELFT> *mergeAtom = new (_readerStorage)
        ELFMergeAtom<ELFT>(*this, sectionName, sectionHdr, contentData, offset);
    const MergeSectionKey mergedSectionKey = {sectionHdr, offset};
    if (_mergedSectionMap.find(mergedSectionKey) == _mergedSectionMap.end())
      _mergedSectionMap.insert(std::make_pair(mergedSectionKey, mergeAtom));
    return mergeAtom;
  }

  /// References to the sections comprising a group, from sections
  /// outside the group, must be made via global UNDEF symbols,
  /// referencing global symbols defined as addresses in the group
  /// sections. They may not reference local symbols for addresses in
  /// the group's sections, including section symbols.
  /// ABI Doc : https://mentorembedded.github.io/cxx-abi/abi/prop-72-comdat.html
  /// Does the atom need to be redirected using a separate undefined atom?
  bool redirectReferenceUsingUndefAtom(const Elf_Sym *sourceSymbol,
                                       const Elf_Sym *targetSymbol) const;

  void addReferenceToSymbol(const ELFReference<ELFT> *r, const Elf_Sym *sym) {
    _referenceToSymbol[r] = sym;
  }

  const Elf_Sym *findSymbolForReference(const ELFReference<ELFT> *r) const {
    auto elfReferenceToSymbol = _referenceToSymbol.find(r);
    if (elfReferenceToSymbol != _referenceToSymbol.end())
      return elfReferenceToSymbol->second;
    return nullptr;
  }

  llvm::BumpPtrAllocator _readerStorage;
  std::unique_ptr<llvm::object::ELFFile<ELFT> > _objFile;

  /// \brief _relocationAddendReferences and _relocationReferences contain the
  /// list of relocations references.  In ELF, if a section named, ".text" has
  /// relocations will also have a section named ".rel.text" or ".rela.text"
  /// which will hold the entries.
  std::unordered_map<StringRef, range<Elf_Rela_Iter>>
  _relocationAddendReferences;
  MergedSectionMapT _mergedSectionMap;
  std::unordered_map<StringRef, range<Elf_Rel_Iter>> _relocationReferences;
  std::vector<ELFReference<ELFT> *> _references;
  llvm::DenseMap<const Elf_Sym *, Atom *> _symbolToAtomMapping;
  llvm::DenseMap<const ELFReference<ELFT> *, const Elf_Sym *>
  _referenceToSymbol;
  // Group child atoms have a pair corresponding to the signature and the
  // section header of the section that was used for generating the signature.
  llvm::DenseMap<const Elf_Sym *, std::pair<StringRef, const Elf_Shdr *>>
      _groupChild;
  llvm::StringMap<Atom *> _undefAtomsForGroupChild;

  /// \brief Atoms that are created for a section that has the merge property
  /// set
  MergeAtomsT _mergeAtoms;

  /// \brief the section and the symbols that are contained within it to create
  /// used to create atoms
  llvm::MapVector<const Elf_Shdr *, std::vector<Elf_Sym_Iter>> _sectionSymbols;

  /// \brief Sections that have merge string property
  std::vector<const Elf_Shdr *> _mergeStringSections;

  std::unique_ptr<MemoryBuffer> _mb;
  int64_t _ordinal;

  /// \brief the cached options relevant while reading the ELF File
  bool _doStringsMerge;

  /// \brief Is --wrap on?
  bool _useWrap;

  /// \brief The LinkingContext.
  ELFLinkingContext &_ctx;

  // Wrap map
  llvm::StringMap<UndefinedAtom *> _wrapSymbolMap;
};

/// \brief All atoms are owned by a File. To add linker specific atoms
/// the atoms need to be inserted to a file called (RuntimeFile) which
/// are basically additional symbols required by libc and other runtime
/// libraries part of executing a program. This class provides support
/// for adding absolute symbols and undefined symbols
template <class ELFT> class RuntimeFile : public ELFFile<ELFT> {
public:
  typedef llvm::object::Elf_Sym_Impl<ELFT> Elf_Sym;
  RuntimeFile(ELFLinkingContext &ctx, StringRef name)
      : ELFFile<ELFT>(name, ctx) {}

  /// \brief add a global absolute atom
  virtual void addAbsoluteAtom(StringRef symbolName) {
    assert(!symbolName.empty() && "AbsoluteAtoms must have a name");
    Elf_Sym *sym = new (this->_readerStorage) Elf_Sym;
    sym->st_name = 0;
    sym->st_value = 0;
    sym->st_shndx = llvm::ELF::SHN_ABS;
    sym->setBindingAndType(llvm::ELF::STB_GLOBAL, llvm::ELF::STT_OBJECT);
    sym->setVisibility(llvm::ELF::STV_DEFAULT);
    sym->st_size = 0;
    ELFAbsoluteAtom<ELFT> *atom = this->createAbsoluteAtom(symbolName, sym, -1);
    this->addAtom(*atom);
  }

  /// \brief add an undefined atom
  virtual void addUndefinedAtom(StringRef symbolName) {
    assert(!symbolName.empty() && "UndefinedAtoms must have a name");
    Elf_Sym *sym = new (this->_readerStorage) Elf_Sym;
    sym->st_name = 0;
    sym->st_value = 0;
    sym->st_shndx = llvm::ELF::SHN_UNDEF;
    sym->setBindingAndType(llvm::ELF::STB_GLOBAL, llvm::ELF::STT_NOTYPE);
    sym->setVisibility(llvm::ELF::STV_DEFAULT);
    sym->st_size = 0;
    ELFUndefinedAtom<ELFT> *atom = this->createUndefinedAtom(symbolName, sym);
    this->addAtom(*atom);
  }
};

template <class ELFT>
std::error_code ELFFile<ELFT>::doParse() {
  std::error_code ec;
  _objFile.reset(new llvm::object::ELFFile<ELFT>(_mb->getBuffer(), ec));
  if (ec)
    return ec;

  if ((ec = createAtomsFromContext()))
    return ec;

  // Read input sections from the input file that need to be converted to
  // atoms
  if ((ec = createAtomizableSections()))
    return ec;

  // For mergeable strings, we would need to split the section into various
  // atoms
  if ((ec = createMergeableAtoms()))
    return ec;

  // Create the necessary symbols that are part of the section that we
  // created in createAtomizableSections function
  if ((ec = createSymbolsFromAtomizableSections()))
    return ec;

  // Create the appropriate atoms from the file
  if ((ec = createAtoms()))
    return ec;
  return std::error_code();
}

template <class ELFT> Reference::KindArch ELFFile<ELFT>::kindArch() {
  switch (_objFile->getHeader()->e_machine) {
  case llvm::ELF::EM_X86_64:
    return Reference::KindArch::x86_64;
  case llvm::ELF::EM_386:
    return Reference::KindArch::x86;
  case llvm::ELF::EM_ARM:
    return Reference::KindArch::ARM;
  case llvm::ELF::EM_HEXAGON:
    return Reference::KindArch::Hexagon;
  case llvm::ELF::EM_MIPS:
    return Reference::KindArch::Mips;
  case llvm::ELF::EM_OPENRISC:
    return Reference::KindArch::OR1K;
  case llvm::ELF::EM_AARCH64:
    return Reference::KindArch::AArch64;
  }
  llvm_unreachable("unsupported e_machine value");
}

template <class ELFT>
std::error_code ELFFile<ELFT>::createAtomizableSections() {
  // Handle: SHT_REL and SHT_RELA sections:
  // Increment over the sections, when REL/RELA section types are found add
  // the contents to the RelocationReferences map.
  // Record the number of relocs to guess at preallocating the buffer.
  uint64_t totalRelocs = 0;
  for (const Elf_Shdr &section : _objFile->sections()) {
    if (isIgnoredSection(&section))
      continue;

    if (isMergeableStringSection(&section)) {
      _mergeStringSections.push_back(&section);
      continue;
    }

    if (section.sh_type == llvm::ELF::SHT_RELA) {
      auto sHdr = _objFile->getSection(section.sh_info);

      auto sectionName = _objFile->getSectionName(sHdr);
      if (std::error_code ec = sectionName.getError())
        return ec;

      auto rai(_objFile->begin_rela(&section));
      auto rae(_objFile->end_rela(&section));

      _relocationAddendReferences[*sectionName] = make_range(rai, rae);
      totalRelocs += std::distance(rai, rae);
    } else if (section.sh_type == llvm::ELF::SHT_REL) {
      auto sHdr = _objFile->getSection(section.sh_info);

      auto sectionName = _objFile->getSectionName(sHdr);
      if (std::error_code ec = sectionName.getError())
        return ec;

      auto ri(_objFile->begin_rel(&section));
      auto re(_objFile->end_rel(&section));

      _relocationReferences[*sectionName] = make_range(ri, re);
      totalRelocs += std::distance(ri, re);
    } else {
      _sectionSymbols[&section];
    }
  }
  _references.reserve(totalRelocs);
  return std::error_code();
}

template <class ELFT> std::error_code ELFFile<ELFT>::createMergeableAtoms() {
  // Divide the section that contains mergeable strings into tokens
  // TODO
  // a) add resolver support to recognize multibyte chars
  // b) Create a separate section chunk to write mergeable atoms
  std::vector<MergeString *> tokens;
  for (const Elf_Shdr *msi : _mergeStringSections) {
    auto sectionName = getSectionName(msi);
    if (std::error_code ec = sectionName.getError())
      return ec;

    auto sectionContents = getSectionContents(msi);
    if (std::error_code ec = sectionContents.getError())
      return ec;

    StringRef secCont(reinterpret_cast<const char *>(sectionContents->begin()),
                      sectionContents->size());

    unsigned int prev = 0;
    for (std::size_t i = 0, e = sectionContents->size(); i != e; ++i) {
      if ((*sectionContents)[i] == '\0') {
        tokens.push_back(new (_readerStorage) MergeString(
            prev, secCont.slice(prev, i + 1), msi, *sectionName));
        prev = i + 1;
      }
    }
  }

  // Create Mergeable atoms
  for (const MergeString *tai : tokens) {
    ArrayRef<uint8_t> content((const uint8_t *)tai->_string.data(),
                              tai->_string.size());
    ELFMergeAtom<ELFT> *atom = createMergedString(tai->_sectionName, tai->_shdr,
                                                  content, tai->_offset);
    atom->setOrdinal(++_ordinal);
    addAtom(*atom);
    _mergeAtoms.push_back(atom);
  }
  return std::error_code();
}

template <class ELFT>
std::error_code ELFFile<ELFT>::createSymbolsFromAtomizableSections() {
  // Increment over all the symbols collecting atoms and symbol names for
  // later use.
  auto SymI = _objFile->begin_symbols(), SymE = _objFile->end_symbols();

  // Skip over dummy sym.
  if (SymI != SymE)
    ++SymI;

  for (; SymI != SymE; ++SymI) {
    const Elf_Shdr *section = _objFile->getSection(&*SymI);

    auto symbolName = _objFile->getSymbolName(SymI);
    if (std::error_code ec = symbolName.getError())
      return ec;

    if (isAbsoluteSymbol(&*SymI)) {
      ELFAbsoluteAtom<ELFT> *absAtom = createAbsoluteAtom(
          *symbolName, &*SymI, (int64_t)getSymbolValue(&*SymI));
      addAtom(*absAtom);
      _symbolToAtomMapping.insert(std::make_pair(&*SymI, absAtom));
    } else if (isUndefinedSymbol(&*SymI)) {
      if (_useWrap &&
          (_wrapSymbolMap.find(*symbolName) != _wrapSymbolMap.end())) {
        auto wrapAtom = _wrapSymbolMap.find(*symbolName);
        _symbolToAtomMapping.insert(
            std::make_pair(&*SymI, wrapAtom->getValue()));
        continue;
      }
      ELFUndefinedAtom<ELFT> *undefAtom =
          createUndefinedAtom(*symbolName, &*SymI);
      addAtom(*undefAtom);
      _symbolToAtomMapping.insert(std::make_pair(&*SymI, undefAtom));
    } else if (isCommonSymbol(&*SymI)) {
      ELFCommonAtom<ELFT> *commonAtom = createCommonAtom(*symbolName, &*SymI);
      commonAtom->setOrdinal(++_ordinal);
      addAtom(*commonAtom);
      _symbolToAtomMapping.insert(std::make_pair(&*SymI, commonAtom));
    } else if (isDefinedSymbol(&*SymI)) {
      _sectionSymbols[section].push_back(SymI);
    } else {
      llvm::errs() << "Unable to create atom for: " << *symbolName << "\n";
      return llvm::object::object_error::parse_failed;
    }
  }

  return std::error_code();
}

template <class ELFT> std::error_code ELFFile<ELFT>::createAtoms() {
  // Holds all the atoms that are part of the section. They are the targets of
  // the kindGroupChild reference.
  llvm::StringMap<std::vector<ELFDefinedAtom<ELFT> *>> atomsForSection;

  // Contains a list of comdat sections for a group.
  for (auto &i : _sectionSymbols) {
    const Elf_Shdr *section = i.first;
    std::vector<Elf_Sym_Iter> &symbols = i.second;

    // Sort symbols by position.
    std::stable_sort(symbols.begin(), symbols.end(),
                     [this](Elf_Sym_Iter a, Elf_Sym_Iter b) {
                       return getSymbolValue(&*a) < getSymbolValue(&*b);
                     });

    ErrorOr<StringRef> sectionName = this->getSectionName(section);
    if (std::error_code ec = sectionName.getError())
      return ec;

    auto sectionContents = getSectionContents(section);
    if (std::error_code ec = sectionContents.getError())
      return ec;

    // SHT_GROUP sections are handled in the following loop.
    if (isGroupSection(section))
      continue;

    bool addAtoms = (!isGnuLinkOnceSection(*sectionName) &&
                     !isSectionMemberOfGroup(section));

    if (handleSectionWithNoSymbols(section, symbols)) {
      ELFDefinedAtom<ELFT> *newAtom =
          createSectionAtom(section, *sectionName, *sectionContents);
      newAtom->setOrdinal(++_ordinal);
      if (addAtoms)
        addAtom(*newAtom);
      else
        atomsForSection[*sectionName].push_back(newAtom);
      continue;
    }

    ELFDefinedAtom<ELFT> *previousAtom = nullptr;
    ELFReference<ELFT> *anonFollowedBy = nullptr;

    for (auto si = symbols.begin(), se = symbols.end(); si != se; ++si) {
      auto symbol = *si;
      StringRef symbolName = "";
      if (symbol->getType() != llvm::ELF::STT_SECTION) {
        auto symName = _objFile->getSymbolName(symbol);
        if (std::error_code ec = symName.getError())
          return ec;
        symbolName = *symName;
      }

      uint64_t contentSize = symbolContentSize(
          section, &*symbol, (si + 1 == se) ? nullptr : &**(si + 1));

      // Check to see if we need to add the FollowOn Reference
      ELFReference<ELFT> *followOn = nullptr;
      if (previousAtom) {
        // Replace the followon atom with the anonymous atom that we created,
        // so that the next symbol that we create is a followon from the
        // anonymous atom.
        if (anonFollowedBy) {
          followOn = anonFollowedBy;
        } else {
          followOn = new (_readerStorage)
              ELFReference<ELFT>(lld::Reference::kindLayoutAfter);
          previousAtom->addReference(followOn);
        }
      }

      ArrayRef<uint8_t> symbolData((const uint8_t *)sectionContents->data() +
                                       getSymbolValue(&*symbol),
                                   contentSize);

      // If the linker finds that a section has global atoms that are in a
      // mergeable section, treat them as defined atoms as they shouldn't be
      // merged away as well as these symbols have to be part of symbol
      // resolution
      if (isMergeableStringSection(section)) {
        if (symbol->getBinding() != llvm::ELF::STB_GLOBAL)
          continue;
        ELFDefinedAtom<ELFT> *atom = createDefinedAtom(
          symbolName, *sectionName, &**si, section, symbolData,
          _references.size(), _references.size(), _references);
        atom->setOrdinal(++_ordinal);
        if (addAtoms)
          addAtom(*atom);
        else
          atomsForSection[*sectionName].push_back(atom);
        continue;
      }

      // Don't allocate content to a weak symbol, as they may be merged away.
      // Create an anonymous atom to hold the data.
      ELFDefinedAtom<ELFT> *anonAtom = nullptr;
      anonFollowedBy = nullptr;
      if (symbol->getBinding() == llvm::ELF::STB_WEAK) {
        // Create anonymous new non-weak ELF symbol that holds the symbol
        // data.
        auto sym = new (_readerStorage) Elf_Sym(*symbol);
        sym->setBinding(llvm::ELF::STB_GLOBAL);
        anonAtom = createDefinedAtomAndAssignRelocations(
            "", *sectionName, sym, section, symbolData, *sectionContents);
        symbolData = ArrayRef<uint8_t>();

        // If this is the last atom, let's not create a followon reference.
        if (anonAtom && (si + 1) != se) {
          anonFollowedBy = new (_readerStorage)
              ELFReference<ELFT>(lld::Reference::kindLayoutAfter);
          anonAtom->addReference(anonFollowedBy);
        }
      }

      ELFDefinedAtom<ELFT> *newAtom = createDefinedAtomAndAssignRelocations(
          symbolName, *sectionName, &*symbol, section, symbolData,
          *sectionContents);
      newAtom->setOrdinal(++_ordinal);

      // If the atom was a weak symbol, let's create a followon reference to
      // the anonymous atom that we created.
      if (anonAtom)
        createEdge(newAtom, anonAtom, Reference::kindLayoutAfter);

      if (previousAtom) {
        // Set the followon atom to the weak atom that we have created, so
        // that they would alias when the file gets written.
        followOn->setTarget(anonAtom ? anonAtom : newAtom);
      }

      // The previous atom is always the atom created before unless the atom
      // is a weak atom.
      previousAtom = anonAtom ? anonAtom : newAtom;

      if (addAtoms)
        addAtom(*newAtom);
      else
        atomsForSection[*sectionName].push_back(newAtom);

      _symbolToAtomMapping.insert(std::make_pair(&*symbol, newAtom));
      if (anonAtom) {
        anonAtom->setOrdinal(++_ordinal);
        if (addAtoms)
          addAtom(*anonAtom);
        else
          atomsForSection[*sectionName].push_back(anonAtom);
      }
    }
  }

  for (auto &i : _sectionSymbols)
    if (std::error_code ec = handleSectionGroup(i.first, atomsForSection))
      return ec;
  for (auto &i : _sectionSymbols)
    if (std::error_code ec = handleGnuLinkOnceSection(i.first, atomsForSection))
      return ec;

  updateReferences();
  return std::error_code();
}

template <class ELFT>
std::error_code ELFFile<ELFT>::handleGnuLinkOnceSection(
    const Elf_Shdr *section,
    llvm::StringMap<std::vector<ELFDefinedAtom<ELFT> *>> &atomsForSection) {
  ErrorOr<StringRef> sectionName = this->getSectionName(section);
  if (std::error_code ec = sectionName.getError())
    return ec;
  if (!isGnuLinkOnceSection(*sectionName))
    return std::error_code();

  unsigned int referenceStart = _references.size();
  std::vector<ELFReference<ELFT> *> refs;
  for (auto ha : atomsForSection[*sectionName]) {
    _groupChild[ha->symbol()] = std::make_pair(*sectionName, section);
    ELFReference<ELFT> *ref =
        new (_readerStorage) ELFReference<ELFT>(lld::Reference::kindGroupChild);
    ref->setTarget(ha);
    refs.push_back(ref);
  }
  atomsForSection[*sectionName].clear();
  // Create a gnu linkonce atom.
  ELFDefinedAtom<ELFT> *atom = createDefinedAtom(
      *sectionName, *sectionName, nullptr, section, ArrayRef<uint8_t>(),
      referenceStart, _references.size(), _references);
  atom->setOrdinal(++_ordinal);
  addAtom(*atom);
  for (auto reference : refs)
    atom->addReference(reference);
  return std::error_code();
}

template <class ELFT>
std::error_code ELFFile<ELFT>::handleSectionGroup(
    const Elf_Shdr *section,
    llvm::StringMap<std::vector<ELFDefinedAtom<ELFT> *>> &atomsForSection) {
  ErrorOr<StringRef> sectionName = this->getSectionName(section);
  if (std::error_code ec = sectionName.getError())
    return ec;
  if (!isGroupSection(section))
    return std::error_code();

  auto sectionContents = getSectionContents(section);
  if (std::error_code ec = sectionContents.getError())
    return ec;

  // A section of type SHT_GROUP defines a grouping of sections. The
  // name of a symbol from one of the containing object's symbol tables
  // provides a signature for the section group. The section header of
  // the SHT_GROUP section specifies the identifying symbol entry, as
  // described: the sh_link member contains the section header index of
  // the symbol table section that contains the entry. The sh_info
  // member contains the symbol table index of the identifying entry.
  // The sh_flags member of the section header contains 0. The name of
  // the section (sh_name) is not specified.
  std::vector<StringRef> sectionNames;
  const Elf_Word *groupMembers =
      reinterpret_cast<const Elf_Word *>(sectionContents->data());
  const size_t count = section->sh_size / sizeof(Elf_Word);
  for (size_t i = 1; i < count; i++) {
    const Elf_Shdr *shdr = _objFile->getSection(groupMembers[i]);
    ErrorOr<StringRef> sectionName = _objFile->getSectionName(shdr);
    if (std::error_code ec = sectionName.getError())
      return ec;
    sectionNames.push_back(*sectionName);
  }
  const Elf_Sym *symbol = _objFile->getSymbol(section->sh_info);
  const Elf_Shdr *symtab = _objFile->getSection(section->sh_link);
  ErrorOr<StringRef> symbolName = _objFile->getSymbolName(symtab, symbol);
  if (std::error_code ec = symbolName.getError())
    return ec;

  unsigned int referenceStart = _references.size();
  std::vector<ELFReference<ELFT> *> refs;
  for (auto name : sectionNames) {
    for (auto ha : atomsForSection[name]) {
      _groupChild[ha->symbol()] = std::make_pair(*symbolName, section);
      ELFReference<ELFT> *ref = new (_readerStorage)
          ELFReference<ELFT>(lld::Reference::kindGroupChild);
      ref->setTarget(ha);
      refs.push_back(ref);
    }
    atomsForSection[name].clear();
  }

  // Create an atom for comdat signature.
  ELFDefinedAtom<ELFT> *atom = createDefinedAtom(
      *symbolName, *sectionName, nullptr, section, ArrayRef<uint8_t>(),
      referenceStart, _references.size(), _references);
  atom->setOrdinal(++_ordinal);
  addAtom(*atom);
  for (auto reference : refs)
    atom->addReference(reference);
  return std::error_code();
}

template <class ELFT> std::error_code ELFFile<ELFT>::createAtomsFromContext() {
  if (!_useWrap)
    return std::error_code();
  // Steps:
  // a) Create an undefined atom for the symbol specified by the --wrap option,
  //    as that may be needed to be pulled from an archive.
  // b) Create an undefined atom for __wrap_<symbolname>.
  // c) All references to the symbol specified by wrap should point to
  //    __wrap_<symbolname>
  // d) All references to __real_symbol should point to the <symbol>
  for (auto &wrapsym : _ctx.wrapCalls()) {
    StringRef wrapStr = wrapsym.getKey();
    // Create a undefined symbol fror the wrap symbol.
    UndefinedAtom *wrapSymAtom =
        new (_readerStorage) SimpleUndefinedAtom(*this, wrapStr);
    StringRef wrapCallSym =
        _ctx.allocateString((llvm::Twine("__wrap_") + wrapStr).str());
    StringRef realCallSym =
        _ctx.allocateString((llvm::Twine("__real_") + wrapStr).str());
    UndefinedAtom *wrapCallAtom =
        new (_readerStorage) SimpleUndefinedAtom(*this, wrapCallSym);
    // Create maps, when there is call to sym, it should point to wrapCallSym.
    _wrapSymbolMap.insert(std::make_pair(wrapStr, wrapCallAtom));
    // Whenever there is a reference to realCall it should point to the symbol
    // created for each wrap usage.
    _wrapSymbolMap.insert(std::make_pair(realCallSym, wrapSymAtom));
    addAtom(*wrapSymAtom);
    addAtom(*wrapCallAtom);
  }
  return std::error_code();
}

template <class ELFT>
ELFDefinedAtom<ELFT> *ELFFile<ELFT>::createDefinedAtomAndAssignRelocations(
    StringRef symbolName, StringRef sectionName, const Elf_Sym *symbol,
    const Elf_Shdr *section, ArrayRef<uint8_t> symContent,
    ArrayRef<uint8_t> secContent) {
  unsigned int referenceStart = _references.size();

  // Add Rela (those with r_addend) references:
  auto rari = _relocationAddendReferences.find(sectionName);
  if (rari != _relocationAddendReferences.end())
    createRelocationReferences(symbol, symContent, rari->second);

  // Add Rel references.
  auto rri = _relocationReferences.find(sectionName);
  if (rri != _relocationReferences.end())
    createRelocationReferences(symbol, symContent, secContent, rri->second);

  // Create the DefinedAtom and add it to the list of DefinedAtoms.
  return createDefinedAtom(symbolName, sectionName, symbol, section, symContent,
                           referenceStart, _references.size(), _references);
}

template <class ELFT>
void ELFFile<ELFT>::createRelocationReferences(const Elf_Sym *symbol,
                                               ArrayRef<uint8_t> content,
                                               range<Elf_Rela_Iter> rels) {
  bool isMips64EL = _objFile->isMips64EL();
  const auto symValue = getSymbolValue(symbol);
  for (const auto &rel : rels) {
    if (rel.r_offset < symValue ||
        symValue + content.size() <= rel.r_offset)
      continue;
    auto elfRelocation = new (_readerStorage)
        ELFReference<ELFT>(&rel, rel.r_offset - symValue, kindArch(),
                           rel.getType(isMips64EL), rel.getSymbol(isMips64EL));
    addReferenceToSymbol(elfRelocation, symbol);
    _references.push_back(elfRelocation);
  }
}

template <class ELFT>
void ELFFile<ELFT>::createRelocationReferences(const Elf_Sym *symbol,
                                               ArrayRef<uint8_t> symContent,
                                               ArrayRef<uint8_t> secContent,
                                               range<Elf_Rel_Iter> rels) {
  bool isMips64EL = _objFile->isMips64EL();
  const auto symValue = getSymbolValue(symbol);
  for (const auto &rel : rels) {
    if (rel.r_offset < symValue ||
        symValue + symContent.size() <= rel.r_offset)
      continue;
    auto elfRelocation = new (_readerStorage)
        ELFReference<ELFT>(rel.r_offset - symValue, kindArch(),
                           rel.getType(isMips64EL), rel.getSymbol(isMips64EL));
    Reference::Addend addend = getInitialAddend(symContent, symValue, rel);
    elfRelocation->setAddend(addend);
    addReferenceToSymbol(elfRelocation, symbol);
    _references.push_back(elfRelocation);
  }
}

template <class ELFT>
void ELFFile<ELFT>::updateReferenceForMergeStringAccess(ELFReference<ELFT> *ref,
                                                        const Elf_Sym *symbol,
                                                        const Elf_Shdr *shdr) {
  // If the target atom is mergeable strefng atom, the atom might have been
  // merged with other atom having the same contents. Try to find the
  // merged one if that's the case.
  int64_t addend = ref->addend();
  if (addend < 0)
    addend = 0;

  const MergeSectionKey ms = {shdr, addend};
  auto msec = _mergedSectionMap.find(ms);
  if (msec != _mergedSectionMap.end()) {
    ref->setTarget(msec->second);
    return;
  }

  // The target atom was not merged. Mergeable atoms are not in
  // _symbolToAtomMapping, so we cannot find it by calling findAtom(). We
  // instead call findMergeAtom().
  if (symbol->getType() != llvm::ELF::STT_SECTION)
    addend = getSymbolValue(symbol) + addend;
  ELFMergeAtom<ELFT> *mergedAtom = findMergeAtom(shdr, addend);
  ref->setOffset(addend - mergedAtom->offset());
  ref->setAddend(0);
  ref->setTarget(mergedAtom);
}

template <class ELFT> void ELFFile<ELFT>::updateReferences() {
  for (auto &ri : _references) {
    if (ri->kindNamespace() != lld::Reference::KindNamespace::ELF)
      continue;
    const Elf_Sym *symbol = _objFile->getSymbol(ri->targetSymbolIndex());
    const Elf_Shdr *shdr = _objFile->getSection(symbol);

    // If the atom is not in mergeable string section, the target atom is
    // simply that atom.
    if (isMergeableStringSection(shdr))
      updateReferenceForMergeStringAccess(ri, symbol, shdr);
    else
      ri->setTarget(findAtom(findSymbolForReference(ri), symbol));
  }
}

template <class ELFT>
bool ELFFile<ELFT>::isIgnoredSection(const Elf_Shdr *section) {
  switch (section->sh_type) {
  case llvm::ELF::SHT_NULL:
  case llvm::ELF::SHT_STRTAB:
  case llvm::ELF::SHT_SYMTAB:
  case llvm::ELF::SHT_SYMTAB_SHNDX:
    return true;
  default:
    break;
  }
  return false;
}

template <class ELFT>
bool ELFFile<ELFT>::isMergeableStringSection(const Elf_Shdr *section) {
  if (_doStringsMerge && section) {
    int64_t sectionFlags = section->sh_flags;
    sectionFlags &= ~llvm::ELF::SHF_ALLOC;
    // Mergeable string sections have both SHF_MERGE and SHF_STRINGS flags
    // set. sh_entsize is the size of each character which is normally 1.
    if ((section->sh_entsize < 2) &&
        (sectionFlags == (llvm::ELF::SHF_MERGE | llvm::ELF::SHF_STRINGS))) {
      return true;
    }
  }
  return false;
}

template <class ELFT>
ELFDefinedAtom<ELFT> *
ELFFile<ELFT>::createSectionAtom(const Elf_Shdr *section, StringRef sectionName,
                                 ArrayRef<uint8_t> content) {
  Elf_Sym *sym = new (_readerStorage) Elf_Sym;
  sym->st_name = 0;
  sym->setBindingAndType(llvm::ELF::STB_LOCAL, llvm::ELF::STT_SECTION);
  sym->st_other = 0;
  sym->st_shndx = 0;
  sym->st_value = 0;
  sym->st_size = 0;
  auto *newAtom = createDefinedAtomAndAssignRelocations(
      "", sectionName, sym, section, content, content);
  newAtom->setOrdinal(++_ordinal);
  return newAtom;
}

template <class ELFT>
uint64_t ELFFile<ELFT>::symbolContentSize(const Elf_Shdr *section,
                                          const Elf_Sym *symbol,
                                          const Elf_Sym *nextSymbol) {
  const auto symValue = getSymbolValue(symbol);
  // if this is the last symbol, take up the remaining data.
  return nextSymbol ? getSymbolValue(nextSymbol) - symValue
                    : section->sh_size - symValue;
}

template <class ELFT>
void ELFFile<ELFT>::createEdge(ELFDefinedAtom<ELFT> *from,
                               ELFDefinedAtom<ELFT> *to, uint32_t edgeKind) {
  auto reference = new (_readerStorage) ELFReference<ELFT>(edgeKind);
  reference->setTarget(to);
  from->addReference(reference);
}

/// Does the atom need to be redirected using a separate undefined atom?
template <class ELFT>
bool ELFFile<ELFT>::redirectReferenceUsingUndefAtom(
    const Elf_Sym *sourceSymbol, const Elf_Sym *targetSymbol) const {
  auto groupChildTarget = _groupChild.find(targetSymbol);

  // If the reference is not to a group child atom, there is no need to redirect
  // using a undefined atom. Its also not needed if the source and target are
  // from the same section.
  if ((groupChildTarget == _groupChild.end()) ||
      (sourceSymbol->st_shndx == targetSymbol->st_shndx))
    return false;

  auto groupChildSource = _groupChild.find(sourceSymbol);

  // If the source symbol is not in a group, use a undefined symbol too.
  if (groupChildSource == _groupChild.end())
    return true;

  // If the source and child are from the same group, we dont need the
  // relocation to go through a undefined symbol.
  if (groupChildSource->second.second == groupChildTarget->second.second)
    return false;

  return true;
}

} // end namespace elf
} // end namespace lld

#endif // LLD_READER_WRITER_ELF_FILE_H
