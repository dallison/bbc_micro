//
//  loader_lifecycle.c
//  c_compiler
//
//  Per-image lifecycle state, dependency-order DSO initialization, reverse
//  finalization after partial init, and init/fini array enumeration.
//

#include "loader_lifecycle.h"
#include "elf.h"
#include <stdlib.h>
#include <string.h>

static LoadedDynamicLibrary* LoaderMainImage(Loader* loader) {
  if (loader == NULL) {
    return NULL;
  }
  if (loader->dynamic_lib != NULL) {
    return loader->dynamic_lib;
  }
  return NULL;
}

static bool LoaderImageIsExecutable(Loader* loader, LoadedDynamicLibrary* lib) {
  if (loader == NULL) {
    return false;
  }
  if (lib == NULL) {
    return true;
  }
  if (loader->dynamic_lib != NULL && lib == loader->dynamic_lib) {
    return true;
  }
  return lib->header != NULL && lib->header->type == ET(exec);
}

static size_t LoaderImagePointerSize(Loader* loader, LoadedDynamicLibrary* lib) {
  const unsigned char* ident = NULL;
  if (lib != NULL && lib->header != NULL) {
    ident = (const unsigned char*)lib->header;
  } else if (loader != NULL && loader->elf_file != NULL &&
             loader->elf_file->header != NULL) {
    ident = (const unsigned char*)loader->elf_file->header;
  }
  if (ident == NULL) {
    return 8;
  }
  return ident[EI_CLASS] == ELFCLASS32 ? 4 : 8;
}

static bool PhaseUsesReverseIteration(LoaderLifecyclePhase phase) {
  return phase == kLoaderLifecycleFini;
}

static int32_t PhaseSectionType(LoaderLifecyclePhase phase) {
  switch (phase) {
    case kLoaderLifecyclePreinit:
      return SHT(preinit_array);
    case kLoaderLifecycleInit:
      return SHT(init_array);
    case kLoaderLifecycleFini:
      return SHT(fini_array);
  }
  return SHT(null);
}

static bool PhaseDynamicTags(LoaderLifecyclePhase phase,
                             ELFDynamicTag* addr_tag,
                             ELFDynamicTag* size_tag) {
  switch (phase) {
    case kLoaderLifecyclePreinit:
      *addr_tag = DT(preinit_array);
      *size_tag = DT(preinit_arraysz);
      return true;
    case kLoaderLifecycleInit:
      *addr_tag = DT(init_array);
      *size_tag = DT(init_arraysz);
      return true;
    case kLoaderLifecycleFini:
      *addr_tag = DT(fini_array);
      *size_tag = DT(fini_arraysz);
      return true;
  }
  return false;
}

static bool GetFunctionArrayFromDynamicTags(Loader* loader,
                                            LoadedDynamicLibrary* lib,
                                            LoaderLifecyclePhase phase,
                                            uint64_t* runtime_start,
                                            size_t* entry_count,
                                            size_t* entry_size) {
  if (lib == NULL || lib->dynamic == NULL) {
    return false;
  }
  ELFDynamicTag addr_tag = DT(null);
  ELFDynamicTag size_tag = DT(null);
  if (!PhaseDynamicTags(phase, &addr_tag, &size_tag)) {
    return false;
  }
  const void* array_start =
      DynamicLoaderFindDynamicSectionAddressEntry(lib, addr_tag);
  int64_t array_size =
      DynamicLoaderFindDynamicSectionOffsetEntry(lib, size_tag);
  if (array_start == NULL || array_size <= 0) {
    return false;
  }
  size_t pointer_size = LoaderImagePointerSize(loader, lib);
  if ((size_t)array_size % pointer_size != 0) {
    LoaderError("Malformed dynamic lifecycle array in %s\n",
                lib->filename.value);
    return false;
  }
  *runtime_start = (uint64_t)(uintptr_t)array_start;
  *entry_count = (size_t)array_size / pointer_size;
  *entry_size = pointer_size;
  return true;
}

static bool GetFunctionArrayFromStaticSections(Loader* loader,
                                             LoadedDynamicLibrary* lib,
                                             LoaderLifecyclePhase phase,
                                             uint64_t* runtime_start,
                                             size_t* entry_count,
                                             size_t* entry_size) {
  int32_t section_type = PhaseSectionType(phase);
  if (lib == NULL) {
    if (loader == NULL || loader->elf_file == NULL) {
      return false;
    }
    Vector sections;
    VectorInit(&sections);
    ELFReaderFileFindSectionsByType(loader->elf_file, section_type, &sections);
    if (sections.length == 0) {
      VectorDestruct(&sections);
      return false;
    }
    if (sections.length != 1) {
      LoaderError("Multiple function array sections of type %d\n",
                  section_type);
      VectorDestruct(&sections);
      return false;
    }
    ELFReaderSection* section = sections.value.p[0];
    VectorDestruct(&sections);
    size_t size = (size_t)section->header->entsize;
    if (size == 0) {
      size = LoaderImagePointerSize(loader, NULL);
    }
    if ((size != 2 && size != 4 && size != 8) ||
        section->header->size % size != 0) {
      LoaderError("Malformed function array section %s\n", section->name.value);
      return false;
    }
    uint64_t runtime = 0;
    if (!LoaderLinkedAddressToRuntime(loader, NULL, section->header->addr,
                                      &runtime)) {
      LoaderError("Cannot translate function array section %s\n",
                  section->name.value);
      return false;
    }
    *runtime_start = runtime;
    *entry_count = (size_t)(section->header->size / size);
    *entry_size = size;
    return true;
  }

  if (lib->section_headers == NULL || lib->header == NULL) {
    return false;
  }
  const ELFSectionHeader* found = NULL;
  for (int i = 0; i < lib->header->shnum; i++) {
    const ELFSectionHeader* section = &lib->section_headers[i];
    if (section->type == section_type) {
      if (found != NULL) {
        LoaderError("Multiple function array sections in %s\n",
                    lib->filename.value);
        return false;
      }
      found = section;
    }
  }
  if (found == NULL) {
    return false;
  }
  size_t size = (size_t)found->entsize;
  if (size == 0) {
    size = LoaderImagePointerSize(loader, lib);
  }
  if ((size != 2 && size != 4 && size != 8) || found->size % size != 0) {
    LoaderError("Malformed function array section in %s\n", lib->filename.value);
    return false;
  }
  uint64_t runtime = 0;
  if (!LoaderLinkedAddressToRuntime(loader, lib, found->addr, &runtime)) {
    LoaderError("Cannot translate function array section in %s\n",
                lib->filename.value);
    return false;
  }
  *runtime_start = runtime;
  *entry_count = (size_t)(found->size / size);
  *entry_size = size;
  return true;
}

bool LoaderGetImageFunctionArray(Loader* loader, LoadedDynamicLibrary* lib,
                                 LoaderLifecyclePhase phase,
                                 uint64_t* runtime_start, size_t* entry_count,
                                 size_t* entry_size) {
  if (loader == NULL || runtime_start == NULL || entry_count == NULL ||
      entry_size == NULL) {
    return false;
  }

  LoadedDynamicLibrary* main_image = LoaderMainImage(loader);
  if (lib == NULL) {
    lib = main_image;
  }

  if (phase == kLoaderLifecyclePreinit && !LoaderImageIsExecutable(loader, lib)) {
    return false;
  }

  if (lib != NULL && lib->dynamic != NULL &&
      GetFunctionArrayFromDynamicTags(loader, lib, phase, runtime_start,
                                      entry_count, entry_size)) {
    return true;
  }

  if (lib == main_image && main_image == NULL) {
    return GetFunctionArrayFromStaticSections(loader, NULL, phase, runtime_start,
                                              entry_count, entry_size);
  }

  if (LoaderImageIsExecutable(loader, lib)) {
    if (GetFunctionArrayFromStaticSections(loader, NULL, phase, runtime_start,
                                           entry_count, entry_size)) {
      return true;
    }
  }

  return GetFunctionArrayFromStaticSections(loader, lib, phase, runtime_start,
                                            entry_count, entry_size);
}

static LoaderLifecycleEntry* FindLifecycleEntry(LoaderLifecycleState* state,
                                                LoadedDynamicLibrary* lib) {
  for (size_t i = 0; i < state->init_order.length; i++) {
    LoaderLifecycleEntry* entry = state->init_order.value.p[i];
    if (entry->lib == lib) {
      return entry;
    }
  }
  return NULL;
}

static LoadedDynamicLibrary* FindNeededLibrary(
    DynamicLibraryRegistry* registry, const char* needed_name) {
  for (size_t i = 0; i < registry->search.length; i++) {
    LoadedDynamicLibrary* candidate = registry->search.value.p[i];
    const char* leaf = strrchr(candidate->filename.value, '/');
    leaf = leaf != NULL ? leaf + 1 : candidate->filename.value;
    if (strcmp(leaf, needed_name) == 0 ||
        strcmp(candidate->libname.value, needed_name) == 0 ||
        strcmp(candidate->filename.value, needed_name) == 0) {
      return candidate;
    }
  }
  return NULL;
}

static bool CollectNeededDependencies(LoadedDynamicLibrary* lib,
                                      Vector* dependencies) {
  if (lib == NULL || lib->dynamic == NULL) {
    return true;
  }
  const char* strtab =
      DynamicLoaderFindDynamicSectionAddressEntry(lib, DT(strtab));
  if (strtab == NULL) {
    LoaderError("Missing dynamic string table in %s\n", lib->filename.value);
    return false;
  }
  const DynamicSection* section = lib->dynamic;
  for (size_t i = 0; section->entries[i].tag != DT(null); i++) {
    if (section->entries[i].tag != DT(needed)) {
      continue;
    }
    const char* needed_name = strtab + section->entries[i].un.val;
    LoadedDynamicLibrary* dep =
        FindNeededLibrary(&lib->loader->loaded_libraries, needed_name);
    if (dep == NULL) {
      LoaderError("Missing loaded dependency %s needed by %s\n", needed_name,
                  lib->filename.value);
      return false;
    }
    VectorAppend(dependencies, dep);
  }
  return true;
}

typedef struct LifecycleSortContext {
  Loader* loader;
  LoaderLifecycleState* state;
  Vector visiting;
} LifecycleSortContext;

static bool LifecycleVisitForSort(LifecycleSortContext* ctx,
                                  LoadedDynamicLibrary* lib) {
  for (size_t i = 0; i < ctx->visiting.length; i++) {
    if (ctx->visiting.value.p[i] == lib) {
      ctx->state->cycle_detected = true;
      LoaderError("Cycle detected in DSO dependency graph at %s\n",
                  lib->filename.value);
      return false;
    }
  }
  LoaderLifecycleEntry* entry = FindLifecycleEntry(ctx->state, lib);
  if (entry != NULL) {
    return true;
  }

  VectorAppend(&ctx->visiting, lib);

  Vector dependencies;
  VectorInit(&dependencies);
  if (!CollectNeededDependencies(lib, &dependencies)) {
    VectorDestruct(&dependencies);
    ctx->visiting.length--;
    return false;
  }
  for (size_t i = 0; i < dependencies.length; i++) {
    LoadedDynamicLibrary* dep = dependencies.value.p[i];
    if (dep->header != NULL && dep->header->type == ET(dyn) &&
        !LifecycleVisitForSort(ctx, dep)) {
      VectorDestruct(&dependencies);
      ctx->visiting.length--;
      return false;
    }
  }
  VectorDestruct(&dependencies);

  entry = calloc(1, sizeof(LoaderLifecycleEntry));
  if (entry == NULL) {
    LoaderError("Failed to allocate lifecycle state for %s\n",
                lib->filename.value);
    ctx->visiting.length--;
    return false;
  }
  entry->lib = lib;
  entry->is_executable = LoaderImageIsExecutable(ctx->loader, lib);
  VectorAppend(&ctx->state->init_order, entry);
  ctx->visiting.length--;
  return true;
}

bool LoaderLifecycleStateInit(Loader* loader, LoaderLifecycleState* state) {
  if (loader == NULL || state == NULL) {
    return false;
  }
  if (state->init_order.length != 0) {
    return !state->cycle_detected;
  }
  VectorInit(&state->init_order);
  VectorInit(&state->successful_init);
  state->cycle_detected = false;
  state->executable_guest_fini_done = false;

  LoadedDynamicLibrary* main_image = LoaderMainImage(loader);
  if (loader->is_static || main_image == NULL) {
    LoaderLifecycleEntry* entry = calloc(1, sizeof(LoaderLifecycleEntry));
    if (entry == NULL) {
      LoaderError("Failed to allocate executable lifecycle state\n");
      return false;
    }
    entry->lib = NULL;
    entry->is_executable = true;
    VectorAppend(&state->init_order, entry);
    return true;
  }

  LifecycleSortContext ctx = {
      .loader = loader,
      .state = state,
  };
  VectorInit(&ctx.visiting);

  for (size_t i = 0; i < loader->loaded_libraries.search.length; i++) {
    LoadedDynamicLibrary* lib = loader->loaded_libraries.search.value.p[i];
    if (lib->header == NULL || lib->header->type != ET(dyn)) {
      continue;
    }
    if (!LifecycleVisitForSort(&ctx, lib)) {
      VectorDestruct(&ctx.visiting);
      return false;
    }
  }
  VectorDestruct(&ctx.visiting);

  LoaderLifecycleEntry* main_entry = FindLifecycleEntry(state, main_image);
  if (main_entry == NULL) {
    main_entry = calloc(1, sizeof(LoaderLifecycleEntry));
    if (main_entry == NULL) {
      LoaderError("Failed to allocate executable lifecycle state\n");
      return false;
    }
    main_entry->lib = main_image;
    main_entry->is_executable = true;
    VectorAppend(&state->init_order, main_entry);
  } else {
    main_entry->is_executable = true;
  }
  return !state->cycle_detected;
}

void LoaderLifecycleStateDestruct(LoaderLifecycleState* state) {
  if (state == NULL) {
    return;
  }
  VectorDestructWithContents(&state->init_order, NULL, /*free_element=*/true);
  VectorDestruct(&state->successful_init);
  state->cycle_detected = false;
  state->executable_guest_fini_done = false;
}

static bool LifecycleEntryPhaseDone(LoaderLifecycleEntry* entry,
                                  LoaderLifecyclePhase phase) {
  switch (phase) {
    case kLoaderLifecyclePreinit:
      return entry->preinit_done;
    case kLoaderLifecycleInit:
      return entry->init_done;
    case kLoaderLifecycleFini:
      return entry->fini_done;
  }
  return true;
}

static void LifecycleEntryMarkPhaseDone(LoaderLifecycleEntry* entry,
                                        LoaderLifecyclePhase phase) {
  switch (phase) {
    case kLoaderLifecyclePreinit:
      entry->preinit_done = true;
      break;
    case kLoaderLifecycleInit:
      entry->init_done = true;
      break;
    case kLoaderLifecycleFini:
      entry->fini_done = true;
      break;
  }
}

bool LoaderLifecycleVisitImageFunctions(Loader* loader,
                                        LoaderLifecycleState* state,
                                        LoadedDynamicLibrary* lib,
                                        LoaderLifecyclePhase phase,
                                        LoaderLifecycleFunctionFn callback,
                                        void* context) {
  if (loader == NULL || state == NULL || callback == NULL) {
    return false;
  }

  LoaderLifecycleEntry* entry = FindLifecycleEntry(state, lib);
  if (entry == NULL) {
    return false;
  }
  if (LifecycleEntryPhaseDone(entry, phase)) {
    return true;
  }
  if (phase == kLoaderLifecycleFini && !entry->init_done) {
    return true;
  }

  uint64_t start = 0;
  size_t count = 0;
  size_t entry_size = 0;
  if (!LoaderGetImageFunctionArray(loader, lib, phase, &start, &count,
                                   &entry_size)) {
    if (LoaderNumErrors() != 0) {
      return false;
    }
    LifecycleEntryMarkPhaseDone(entry, phase);
    if (phase == kLoaderLifecycleInit) {
      VectorAppend(&state->successful_init, entry);
    }
    return true;
  }

  bool reverse = PhaseUsesReverseIteration(phase);
  for (size_t i = 0; i < count; i++) {
    size_t index = reverse ? count - i - 1 : i;
    uint64_t function = 0;
    if (!LoaderReadFunctionArrayEntry(start, index, entry_size, &function)) {
      return false;
    }
    if (function == 0) {
      continue;
    }
    if (!callback(context, lib, function, phase)) {
      return false;
    }
  }

  LifecycleEntryMarkPhaseDone(entry, phase);
  if (phase == kLoaderLifecycleInit) {
    VectorAppend(&state->successful_init, entry);
  }
  return true;
}

static LoaderLifecycleEntry* LoaderExecutableEntry(
    LoaderLifecycleState* state) {
  for (size_t i = 0; i < state->init_order.length; i++) {
    LoaderLifecycleEntry* entry = state->init_order.value.p[i];
    if (entry->is_executable) {
      return entry;
    }
  }
  return NULL;
}

bool LoaderLifecycleExecutableFiniAlreadyDone(
    const LoaderLifecycleState* state) {
  return state != NULL && state->executable_guest_fini_done;
}

void LoaderLifecycleMarkExecutableFiniComplete(Loader* loader,
                                             LoaderLifecycleState* state) {
  if (loader == NULL) {
    return;
  }
  if (state == NULL) {
    state = loader->lifecycle;
  }
  if (state == NULL) {
    return;
  }
  LoaderLifecycleEntry* entry = LoaderExecutableEntry(state);
  if (entry != NULL) {
    entry->fini_done = true;
  }
  state->executable_guest_fini_done = true;
}

bool LoaderLifecycleRunPhase(Loader* loader, LoaderLifecycleState* state,
                             LoaderLifecyclePhase phase,
                             LoaderLifecycleFunctionFn callback,
                             void* context) {
  if (loader == NULL || callback == NULL) {
    return false;
  }
  if (state == NULL) {
    state = loader->lifecycle;
  }
  if (state == NULL) {
    return false;
  }
  if (state->init_order.length == 0) {
    if (!LoaderLifecycleStateInit(loader, state)) {
      return false;
    }
  }
  if (state->cycle_detected) {
    return false;
  }

  if (phase == kLoaderLifecycleFini) {
    for (size_t i = state->successful_init.length; i > 0; i--) {
      LoaderLifecycleEntry* entry = state->successful_init.value.p[i - 1];
      if (!LoaderLifecycleVisitImageFunctions(loader, state, entry->lib, phase,
                                              callback, context)) {
        return false;
      }
    }
    return true;
  }

  for (size_t i = 0; i < state->init_order.length; i++) {
    LoaderLifecycleEntry* entry = state->init_order.value.p[i];
    if (phase == kLoaderLifecyclePreinit && !entry->is_executable) {
      continue;
    }
    if (!LoaderLifecycleVisitImageFunctions(loader, state, entry->lib, phase,
                                            callback, context)) {
      return false;
    }
  }
  return true;
}
