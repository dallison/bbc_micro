//
//  set.c
//  c_compiler
//
//  Created by David Allison on 12/13/17.
//  Copyright © 2017 David Allison. All rights reserved.
//

#include "set.h"
#include <stdlib.h>

void SetInit(Set* set, SetCompareFunc func) {
  VectorInit(&set->vec);
  set->compare = func;
}

Set* NewSet(SetCompareFunc func) {
  Set* set = malloc(sizeof(Set));
  SetInit(set, func);
  return set;
}

void SetDestruct(Set* set) { VectorDestruct(&set->vec); }

void SetDelete(Set* set) {
  SetDestruct(set);
  free(set);
}

void SetClear(Set* set) { VectorClear(&set->vec); }

void SetDestructWithContents(Set* set, SetElementDestructor destructor, bool free_contents) {
  VectorDestructWithContents(&set->vec, (VectorElementDestructor)destructor, free_contents);
}

void SetDeleteWithContents(Set* set, SetElementDestructor destructor, bool free_contents) {
  VectorDestructWithContents(&set->vec, (VectorElementDestructor)destructor, free_contents);
  free(set);
}

void SetClearWithContents(Set* set, SetElementDestructor destructor, bool free_contents) {
  VectorClearWithContents(&set->vec, (VectorElementDestructor)destructor, free_contents);
}

static int ComparePointers(const void* a, const void* b) {
  // Order by address.  The difference of two pointers does not survive being
  // truncated to `int`: elements more than 2GB apart would compare equal or in
  // the wrong order, breaking the set's binary search.
  uintptr_t v1 = (uintptr_t)*(const void**)a;
  uintptr_t v2 = (uintptr_t)*(const void**)b;
  if (v1 == v2) {
    return 0;
  }
  return v1 < v2 ? -1 : 1;
}

static int CompareIntegers(const void* a, const void* b) {
  int v1 = *(int*)a;
  int v2 = *(int*)b;
  return v1 - v2;
}

void SetInitForPointers(Set* set) {
  SetInit(set, ComparePointers);
}

void SetInitForIntegers(Set* set) {
  SetInit(set, CompareIntegers);
}

// Find the location in the set at which we should insert before
// to keep the vector sorted.  This uses a binary search, returning
// NULL if there is no location (location is beyond end of set).  Also
// sets *found to true if the value is found in the set.
static void** FindLocation(Set* set, void* value, bool* found) {
  *found = false;
  size_t low = 0;
  size_t high = set->vec.length;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    int compval = set->compare(&value, &set->vec.value.p[mid]);
    if (compval == 0) {
      // Exact match found.
      *found = true;
      return &set->vec.value.p[mid];
    }
    if (compval < 0) {
      // In first half.
      high = mid;
    } else {
      // In second half.
      low = mid + 1;
    }
  }
  if (high == set->vec.length) {
    return NULL;
  }
  return &set->vec.value.p[high];
}

// Insert into the vector keeping it sorted and omitting duplicates.
// O(n) for insertion
static void LinearInsert(Set* set, void* value) {
  for (size_t i = 0; i < set->vec.length; i++) {
    int compval = set->compare(&value, &set->vec.value.p[i]);
    if (compval == 0) {
      // Matches existing value, do not insert.
      return;
    }
    if (compval < 0) {
      // value < vec[i]
      VectorInsertBefore(&set->vec, i, value);
      return;
    }
  }
  VectorAppend(&set->vec, value);
}

// Insert into the set using a binary insertion.  This will be
// O(log2).
static void BinaryInsert(Set* set, void* value) {
  bool found;
  void** p = FindLocation(set, value, &found);
  if (p == NULL) {
    VectorAppend(&set->vec, value);
    return;
  }
  if (found) {
    return;
  }
  // Insert before 'p'.
  VectorInsertBefore(&set->vec, p - set->vec.value.p, value);
}

// Insert the value into the set.
void SetInsert(Set* set, void* value) {
  if (set->vec.length < 5) {
    LinearInsert(set, value);
  } else {
    BinaryInsert(set, value);
  }
}

// Remove an element given its value
// O(log(n)) for removal.
void SetRemove(Set* set, void* value) {
  bool found;
  void** p = FindLocation(set, value, &found);
  if (p == NULL || !found) {
    return;
  }
  VectorDeleteElement(&set->vec, p - set->vec.value.p);
}

// Find an element given a value, return true if found.
// O(ln2(n))
bool SetContains(Set* set, void* value) {
  return bsearch(&value, set->vec.value.p,
                 set->vec.length,
                 sizeof(void*),
                 set->compare) != NULL;
}

void SetIntersection(Set* set1, Set* set2, Set* result) {
  size_t i = 0;
  size_t j = 0;
  while (i < set1->vec.length && j < set2->vec.length) {
    int compval = set1->compare(&set1->vec.value.p[i], &set2->vec.value.p[j]);
    if (compval == 0) {
      SetInsert(result, set1->vec.value.p[i]);
      i++;
      j++;
    } else if (compval < 0) {
      i++;
    } else {
      j++;
    }
  }
}

void SetUnion(Set* set1, Set* set2, Set* result) {
  for (size_t i = 0; i < set1->vec.length; i++) {
    SetInsert(result, set1->vec.value.p[i]);
  }
  for (size_t i = 0; i < set2->vec.length; i++) {
    SetInsert(result, set2->vec.value.p[i]);
  }
}

void SetCopy(Set* to, Set* from) { VectorCopy(&to->vec, &from->vec); }

bool SetEqual(Set* set1, Set* set2) {
  return VectorEqual(&set1->vec, &set2->vec);
}
