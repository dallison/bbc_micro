//
//  set.h
//  c_compiler
//
//  Created by David Allison on 12/13/17.
//  Copyright © 2017 David Allison. All rights reserved.
//

#ifndef set_h
#define set_h

#include "vector.h"

// Function pointer type to destruct contents of set.
typedef void (*SetElementDestructor)(void*);

// A set is a vector of items, each of which is the same type
// and can be compared using a comparision function.  A set contains
// only unique items.  If you try to insert an item that will compare
// equal with one already in the set, the item will be replaced.
typedef int (*SetCompareFunc)(const void*, const void*);
typedef struct {
  Vector vec;              // Vector of items.
  SetCompareFunc compare;  // Comparison function.
} Set;

void SetInit(Set* set, SetCompareFunc func);
Set* NewSet(SetCompareFunc func);
void SetDestruct(Set* set);
void SetDelete(Set* set);
void SetClear(Set* set);

void SetDestructWithContents(Set* set, SetElementDestructor destructor, bool free_contents);
void SetDeleteWithContents(Set* set, SetElementDestructor destructor, bool free_contents);
void SetClearWithContents(Set* set, SetElementDestructor destructor, bool free_contents);

void SetInitForPointers(Set* set);
void SetInitForIntegers(Set* set);

void SetInsert(Set* set, void* value);
void SetRemove(Set* set, void* value);
bool SetContains(Set* set, void* value);

void SetIntersection(Set* set1, Set* set2, Set* result);
void SetUnion(Set* set1, Set* set2, Set* result);

void SetCopy(Set* to, Set* from);
bool SetEqual(Set* set1, Set* set2);

#endif /* set_h */
