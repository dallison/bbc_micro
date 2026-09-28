//
//  elf_reader.c
//  c_compiler
//
//  Created by David Allison on 1/9/18.
//  Copyright © 2018 David Allison. All rights reserved.
//

#include "elf_reader.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

ELFReaderSection* NewELFReaderSection() {
  ELFReaderSection* section = malloc(sizeof(ELFReaderSection));
  StringInit(&section->name, "");
  section->contents = NULL;
  section->address = 0;
  section->offset = 0;
  section->output_section_index = 0;
  section->section_symbol_index = -1;
  section->discarded = false;
  VectorInit(&section->gc_refs);
  return section;
}

void ELFReaderSectionDelete(ELFReaderSection* section) {
  StringDestruct(&section->name);
  VectorDestruct(&section->gc_refs);
}

void ELFReaderFileInit(ELFReaderFile* elf, String* filename) {
  StringInit(&elf->filename, filename->value);
  elf->header = NULL;
  VectorInit(&elf->sections);
  VectorInit(&elf->segments);
  elf->section_names = NULL;
  elf->ops = NULL;
  elf->base = NULL;
  elf->owns_decoded = false;
}

ELFReaderFile* NewELFReaderFile(String* filename) {
  ELFReaderFile* elf = malloc(sizeof(ELFReaderFile));
  ELFReaderFileInit(elf, filename);
  return elf;
}

// Read the contents of the ELF file mapped into memory at
// the address 'addr'.
bool ReadFileContents(ELFReaderFile* elf, void* addr, int64_t length) {
  elf->file_length = length;
  elf->base = addr;

  // Address of the header as char* so we can perform math on it.
  const char* header_addr = addr;
  
  // Check that this is an ELF file by looking at the magic number at the start
  // of the header.
  if (header_addr[EI_MAG0] != '\x7f' || header_addr[EI_MAG1] != 'E' || header_addr[EI_MAG2] != 'L' ||
      header_addr[EI_MAG3] != 'F') {
    return false;
  }

  // Select the format operations from the file's class byte.
  bool is_64_bit =
      (uint8_t)header_addr[EI_CLASS] != ELFCLASS32;  // default to 64-bit.
  elf->ops = ELFFormatOpsFor(is_64_bit);

  // For ELF64 the on-disk layout matches the canonical (wide) in-memory
  // layout, so the header and section/segment headers can be referenced
  // directly from the mapping.  For ELF32 they have to be decoded into owned
  // wide structures.
  elf->owns_decoded = !is_64_bit;

  // Read the file header.
  if (elf->owns_decoded) {
    ELFHeader* header = malloc(sizeof(ELFHeader));
    elf->ops->ReadHeader(header, header_addr);
    elf->header = header;
  } else {
    elf->header = (ELFHeader*)addr;
  }

  // Read the section headers.
  // The first section header is at header->shoff bytes from the
  // file header.
  const char* section_header = header_addr + elf->header->shoff;
  for (int i = 0; i < elf->header->shnum; i++) {
    ELFReaderSection* section = NewELFReaderSection();
    if (elf->owns_decoded) {
      ELFSectionHeader* hdr = malloc(sizeof(ELFSectionHeader));
      elf->ops->ReadSectionHeader(hdr, section_header);
      section->header = hdr;
    } else {
      section->header = (ELFSectionHeader*)section_header;
    }
    VectorAppend(&elf->sections, section);
    section_header += elf->ops->section_header_size;   // Next section header.
  }
  
  // Read the section name string table.
  ELFReaderSection* section_names = elf->sections.value.p[elf->header->shstrndx];
  elf->section_names = header_addr + section_names->header->offset;
  
  // Now that we have the section names in memory we can initialize
  // the sections.
  for (size_t i = 0; i < elf->sections.length; i++) {
    ELFReaderSection* section = elf->sections.value.p[i];
    StringSet(&section->name, elf->section_names + section->header->name);
    section->contents = (void*)(header_addr + section->header->offset);
  }
  
  // Read the program segments and insert them into the segments vector.
  const char* segment_header = header_addr + elf->header->phoff;
  for (int i = 0; i < elf->header->phnum; i++) {
    if (elf->owns_decoded) {
      ELFProgramHeader* segment = malloc(sizeof(ELFProgramHeader));
      elf->ops->ReadProgramHeader(segment, segment_header);
      VectorAppend(&elf->segments, segment);
    } else {
      ELFProgramHeader* segment = (ELFProgramHeader*)segment_header;
      VectorAppend(&elf->segments, segment);
    }
    segment_header += elf->ops->program_header_size;   // Next segment header.
  }
  return true;
}

// Read and ELF file from disk.  This maps the file into memory using the
// UNIX 'mmap' call.  The 'mmap' function takes the file descriptor of an
// open file and maps its contents into the virtual memory of the process.
// The address is chosen by the memory manager.
bool ELFReaderFileRead(ELFReaderFile* elf, int64_t length, int64_t offset) {
  if (length == 0) {
    // First get the length of the file if needed.
    int e = stat(elf->filename.value, &elf->file_stat);
    if (e != 0) {
      return false;
    }
    length = elf->file_stat.st_size;
  }
  
  // Open the file.
  int fd = open(elf->filename.value, O_RDONLY);
  if (fd < 0) {
    return false;
  }

  // Offset must be page aligned.
  int page_size = (int)sysconf(_SC_PAGESIZE);
  int page_mask = page_size - 1;
  int64_t aligned_offset = offset & ~page_mask;
  int64_t addr_diff = offset & page_mask;
  int64_t full_length = length + addr_diff;
  
  // Map it into memory.
  void* addr = mmap(NULL, full_length, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd,
                    aligned_offset);
  if (addr == MAP_FAILED) {
    close(fd);
    return false;
  }

  // Now we have the file mapped into memory at address 'addr'.  We can
  // access this memory directly.  It is mapped read-only so we can't
  // write to it.
  void* start_addr = (char*)addr + addr_diff;
  bool ok = ReadFileContents(elf, start_addr, length);
  if (!ok) {
    munmap(addr, length);
  }
  close(fd);
  return ok;
}

void ELFReaderFileDestruct(ELFReaderFile* elf) {
  // For ELF32 the header and the per-section/segment headers were decoded into
  // heap-allocated wide structures, so free them here.  For ELF64 they point
  // directly into the file mapping and are not owned.
  for (size_t i = 0; i < elf->sections.length; i++) {
    ELFReaderSection* section = elf->sections.value.p[i];
    if (elf->owns_decoded) {
      free(section->header);
    }
    ELFReaderSectionDelete(section);
  }
  if (elf->owns_decoded) {
    for (size_t i = 0; i < elf->segments.length; i++) {
      free(elf->segments.value.p[i]);
    }
    free(elf->header);
    elf->header = NULL;
  }
  VectorDestruct(&elf->sections);
  VectorDestruct(&elf->segments);
}

void ELFReaderFileDelete(ELFReaderFile* elf) {
  ELFReaderFileDestruct(elf);
  free(elf);
}

ELFReaderSection* ELFReaderFileFindSection(ELFReaderFile* elf, const char* name) {
  for (size_t i = 0; i < elf->sections.length; i++) {
    ELFReaderSection* section = elf->sections.value.p[i];
    if (StringEqual(&section->name, name)) {
      return section;
    }
  }
  return NULL;
}

void ELFReaderFileFindSectionsByType(ELFReaderFile* elf, int32_t type, Vector* output) {
  for (size_t i = 0; i < elf->sections.length; i++) {
    ELFReaderSection* section = elf->sections.value.p[i];
    if (section->header->type == type) {
      VectorAppend(output, section);
    }
  }
}
