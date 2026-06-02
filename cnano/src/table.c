#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"

// Grow when the table is 75% full. Keeping a hash table below ~75% load is the
// classic rule of thumb: emptier tables have shorter probe sequences (faster
// lookups) but waste memory; 0.75 is a good balance used by many libraries.
#define TABLE_MAX_LOAD 0.75

void initTable(Table *table) {
  table->count = 0;
  table->capacity = 0;
  table->entries = NULL;
}

void freeTable(Table *table) {
  free(table->entries);
  initTable(table);
}

// The core probing routine, shared by get/set/delete. Given a bucket array and a
// key, return the bucket where the key lives OR the bucket where it should be
// inserted. Two kinds of "empty" exist here and the distinction is the whole
// trick of deletion in an open-addressed table:
//
//   * a truly empty bucket:  key == NULL and value is NIL
//   * a TOMBSTONE:           key == NULL and value is BOOL(true)
//
// A tombstone marks "something was deleted here". Probing must treat a tombstone
// as OCCUPIED (keep walking past it on lookup) — otherwise a delete in the
// middle of a probe sequence would orphan every key after it. But insertion may
// REUSE a tombstone. We remember the first tombstone we pass so we can fill it.
static Entry *findEntry(Entry *entries, int capacity, ObjString *key) {
  // capacity is a power of two, so `& (capacity-1)` is a fast modulo.
  uint32_t index = key->hash & (capacity - 1);
  Entry *tombstone = NULL;

  for (;;) {
    Entry *entry = &entries[index];
    if (entry->key == NULL) {
      if (IS_NIL(entry->value)) {
        // Empty bucket: the key isn't here. Prefer a tombstone we passed so
        // deleted slots get reclaimed instead of growing the table forever.
        return tombstone != NULL ? tombstone : entry;
      } else {
        // A tombstone. Note the first one and keep probing.
        if (tombstone == NULL)
          tombstone = entry;
      }
    } else if (entry->key == key) {
      // Found it. Keys are interned (see object.c), so identical strings are the
      // SAME pointer — we can compare with == instead of strcmp. That pointer
      // equality is the entire payoff of interning and makes lookups very fast.
      return entry;
    }
    index = (index + 1) & (capacity - 1); // linear probe, wrapping around
  }
}

bool tableGet(Table *table, ObjString *key, Value *value) {
  if (table->count == 0)
    return false;
  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL)
    return false;
  *value = entry->value;
  return true;
}

int tableFindIndex(Table *table, ObjString *key) {
  if (table->count == 0)
    return -1;
  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL)
    return -1;
  return (int)(entry - table->entries); // position within the bucket array
}

// Allocate a bigger bucket array and RE-INSERT every live entry. We cannot just
// realloc-and-copy: an entry's home bucket depends on `capacity`, so every key
// must be re-hashed into the new array. Re-inserting also drops tombstones,
// which is why we recompute `count` from scratch here.
static void adjustCapacity(Table *table, int capacity) {
  Entry *entries = malloc(sizeof(Entry) * capacity);
  if (entries == NULL) {
    fprintf(stderr, "cnano: out of memory growing hash table\n");
    exit(70);
  }
  for (int i = 0; i < capacity; i++) {
    entries[i].key = NULL;
    entries[i].value = NIL_VAL;
  }

  table->count = 0;
  for (int i = 0; i < table->capacity; i++) {
    Entry *src = &table->entries[i];
    if (src->key == NULL)
      continue; // skip empties AND tombstones — tombstones don't survive a grow
    Entry *dest = findEntry(entries, capacity, src->key);
    dest->key = src->key;
    dest->value = src->value;
    table->count++;
  }

  free(table->entries);
  table->entries = entries;
  table->capacity = capacity;
}

bool tableSet(Table *table, ObjString *key, Value value) {
  // Grow before we exceed the load factor — but ONLY when this set will actually
  // INSERT a new key (count rises). Updating an existing key must never grow,
  // because a resize moves every entry to a new bucket index, and the global
  // inline cache (vm.c) caches an entry INDEX that is only re-resolved when the
  // generation bumps — and the generation bumps on global DEFINE, not on a plain
  // assignment. Growing on an update would silently invalidate those caches.
  if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
    bool willInsert = true; // (also true when there is no storage yet)
    if (table->capacity > 0) {
      Entry *existing = findEntry(table->entries, table->capacity, key);
      willInsert = existing->key == NULL && IS_NIL(existing->value); // empty bucket
    }
    if (willInsert) {
      int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
      adjustCapacity(table, capacity);
    }
  }

  Entry *entry = findEntry(table->entries, table->capacity, key);
  bool isNewKey = entry->key == NULL;
  // Only bump count for a brand-new key landing in a truly empty bucket. Reusing
  // a tombstone (key NULL but value not NIL) must NOT increment count, since the
  // tombstone was already counted when it was created.
  if (isNewKey && IS_NIL(entry->value))
    table->count++;

  entry->key = key;
  entry->value = value;
  return isNewKey;
}

bool tableDelete(Table *table, ObjString *key) {
  if (table->count == 0)
    return false;
  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL)
    return false;
  // Leave a tombstone: key NULL, value BOOL(true). count is intentionally NOT
  // decremented — the tombstone still occupies the probe sequence.
  entry->key = NULL;
  entry->value = BOOL_VAL(true);
  return true;
}

// Find a string by content rather than by pointer. This is the ONE place that
// compares characters (everywhere else uses interned-pointer equality). It is
// how copyString checks "do we already have this exact string?" so it can return
// the existing object and keep the intern invariant.
ObjString *tableFindString(Table *table, const char *chars, int length,
                           uint32_t hash) {
  if (table->count == 0)
    return NULL;
  uint32_t index = hash & (table->capacity - 1);
  for (;;) {
    Entry *entry = &table->entries[index];
    if (entry->key == NULL) {
      // Stop at a truly empty (non-tombstone) bucket: the string isn't here.
      if (IS_NIL(entry->value))
        return NULL;
    } else if (entry->key->length == length && entry->key->hash == hash &&
               memcmp(entry->key->chars, chars, length) == 0) {
      return entry->key; // found an identical existing string
    }
    index = (index + 1) & (table->capacity - 1);
  }
}

// --- garbage-collector support ---------------------------------------------

void markTable(Table *table) {
  // The globals table is a GC root: both its keys (ObjString names) and its
  // values must survive a collection. Walk every bucket; markObject/markValue
  // ignore empty buckets (NULL key) and non-object values.
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    markObject((Obj *)entry->key);
    markValue(entry->value);
  }
}

void tableRemoveWhite(Table *table) {
  // The string intern pool is a WEAK table: it must not keep a string alive by
  // itself. After tracing, any key still unmarked is unreachable and about to be
  // freed, so delete its entry here — otherwise the table would be left pointing
  // at freed memory (a classic dangling-weak-reference bug).
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (entry->key != NULL && !entry->key->obj.isMarked)
      tableDelete(table, entry->key);
  }
}
