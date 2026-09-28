//
//  hashtable.c
//  c_compiler
//
//  Created by David Allison on 11/19/17.
//  Copyright © 2017 David Allison. All rights reserved.
//

#include "hashtable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void HashTableInit(HashTable* table, const char* name, size_t size,
                   HashTableHasher hash, HashTableInserter insert,
                   HashTableSearcher search) {
  StringInit(&table->name, name);
  table->size = size;
  table->object_count = 0;
  table->entries = calloc(sizeof(void*), size);
  table->Hash = hash;
  table->Insert = insert;
  table->Search = search;
}

HashTable* NewHashTable(size_t size, const char* name, HashTableHasher hash,
                        HashTableInserter insert, HashTableSearcher search) {
  HashTable* table = malloc(sizeof(HashTable));
  HashTableInit(table, name, size, hash, insert, search);
  return table;
}

void HashTableDestruct(HashTable* table) {
  StringDestruct(&table->name);
  free(table->entries);
  table->entries = NULL;
  table->size = 0;
}

void HashTableDelete(HashTable* table) {
  HashTableDestruct(table);
  free(table);
}

bool HashTableInsert(HashTable* table, void* value) {
  size_t hash_value = table->Hash(value, table, kHashInsert);
  hash_value %= table->size;
  bool ok = table->Insert(table->entries[hash_value], value,
                          &table->entries[hash_value]);
  if (ok) {
    table->object_count++;
  }
  return ok;
}

void* HashTableSearch(HashTable* table, void* value) {
  size_t hash_value = table->Hash(value, table, kHashSearch);
  hash_value %= table->size;
  return table->Search(table->entries[hash_value], value);
}

void HashTableTraverse(HashTable* table, void (*func)(void* entry, void* data),
                       void* data) {
  for (size_t i = 0; i < table->size; i++) {
    if (table->entries[i] != NULL) {
      func(table->entries[i], data);
    }
  }
}

void HashTableCopy(HashTable* to, HashTable* from,
                   void* (*copy_func)(void* entry)) {
  for (size_t i = 0; i < from->size; i++) {
    if (from->entries[i] != NULL) {
      to->entries[i] = copy_func(from->entries[i]);
      to->object_count++;
    }
  }
}

void HashTableClear(HashTable* table) {
  memset(table->entries, 0, sizeof(void*) * table->size);
}

void HashTablePrintStats(HashTable* table, FILE* fp) {
  fprintf(fp, "table %s: %zd objects\n", table->name.value, table->object_count);
}
