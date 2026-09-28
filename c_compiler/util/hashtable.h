//
//  hashtable.h
//  c_compiler
//
//  Created by David Allison on 11/19/17.
//  Copyright © 2017 David Allison. All rights reserved.
//

#ifndef hashtable_h
#define hashtable_h

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "dstring.h"

//
// This a general purpose hash table.  It consists of a fixed (at init time)
// sized array of pointers.  The user provides three functions:
//
// 1. A function to create an integer from a value passed.  The value passed
//    depends on whether we are searching or inserting.  This allows the
//    insertion to have a different value from the search.
// 2. A function to insert a value into the table at a certain index.
//    This is passed three parameters:
//      1. The value of the entry at the index we want to insert into.
//      2. The value being inserted.
//      3. The address of the entry at the index we are inserting into.
//    The idea is that if the entry contains a data structure the inserter
//    is passed all the information is needs to perform an insertion into that
//    data structure.
// 3. A function to search a hash table entry.  This is passed 2 parameters:
//      1. The value of the entry to search.
//      2. The value to search for.
//    If the entry contains a data structure, this function is passed a pointer
//    to the start of this data structure and the value to find.

// Mode passed to the hasher function.  Can be used by the user-provided
// function to determine the type of the value.
typedef enum {
  kHashSearch,
  kHashInsert,
} HashMode;

struct HashTable;

// Hash table helper functions, provided by the user and used to
// perform the work of the hash table.
typedef size_t (*HashTableHasher)(void* value, struct HashTable* table,
                                  HashMode mode);
typedef bool (*HashTableInserter)(void* entry, void* value, void** parent);
typedef void* (*HashTableSearcher)(void* entry, void* value);

// The hash table itself.
typedef struct HashTable {
  String name;
  void** entries;            // The entries.
  size_t size;               // Size of the table (number of entries).
  size_t object_count;       // Number of object in the table.
  HashTableHasher Hash;      // Function to create hash value.
  HashTableInserter Insert;  // Function to insert into entry.
  HashTableSearcher Search;  // Function to search an entry.
} HashTable;

// Initializes a hash table.
void HashTableInit(HashTable* table, const char* name, size_t size,
                   HashTableHasher hash, HashTableInserter insert,
                   HashTableSearcher search);

// Creates and initializes a new hash table from the heap.
HashTable* NewHashTable(size_t size, const char* name, HashTableHasher hash,
                        HashTableInserter insert, HashTableSearcher search);

// Destruct a hash table.
void HashTableDestruct(HashTable* table);

// Delete a hash table, freeing the memory.
void HashTableDelete(HashTable* table);

// Clear the table but don't delete the entries.
void HashTableClear(HashTable* table);

// Insert the given value into the given table. Returns true if the
// insertion was successful.
bool HashTableInsert(HashTable* table, void* value);

// Search the table for the given value and return the associated inserted
// value.
void* HashTableSearch(HashTable* table, void* value);

// Traverse the hash table and for each entry call the function with
// the entry as a parameter.  The 'data' is passed through to the function.
void HashTableTraverse(HashTable* table, void (*func)(void* entry, void* data),
                       void* data);

void HashTableCopy(HashTable* to, HashTable* from,
                   void* (*copy_func)(void* entry));

void HashTablePrintStats(HashTable* table, FILE* fp);

#endif /* hashtable_h */
