//
//  BitSet.c
//  c_compiler
//
//  Created by David Allison on 12/14/17.
//  Copyright © 2017 David Allison. All rights reserved.
//

#include "bitset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void BitSetInit(BitSet* set) {
  set->value = NULL;
  set->capacity = 0;
}

BitSet* NewBitSet() {
  BitSet* set = malloc(sizeof(BitSet));
  BitSetInit(set);
  return set;
}

void BitSetDestruct(BitSet* set) {
  free(set->value);
  set->value = NULL;
  set->capacity = 0;
}

void BitSetDelete(BitSet* set) {
  BitSetDestruct(set);
  free(set);
}

void BitSetClear(BitSet* set) {
  if (set->value == NULL) {
    return;
  }
  memset(set->value, 0, set->capacity * sizeof(uint64_t));
}

static int FindFirstSet64(uint64_t value) {
  if (value == 0) {
    return 0;
  }
  return __builtin_ctzll(value) + 1;
}

static size_t GrowthCapacity(size_t current, size_t required) {
  size_t capacity = current == 0 ? 1 : current;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      return required;
    }
    capacity *= 2;
  }
  return capacity;
}

// Make room for an index into the set.
static void MakeRoomFor(BitSet* set, size_t index) {
  size_t words_required = (index + 64) / 64;
  size_t new_capacity = GrowthCapacity(set->capacity, words_required);
  if (set->value == NULL) {
    set->value = calloc(new_capacity, sizeof(uint64_t));
    set->capacity = new_capacity;
  } else if (words_required > set->capacity) {
    set->value = realloc(set->value, new_capacity * sizeof(uint64_t));
    // Clear new memory.
    memset(&set->value[set->capacity], 0,
           (new_capacity - set->capacity) * sizeof(uint64_t));
    set->capacity = new_capacity;
  }
}

// Make room for a number of words.
static void MakeRoom(BitSet* set, size_t words) {
  if (words == 0) {
    return;
  }
  size_t new_capacity = GrowthCapacity(set->capacity, words);
  if (set->value == NULL) {
    set->value = calloc(new_capacity, sizeof(uint64_t));
    set->capacity = new_capacity;
  } else if (words > set->capacity) {
    size_t old_capacity = set->capacity;
    set->value = realloc(set->value, new_capacity * sizeof(uint64_t));
    memset(&set->value[old_capacity], 0,
           (new_capacity - old_capacity) * sizeof(uint64_t));
    set->capacity = new_capacity;
  }
}

void BitSetFill(BitSet* set, size_t bit_count) {
  size_t words = (bit_count + 63) / 64;
  if (words != 0) {
    MakeRoom(set, words);
    memset(set->value, 0xff, words * sizeof(uint64_t));
    size_t final_word_bits = bit_count % 64;
    if (final_word_bits != 0) {
      set->value[words - 1] =
          (UINT64_C(1) << final_word_bits) - UINT64_C(1);
    }
  }
  if (set->capacity > words) {
    memset(&set->value[words], 0,
           (set->capacity - words) * sizeof(uint64_t));
  }
}

void BitSetInsertGrow(BitSet* set, size_t index) {
  size_t word = index / 64;
  MakeRoomFor(set, index);
  set->value[word] |= UINT64_C(1) << (index % 64);
}

bool BitSetContains(BitSet* set, size_t index) {
  size_t word = index / 64;
  if (word >= set->capacity) {
    return false;
  }
  return (set->value[word] & (UINT64_C(1) << (index % 64))) != 0;
}

size_t BitSetFindFirstSet(BitSet* set) {
  for (size_t word = 0; word < set->capacity; word++) {
    uint64_t w = set->value[word];
    int index = FindFirstSet64(w);
    if (index != 0) {
      return word * 64 + index - 1;
    }
  }
  return (size_t)-1;
}

size_t BitSetFindFirstClear(BitSet* set) {
  for (size_t word = 0; word < set->capacity; word++) {
    uint64_t w = ~set->value[word];
    int index = FindFirstSet64(w);
    if (index != 0) {
      return word * 64 + index - 1;
    }
  }
  return (size_t)-1;
}

void BitSetRemove(BitSet* set, size_t index) {
  size_t word = index / 64;
  if (word >= set->capacity) {
    return;
  }
  set->value[word] &= ~(UINT64_C(1) << (index % 64));
}

void BitSetIntersection(BitSet* set1, BitSet* set2, BitSet* result) {
  size_t min = set1->capacity;
  if (min > set2->capacity) {
    min = set2->capacity;
  }
  // If the min size is zero then the result is an empty set.
  if (min == 0) {
    BitSetClear(result);
    return;
  }
  MakeRoom(result, min);
  for (size_t i = 0; i < min; i++) {
    result->value[i] = set1->value[i] & set2->value[i];
  }
  if (result->capacity > min) {
    memset(&result->value[min], 0,
           (result->capacity - min) * sizeof(uint64_t));
  }
}

void BitSetUnion(BitSet* set1, BitSet* set2, BitSet* result) {
  size_t max = set1->capacity;
  if (max < set2->capacity) {
    max = set2->capacity;
  }
  MakeRoom(result, max);
  for (size_t i = 0; i < max; i++) {
    uint64_t left = i < set1->capacity ? set1->value[i] : 0;
    uint64_t right = i < set2->capacity ? set2->value[i] : 0;
    result->value[i] = left | right;
  }
  if (result->capacity > max) {
    memset(&result->value[max], 0,
           (result->capacity - max) * sizeof(uint64_t));
  }
}

void BitSetUnionInPlace(BitSet* dest, BitSet* src) {
  size_t max = dest->capacity;
  if (max < src->capacity) {
    max = src->capacity;
  }
  MakeRoom(dest, max);
  for (size_t i = 0; i < src->capacity; i++) {
    dest->value[i] |= src->value[i];
  }
}

void BitSetCopy(BitSet* to, BitSet* from) {
  MakeRoom(to, from->capacity);
  if (from->capacity > 0) {
    memcpy(to->value, from->value, from->capacity * sizeof(uint64_t));
  }
  if (to->capacity > from->capacity) {
    memset(&to->value[from->capacity], 0,
           (to->capacity - from->capacity) * sizeof(uint64_t));
  }
}

bool BitSetEqual(BitSet* set1, BitSet* set2) {
  BitSet* larger = set2;
  size_t min = set1->capacity;
  if (min > set2->capacity) {
    min = set2->capacity;
    larger = set1;
  }

  // First compare area where the capacities match.  One may have a larger
  // capacity but all the extra bits might be zero.
  int v = memcmp(set1->value, set2->value, min * sizeof(uint64_t));
  if (v != 0) {
    return false;
  }
  // Now we look at the extra bits in the larger set to see if they
  // are all zero.
  for (size_t i = min; i < larger->capacity; i++) {
    if (larger->value[i] != 0) {
      return false;
    }
  }
  return true;
}

void BitSetExpand(BitSet* set, Vector* vec) {
  size_t index = 0;
  for (size_t word = 0; word < set->capacity; word++) {
    for (size_t bit = 0; bit < 64; bit++) {
      if ((set->value[word] & (UINT64_C(1) << bit)) != 0) {
        VectorAppend(vec, (void*)index);
      }
      index++;
    }
  }
}

size_t BitSetCount(BitSet* set) {
  size_t count = 0;
  for (size_t word = 0; word < set->capacity; word++) {
    count += (size_t)__builtin_popcountll(set->value[word]);
  }
  return count;
}

void BitSetPrint(BitSet* set, FILE* fp) {
  fprintf(fp, "{");
  const char* sep = "";
  size_t index = 0;
  for (size_t word = 0; word < set->capacity; word++) {
    for (size_t bit = 0; bit < 64; bit++) {
      if ((set->value[word] & (UINT64_C(1) << bit)) != 0) {
        fprintf(fp, "%s%zd", sep, index);
        sep = ", ";
      }
      index++;
    }
  }
  fprintf(fp, "}");
}

void BitSetIteratorStart(BitSetIterator* it, BitSet* set) {
  it->set = set;
  it->word_offset = 0;
  it->bit_offset = 0;
  it->remaining = 0;
  while (it->word_offset < set->capacity &&
         set->value[it->word_offset] == 0) {
    it->word_offset++;
  }
  if (it->word_offset < set->capacity) {
    it->remaining = set->value[it->word_offset];
    it->bit_offset = (size_t)__builtin_ctzll(it->remaining);
  }
}

bool BitSetIteratorDone(BitSetIterator* it);

void BitSetIteratorNext(BitSetIterator* it) {
  if (BitSetIteratorDone(it)) {
    return;
  }
  it->remaining &= it->remaining - 1;
  if (it->remaining != 0) {
    it->bit_offset = (size_t)__builtin_ctzll(it->remaining);
    return;
  }
  do {
    it->word_offset++;
    if (it->word_offset < it->set->capacity &&
        it->set->value[it->word_offset] != 0) {
      it->remaining = it->set->value[it->word_offset];
      it->bit_offset = (size_t)__builtin_ctzll(it->remaining);
      return;
    }
  } while (it->word_offset < it->set->capacity);
  it->bit_offset = 0;
  it->remaining = 0;
}

size_t BitSetIteratorValue(BitSetIterator* it);
