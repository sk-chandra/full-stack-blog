#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "vm.h" // for the global object list head and the intern table

// Allocate a heap object of `size` bytes, tag it, and thread it onto the VM's
// intrusive object list. Every object is born through here — the single choke
// point the garbage collector hooks: allocation goes through reallocate (which
// may trigger a collection first) and each new object starts unmarked (white).
static Obj *allocateObject(size_t size, ObjType type) {
  Obj *object = (Obj *)reallocate(NULL, 0, size);
  object->type = type;
  object->isMarked = false;
  object->next = vm.objects; // push onto the front of the list
  vm.objects = object;
  return object;
}

// FNV-1a: a small, fast, well-distributed hash for short strings. Not
// cryptographic, but exactly right for a hash table. The constants are part of
// the published algorithm. We hash once, at string creation, and cache it.
static uint32_t hashString(const char *key, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619u;
  }
  return hash;
}

// Build an ObjString that takes OWNERSHIP of the heap buffer `chars` (already
// length+1 bytes, NUL-terminated). Registers it in the intern pool.
static ObjString *allocateString(char *chars, int length, uint32_t hash) {
  ObjString *string = (ObjString *)allocateObject(sizeof(ObjString), OBJ_STRING);
  string->length = length;
  string->chars = chars;
  string->hash = hash;
  // Add to the intern set (value is irrelevant; we use the table as a set).
  tableSet(&vm.strings, string, NIL_VAL);
  return string;
}

ObjString *copyString(const char *chars, int length) {
  uint32_t hash = hashString(chars, length);
  // INTERNING: if an identical string already exists, return that ONE object
  // instead of making a duplicate. The payoff is that any two equal strings are
  // the same pointer, so equality and hash-table lookups compare pointers, not
  // bytes. Variable names recur constantly, so this is a big win — and it's why
  // findEntry can use `entry->key == key`.
  ObjString *interned = tableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL)
    return interned;

  // Not seen before: copy the bytes into our own NUL-terminated buffer. We copy
  // because the source is often a transient slice of the program text. Routed
  // through reallocate so the GC accounts for the buffer too.
  char *heapChars = (char *)reallocate(NULL, 0, length + 1);
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';
  return allocateString(heapChars, length, hash);
}

ObjFunction *newFunction(void) {
  ObjFunction *function =
      (ObjFunction *)allocateObject(sizeof(ObjFunction), OBJ_FUNCTION);
  function->arity = 0;
  function->upvalueCount = 0;
  function->name = NULL;
  initChunk(&function->chunk); // each function owns a fresh, empty chunk
  return function;
}

ObjClosure *newClosure(ObjFunction *function) {
  // Allocate the upvalue pointer array first; the VM populates it right after.
  // Through reallocate so freeObject can subtract the same bytes on release.
  ObjUpvalue **upvalues = (ObjUpvalue **)reallocate(
      NULL, 0, sizeof(ObjUpvalue *) * function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++)
    upvalues[i] = NULL;

  ObjClosure *closure =
      (ObjClosure *)allocateObject(sizeof(ObjClosure), OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  return closure;
}

ObjNative *newNative(NativeFn fn, const char *name, int arity) {
  ObjNative *native = (ObjNative *)allocateObject(sizeof(ObjNative), OBJ_NATIVE);
  native->function = fn;
  native->name = name;
  native->arity = arity;
  return native;
}

ObjArray *newArrayObject(void) {
  ObjArray *array = (ObjArray *)allocateObject(sizeof(ObjArray), OBJ_ARRAY);
  initValueArray(&array->elements);
  return array;
}

// --- maps: a value-keyed hash table ----------------------------------------

#define MAP_MAX_LOAD 0.75

bool isHashableKey(Value key) {
  // Primitives are always hashable; among objects, only (interned) strings are.
  // Heap aggregates have no stable hash/equality we want to commit to as keys.
  return !IS_OBJ(key) || IS_STRING(key);
}

// Hash a hashable value. Strings reuse their cached FNV hash; integers get a
// 64->32 bit mix (SplitMix64's finaliser) so nearby keys scatter across buckets.
static uint32_t hashValue(Value key) {
  switch (key.type) {
  case VAL_NIL:
    return 0;
  case VAL_BOOL:
    return AS_BOOL(key) ? 1u : 2u;
  case VAL_INT: {
    uint64_t x = (uint64_t)AS_INT(key);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return (uint32_t)(x ^ (x >> 31));
  }
  case VAL_OBJ:
    return AS_STRING(key)->hash; // guaranteed a string by isHashableKey
  default:
    return 0;
  }
}

// Find the slot for `key`: the occupied entry holding it, or the slot where it
// would be inserted (preferring a tombstone passed on the way, so deletes get
// reclaimed). Tombstones don't stop the probe; an empty bucket does. Load < 1
// guarantees an empty bucket exists, so the probe always terminates.
static MapEntry *findMapEntry(MapEntry *entries, int capacity, Value key) {
  uint32_t index = hashValue(key) & (capacity - 1);
  MapEntry *tombstone = NULL;
  for (;;) {
    MapEntry *entry = &entries[index];
    if (entry->state == MAP_EMPTY)
      return tombstone != NULL ? tombstone : entry;
    if (entry->state == MAP_TOMBSTONE) {
      if (tombstone == NULL)
        tombstone = entry;
    } else if (valuesEqual(entry->key, key)) {
      return entry; // MAP_OCCUPIED with a matching key
    }
    index = (index + 1) & (capacity - 1);
  }
}

static void adjustMapCapacity(ObjMap *map, int capacity) {
  MapEntry *entries = malloc(sizeof(MapEntry) * capacity);
  if (entries == NULL) {
    fprintf(stderr, "cnano: out of memory growing a map\n");
    exit(70);
  }
  for (int i = 0; i < capacity; i++)
    entries[i].state = MAP_EMPTY;
  // Re-insert live entries only; tombstones are dropped by the rehash.
  for (int i = 0; i < map->capacity; i++) {
    MapEntry *src = &map->entries[i];
    if (src->state != MAP_OCCUPIED)
      continue;
    MapEntry *dest = findMapEntry(entries, capacity, src->key);
    dest->key = src->key;
    dest->value = src->value;
    dest->state = MAP_OCCUPIED;
  }
  free(map->entries);
  map->entries = entries;
  map->capacity = capacity;
  map->tombstones = 0;
}

bool mapGet(ObjMap *map, Value key, Value *out) {
  if (map->count == 0)
    return false;
  MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
  if (entry->state != MAP_OCCUPIED)
    return false;
  *out = entry->value;
  return true;
}

void mapSet(ObjMap *map, Value key, Value value) {
  // Grow when live + tombstones would exceed the load factor (tombstones count,
  // since they still lengthen probe sequences).
  if (map->count + map->tombstones + 1 > map->capacity * MAP_MAX_LOAD) {
    int capacity = map->capacity < 8 ? 8 : map->capacity * 2;
    adjustMapCapacity(map, capacity);
  }
  MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
  if (entry->state != MAP_OCCUPIED) {
    if (entry->state == MAP_TOMBSTONE)
      map->tombstones--; // reusing a tombstone
    map->count++;
  }
  entry->key = key;
  entry->value = value;
  entry->state = MAP_OCCUPIED;
}

bool mapDelete(ObjMap *map, Value key) {
  if (map->count == 0)
    return false;
  MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
  if (entry->state != MAP_OCCUPIED)
    return false;
  entry->state = MAP_TOMBSTONE; // keep the slot so probe chains stay intact
  entry->value = NIL_VAL;
  map->count--;
  map->tombstones++;
  return true;
}

ObjMap *newMapObject(void) {
  ObjMap *map = (ObjMap *)allocateObject(sizeof(ObjMap), OBJ_MAP);
  map->count = 0;
  map->tombstones = 0;
  map->capacity = 0;
  map->entries = NULL;
  return map;
}

ObjStruct *newStruct(ObjString *name, ObjString **fieldNames, int fieldCount) {
  ObjStruct *s = (ObjStruct *)allocateObject(sizeof(ObjStruct), OBJ_STRUCT);
  s->name = name;
  s->fieldNames = fieldNames;
  s->fieldCount = fieldCount;
  initTable(&s->methods);
  return s;
}

ObjInstance *newInstance(ObjStruct *type) {
  ObjInstance *instance =
      (ObjInstance *)allocateObject(sizeof(ObjInstance), OBJ_INSTANCE);
  instance->type = type;
  initTable(&instance->fields);
  return instance;
}

bool structHasField(ObjStruct *s, ObjString *name) {
  for (int i = 0; i < s->fieldCount; i++)
    if (s->fieldNames[i] == name) // interned: pointer comparison
      return true;
  return false;
}

ObjUpvalue *newUpvalue(Value *slot) {
  ObjUpvalue *upvalue =
      (ObjUpvalue *)allocateObject(sizeof(ObjUpvalue), OBJ_UPVALUE);
  upvalue->location = slot; // open: points into the stack
  upvalue->closed = NIL_VAL;
  upvalue->next = NULL;
  return upvalue;
}

void printObject(Value value) {
  switch (AS_OBJ(value)->type) {
  case OBJ_STRING:
    printf("%s", AS_CSTRING(value));
    break;
  case OBJ_FUNCTION: {
    ObjFunction *fn = AS_FUNCTION(value);
    if (fn->name == NULL)
      printf("<script>"); // the implicit top-level function
    else
      printf("<fn %s>", fn->name->chars);
    break;
  }
  case OBJ_CLOSURE: {
    // A closure prints like the function it wraps — the upvalues are an
    // implementation detail the user never sees.
    ObjFunction *fn = AS_CLOSURE(value)->function;
    if (fn->name == NULL)
      printf("<script>");
    else
      printf("<fn %s>", fn->name->chars);
    break;
  }
  case OBJ_NATIVE:
    printf("<native fn %s>", AS_NATIVE(value)->name);
    break;
  case OBJ_ARRAY: {
    // Print like the literal that built it: [e0, e1, ...].
    ObjArray *array = AS_ARRAY(value);
    printf("[");
    for (int i = 0; i < array->elements.count; i++) {
      if (i > 0)
        printf(", ");
      printValue(array->elements.values[i]);
    }
    printf("]");
    break;
  }
  case OBJ_MAP: {
    // Print like the literal: {k0: v0, k1: v1}. Iteration order is bucket order,
    // not insertion order — a deliberate, documented property of a hash map.
    ObjMap *map = AS_MAP(value);
    printf("{");
    bool first = true;
    for (int i = 0; i < map->capacity; i++) {
      if (map->entries[i].state != MAP_OCCUPIED)
        continue;
      if (!first)
        printf(", ");
      first = false;
      printValue(map->entries[i].key);
      printf(": ");
      printValue(map->entries[i].value);
    }
    printf("}");
    break;
  }
  case OBJ_STRUCT:
    printf("<struct %s>", AS_STRUCT(value)->name->chars);
    break;
  case OBJ_INSTANCE: {
    // Print like the constructor call would read: Point{x: 1, y: 2}, in the
    // struct's declared field order.
    ObjInstance *inst = AS_INSTANCE(value);
    printf("%s{", inst->type->name->chars);
    for (int i = 0; i < inst->type->fieldCount; i++) {
      if (i > 0)
        printf(", ");
      ObjString *field = inst->type->fieldNames[i];
      Value fv;
      tableGet(&inst->fields, field, &fv);
      printf("%s: ", field->chars);
      printValue(fv);
    }
    printf("}");
    break;
  }
  case OBJ_UPVALUE:
    // Upvalues never appear as first-class values; this is here for completeness.
    printf("<upvalue>");
    break;
  }
}

// Free one object. Strings own two allocations: the char buffer and the struct.
// All frees go through reallocate(ptr, size, 0) so the GC's byte tally stays
// accurate (it must subtract exactly what allocation added).
void freeObject(Obj *object) {
  switch (object->type) {
  case OBJ_STRING: {
    ObjString *string = (ObjString *)object;
    reallocate(string->chars, string->length + 1, 0);
    reallocate(string, sizeof(ObjString), 0);
    break;
  }
  case OBJ_FUNCTION: {
    // A function owns its chunk, so free that too, then the struct. (The name
    // ObjString is owned by the object list / intern pool, not freed here.)
    ObjFunction *function = (ObjFunction *)object;
    freeChunk(&function->chunk);
    reallocate(function, sizeof(ObjFunction), 0);
    break;
  }
  case OBJ_CLOSURE: {
    // A closure owns its upvalue POINTER array, but NOT the upvalues themselves
    // (those are shared, and freed as their own objects on the VM list).
    ObjClosure *closure = (ObjClosure *)object;
    reallocate(closure->upvalues,
               sizeof(ObjUpvalue *) * closure->upvalueCount, 0);
    reallocate(closure, sizeof(ObjClosure), 0);
    break;
  }
  case OBJ_NATIVE:
    // The wrapped C function and its name are static; only the struct is ours.
    reallocate(object, sizeof(ObjNative), 0);
    break;
  case OBJ_ARRAY: {
    // Free the backing element storage, then the struct.
    ObjArray *array = (ObjArray *)object;
    freeValueArray(&array->elements);
    reallocate(array, sizeof(ObjArray), 0);
    break;
  }
  case OBJ_MAP: {
    // The entries array is plain malloc'd (like the string Table); free it, then
    // the struct. The keys/values it held are separate objects on the GC list.
    ObjMap *map = (ObjMap *)object;
    free(map->entries);
    reallocate(map, sizeof(ObjMap), 0);
    break;
  }
  case OBJ_STRUCT: {
    ObjStruct *s = (ObjStruct *)object;
    free(s->fieldNames); // plain malloc'd by the compiler; not GC-accounted
    freeTable(&s->methods);
    reallocate(s, sizeof(ObjStruct), 0);
    break;
  }
  case OBJ_INSTANCE: {
    ObjInstance *inst = (ObjInstance *)object;
    freeTable(&inst->fields); // field names/values are separate GC objects
    reallocate(inst, sizeof(ObjInstance), 0);
    break;
  }
  case OBJ_UPVALUE:
    // The upvalue does not own the value it points at; just free the struct.
    reallocate(object, sizeof(ObjUpvalue), 0);
    break;
  }
}

// Walk the intrusive list and free everything. This is the manual stand-in for a
// garbage collector: because every object was threaded onto vm.objects at birth,
// we can reclaim them all here at VM shutdown.
void freeObjects(void) {
  Obj *object = vm.objects;
  while (object != NULL) {
    Obj *next = object->next;
    freeObject(object);
    object = next;
  }
  vm.objects = NULL;
}
