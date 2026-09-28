//
//  elf_format.c
//  c_compiler
//
//  ELF32 and ELF64 serialization/deserialization primitives and the generic
//  ELFFormatOps dispatch tables.
//

#include "elf_format.h"

#include <string.h>

// ===========================================================================
// ELF64 primitives.
//
// The canonical in-memory structures are already in the ELF64 on-disk layout,
// so these are mostly straight copies.
// ===========================================================================

static void ELF64WriteHeader(const ELFHeader* hdr, FILE* fp) {
  fwrite(hdr, sizeof(ELF64Header), 1, fp);
}

static void ELF64WriteSectionHeader(const ELFSectionHeader* hdr, FILE* fp) {
  fwrite(hdr, sizeof(ELF64SectionHeader), 1, fp);
}

static void ELF64WriteProgramHeader(const ELFProgramHeader* hdr, FILE* fp) {
  fwrite(hdr, sizeof(ELF64ProgramHeader), 1, fp);
}

static void ELF64WriteSymbol(const ELFSymbol* sym, FILE* fp) {
  fwrite(sym, sizeof(ELF64Symbol), 1, fp);
}

static void ELF64WriteRelocation(const ELFRelocation* rel, FILE* fp) {
  fwrite(rel, sizeof(ELF64Relocation), 1, fp);
}

static void ELF64WriteDynamicEntry(const ELFDynamicSectionEntry* dyn, FILE* fp) {
  fwrite(dyn, sizeof(ELF64DynamicSectionEntry), 1, fp);
}

static void ELF64ReadHeader(ELFHeader* dst, const void* src) {
  memcpy(dst, src, sizeof(ELF64Header));
}

static void ELF64ReadSectionHeader(ELFSectionHeader* dst, const void* src) {
  memcpy(dst, src, sizeof(ELF64SectionHeader));
}

static void ELF64ReadProgramHeader(ELFProgramHeader* dst, const void* src) {
  memcpy(dst, src, sizeof(ELF64ProgramHeader));
}

static void ELF64ReadSymbol(ELFSymbol* dst, const void* src) {
  memcpy(dst, src, sizeof(ELF64Symbol));
}

static void ELF64ReadRelocation(ELFRelocation* dst, const void* src) {
  memcpy(dst, src, sizeof(ELF64Relocation));
}

static ELF_Xword ELF64EncodeRelocationInfo(uint32_t symbol, uint32_t type) {
  return ELF64_R_INFO(symbol, type);
}

static uint32_t ELF64DecodeRelocationSym(ELF_Xword info) {
  return ELF64_R_SYM(info);
}

static uint32_t ELF64DecodeRelocationType(ELF_Xword info) {
  return ELF64_R_TYPE(info);
}

// ===========================================================================
// ELF32 primitives.
//
// These convert between the wide (canonical) in-memory structures and the
// narrow ELF32 on-disk structures.
// ===========================================================================

static void ELF32WriteHeader(const ELFHeader* hdr, FILE* fp) {
  ELF32Header out;
  memset(&out, 0, sizeof(out));
  memcpy(out.ident, hdr->ident, sizeof(out.ident));
  out.type = hdr->type;
  out.machine = hdr->machine;
  out.version = hdr->version;
  out.entry = (ELF32_Addr)hdr->entry;
  out.phoff = (ELF32_Off)hdr->phoff;
  out.shoff = (ELF32_Off)hdr->shoff;
  out.flags = hdr->flags;
  out.ehsize = hdr->ehsize;
  out.phentsize = hdr->phentsize;
  out.phnum = hdr->phnum;
  out.shentsize = hdr->shentsize;
  out.shnum = hdr->shnum;
  out.shstrndx = hdr->shstrndx;
  fwrite(&out, sizeof(out), 1, fp);
}

static void ELF32WriteSectionHeader(const ELFSectionHeader* hdr, FILE* fp) {
  ELF32SectionHeader out;
  memset(&out, 0, sizeof(out));
  out.name = hdr->name;
  out.type = hdr->type;
  out.flags = (ELF32_Word)hdr->flags;
  out.addr = (ELF32_Addr)hdr->addr;
  out.offset = (ELF32_Off)hdr->offset;
  out.size = (ELF32_Word)hdr->size;
  out.link = hdr->link;
  out.info = hdr->info;
  out.addralign = (ELF32_Word)hdr->addralign;
  out.entsize = (ELF32_Word)hdr->entsize;
  fwrite(&out, sizeof(out), 1, fp);
}

static void ELF32WriteProgramHeader(const ELFProgramHeader* hdr, FILE* fp) {
  ELF32ProgramHeader out;
  memset(&out, 0, sizeof(out));
  out.type = hdr->type;
  out.offset = (ELF32_Off)hdr->offset;
  out.vaddr = (ELF32_Addr)hdr->vaddr;
  out.paddr = (ELF32_Addr)hdr->paddr;
  out.filesz = (ELF32_Word)hdr->filesz;
  out.memsz = (ELF32_Word)hdr->memsz;
  out.flags = hdr->flags;
  out.align = (ELF32_Word)hdr->align;
  fwrite(&out, sizeof(out), 1, fp);
}

static void ELF32WriteSymbol(const ELFSymbol* sym, FILE* fp) {
  ELF32Symbol out;
  memset(&out, 0, sizeof(out));
  out.name = sym->name;
  out.value = (ELF32_Addr)sym->value;
  out.size = (ELF32_Word)sym->size;
  out.info = sym->info;
  out.other = sym->other;
  out.shndx = sym->shndx;
  fwrite(&out, sizeof(out), 1, fp);
}

static void ELF32WriteRelocation(const ELFRelocation* rel, FILE* fp) {
  ELF32Rel out;
  memset(&out, 0, sizeof(out));
  out.offset = (ELF32_Addr)rel->offset;
  // The in-memory 'info' uses the ELF64 encoding; re-encode for ELF32.
  uint32_t sym = ELF64_R_SYM(rel->info);
  uint32_t type = ELF64_R_TYPE(rel->info);
  out.info = ELF32_R_INFO(sym, type);
  fwrite(&out, sizeof(out), 1, fp);
}

static void ELF32WriteDynamicEntry(const ELFDynamicSectionEntry* dyn, FILE* fp) {
  ELF32DynamicSectionEntry out;
  memset(&out, 0, sizeof(out));
  out.tag = (ELF32_Sword)dyn->tag;
  out.un.val = (ELF32_Word)dyn->un.val;
  fwrite(&out, sizeof(out), 1, fp);
}

static void ELF32ReadHeader(ELFHeader* dst, const void* src) {
  const ELF32Header* in = src;
  memset(dst, 0, sizeof(*dst));
  memcpy(dst->ident, in->ident, sizeof(dst->ident));
  dst->type = in->type;
  dst->machine = in->machine;
  dst->version = in->version;
  dst->entry = in->entry;
  dst->phoff = in->phoff;
  dst->shoff = in->shoff;
  dst->flags = in->flags;
  dst->ehsize = in->ehsize;
  dst->phentsize = in->phentsize;
  dst->phnum = in->phnum;
  dst->shentsize = in->shentsize;
  dst->shnum = in->shnum;
  dst->shstrndx = in->shstrndx;
}

static void ELF32ReadSectionHeader(ELFSectionHeader* dst, const void* src) {
  const ELF32SectionHeader* in = src;
  memset(dst, 0, sizeof(*dst));
  dst->name = in->name;
  dst->type = in->type;
  dst->flags = in->flags;
  dst->addr = in->addr;
  dst->offset = in->offset;
  dst->size = in->size;
  dst->link = in->link;
  dst->info = in->info;
  dst->addralign = in->addralign;
  dst->entsize = in->entsize;
}

static void ELF32ReadProgramHeader(ELFProgramHeader* dst, const void* src) {
  const ELF32ProgramHeader* in = src;
  memset(dst, 0, sizeof(*dst));
  dst->type = in->type;
  dst->flags = in->flags;
  dst->offset = in->offset;
  dst->vaddr = in->vaddr;
  dst->paddr = in->paddr;
  dst->filesz = in->filesz;
  dst->memsz = in->memsz;
  dst->align = in->align;
}

static void ELF32ReadSymbol(ELFSymbol* dst, const void* src) {
  const ELF32Symbol* in = src;
  memset(dst, 0, sizeof(*dst));
  dst->name = in->name;
  dst->info = in->info;
  dst->other = in->other;
  dst->shndx = in->shndx;
  dst->value = in->value;
  dst->size = in->size;
}

static void ELF32ReadRelocation(ELFRelocation* dst, const void* src) {
  const ELF32Relocation* in = src;
  memset(dst, 0, sizeof(*dst));
  dst->offset = in->offset;
  // Decode the ELF32 'info' encoding and store it in the canonical (ELF64)
  // encoding.
  uint32_t sym = ELF32_R_SYM(in->info);
  uint32_t type = ELF32_R_TYPE(in->info);
  dst->info = ELF64_R_INFO(sym, type);
  dst->addend = in->addend;
}

static ELF_Xword ELF32EncodeRelocationInfo(uint32_t symbol, uint32_t type) {
  return ELF32_R_INFO(symbol, type);
}

static uint32_t ELF32DecodeRelocationSym(ELF_Xword info) {
  return ELF32_R_SYM(info);
}

static uint32_t ELF32DecodeRelocationType(ELF_Xword info) {
  return ELF32_R_TYPE(info);
}

// ===========================================================================
// Dispatch tables.
// ===========================================================================

static const ELFFormatOps kELF64FormatOps = {
    .is_64_bit = true,
    .header_size = sizeof(ELF64Header),
    .section_header_size = sizeof(ELF64SectionHeader),
    .program_header_size = sizeof(ELF64ProgramHeader),
    .symbol_size = sizeof(ELF64Symbol),
    .relocation_size = sizeof(ELF64Relocation),
    .dynamic_entry_size = sizeof(ELF64DynamicSectionEntry),
    .WriteHeader = ELF64WriteHeader,
    .WriteSectionHeader = ELF64WriteSectionHeader,
    .WriteProgramHeader = ELF64WriteProgramHeader,
    .WriteSymbol = ELF64WriteSymbol,
    .WriteRelocation = ELF64WriteRelocation,
    .WriteDynamicEntry = ELF64WriteDynamicEntry,
    .ReadHeader = ELF64ReadHeader,
    .ReadSectionHeader = ELF64ReadSectionHeader,
    .ReadProgramHeader = ELF64ReadProgramHeader,
    .ReadSymbol = ELF64ReadSymbol,
    .ReadRelocation = ELF64ReadRelocation,
    .EncodeRelocationInfo = ELF64EncodeRelocationInfo,
    .DecodeRelocationSym = ELF64DecodeRelocationSym,
    .DecodeRelocationType = ELF64DecodeRelocationType,
};

static const ELFFormatOps kELF32FormatOps = {
    .is_64_bit = false,
    .header_size = sizeof(ELF32Header),
    .section_header_size = sizeof(ELF32SectionHeader),
    .program_header_size = sizeof(ELF32ProgramHeader),
    .symbol_size = sizeof(ELF32Symbol),
    .relocation_size = sizeof(ELF32Rel),
    .dynamic_entry_size = sizeof(ELF32DynamicSectionEntry),
    .WriteHeader = ELF32WriteHeader,
    .WriteSectionHeader = ELF32WriteSectionHeader,
    .WriteProgramHeader = ELF32WriteProgramHeader,
    .WriteSymbol = ELF32WriteSymbol,
    .WriteRelocation = ELF32WriteRelocation,
    .WriteDynamicEntry = ELF32WriteDynamicEntry,
    .ReadHeader = ELF32ReadHeader,
    .ReadSectionHeader = ELF32ReadSectionHeader,
    .ReadProgramHeader = ELF32ReadProgramHeader,
    .ReadSymbol = ELF32ReadSymbol,
    .ReadRelocation = ELF32ReadRelocation,
    .EncodeRelocationInfo = ELF32EncodeRelocationInfo,
    .DecodeRelocationSym = ELF32DecodeRelocationSym,
    .DecodeRelocationType = ELF32DecodeRelocationType,
};

const ELFFormatOps* ELFFormatOpsFor(bool is_64_bit) {
  return is_64_bit ? &kELF64FormatOps : &kELF32FormatOps;
}

void ELFFormatReadRelocation(const ELFFormatOps* ops, bool is_rela,
                             ELFRelocation* dst, const void* src) {
  if (is_rela) {
    ops->ReadRelocation(dst, src);
    return;
  }
  if (ops->is_64_bit) {
    ELF_Xword in[2];
    memcpy(in, src, sizeof(in));
    memset(dst, 0, sizeof(*dst));
    dst->offset = in[0];
    dst->info = in[1];
    return;
  }
  const ELF32Rel* in = src;
  memset(dst, 0, sizeof(*dst));
  dst->offset = in->offset;
  dst->info =
      ELF64_R_INFO(ELF32_R_SYM(in->info), ELF32_R_TYPE(in->info));
}
