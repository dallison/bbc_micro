//
//  loader_lifecycle.h
//  c_compiler
//
//  Central ELF init/fini array visitation for the main executable and loaded
//  DSOs.  Handles dependency-order initialization, reverse finalization, DT tag
//  and static-section lookup, and per-image idempotency.
//

#ifndef loader_lifecycle_h
#define loader_lifecycle_h

#include "loader.h"

typedef enum {
  kLoaderLifecyclePreinit = 0,
  kLoaderLifecycleInit = 1,
  kLoaderLifecycleFini = 2,
} LoaderLifecyclePhase;

typedef struct LoaderLifecycleEntry {
  LoadedDynamicLibrary* lib;
  bool is_executable;
  bool preinit_done;
  bool init_done;
  bool fini_done;
} LoaderLifecycleEntry;

typedef struct LoaderLifecycleState {
  Vector init_order;
  Vector successful_init;
  bool cycle_detected;
  bool executable_guest_fini_done;
} LoaderLifecycleState;

typedef bool (*LoaderLifecycleFunctionFn)(void* context,
                                          LoadedDynamicLibrary* image,
                                          uint64_t function_runtime,
                                          LoaderLifecyclePhase phase);

bool LoaderLifecycleStateInit(Loader* loader, LoaderLifecycleState* state);
void LoaderLifecycleStateDestruct(LoaderLifecycleState* state);

bool LoaderGetImageFunctionArray(Loader* loader, LoadedDynamicLibrary* lib,
                                 LoaderLifecyclePhase phase,
                                 uint64_t* runtime_start, size_t* entry_count,
                                 size_t* entry_size);

bool LoaderLifecycleVisitImageFunctions(Loader* loader,
                                        LoaderLifecycleState* state,
                                        LoadedDynamicLibrary* lib,
                                        LoaderLifecyclePhase phase,
                                        LoaderLifecycleFunctionFn callback,
                                        void* context);

bool LoaderLifecycleRunPhase(Loader* loader, LoaderLifecycleState* state,
                             LoaderLifecyclePhase phase,
                             LoaderLifecycleFunctionFn callback,
                             void* context);

bool LoaderLifecycleExecutableFiniAlreadyDone(
    const LoaderLifecycleState* state);

void LoaderLifecycleMarkExecutableFiniComplete(Loader* loader,
                                             LoaderLifecycleState* state);

#endif /* loader_lifecycle_h */
