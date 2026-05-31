// table.h — a hash table mapping ObjString* keys to Values.
//
// This is the data-structure heart of step 3. It backs two things:
//   * the global-variable store (name -> value), and
//   * the string intern pool (used as a set: key present or not).
//
// Design: OPEN ADDRESSING with LINEAR PROBING. All entries live in one flat
// array. To find a key we hash it to a starting bucket and, if that bucket is
// taken by a different key, walk forward (wrapping around) until we find the key
// or an empty bucket. The alternative — separate chaining (a linked list per
// bucket) — is simpler to reason about but pointer-chases and allocates per
// entry; open addressing is cache-friendlier and is what most modern hash tables
// use. The cost is the subtlety below (tombstones).
#ifndef CNANO_TABLE_H
#define CNANO_TABLE_H

#include "common.h"
#include "value.h"

typedef struct ObjString ObjString; // forward decl; full type in object.h

typedef struct {
  ObjString *key; // NULL means this bucket has never held a live entry
  Value value;
} Entry;

typedef struct {
  int count;     // live entries PLUS tombstones (see table.c)
  int capacity;  // number of buckets (always a power of two)
  Entry *entries;
} Table;

void initTable(Table *table);
void freeTable(Table *table);

// Look up `key`. If found, copy its value into *value and return true.
bool tableGet(Table *table, ObjString *key, Value *value);

// Insert or overwrite `key` -> `value`. Returns true if a NEW key was added
// (false if it overwrote an existing key) — the compiler/VM use this to tell
// "define" from "assign".
bool tableSet(Table *table, ObjString *key, Value value);

// Remove `key`. Returns true if it was present. Leaves a tombstone behind.
bool tableDelete(Table *table, ObjString *key);

// Intern support: find an existing key by its raw characters+hash, or NULL.
// This is what makes string interning possible — see object.c.
ObjString *tableFindString(Table *table, const char *chars, int length,
                           uint32_t hash);

// GC support. markTable marks every key and value as reachable (used for the
// globals table, a GC root). tableRemoveWhite deletes entries whose key was NOT
// marked — used to prune the string intern pool, a WEAK table, of strings that
// are about to be swept (see memory.c / collectGarbage).
void markTable(Table *table);
void tableRemoveWhite(Table *table);

#endif // CNANO_TABLE_H
