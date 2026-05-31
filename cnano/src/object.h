// object.h — heap-allocated values ("objects").
//
// Integers, booleans and nil fit entirely inside a Value (they are "value
// types"). Strings do not: they are variable-length and must live on the heap,
// with the Value merely pointing at them. Every heap value in cnano is an
// `Obj`. This file introduces TWO ideas that recur in every real language VM:
//
//   1. "Inheritance" in C via struct embedding. `Obj` is a common header; each
//      concrete object type puts an `Obj` as its FIRST field, so a pointer to
//      the concrete type can be cast to `Obj*` and back. The leading `type` tag
//      tells you which concrete type you really have.
//
//   2. A VM-owned intrusive linked list of every object ever allocated, so the
//      VM can free them all on shutdown. (A real language replaces this manual
//      list with a garbage collector — but the allocation hook is the same.)
#ifndef CNANO_OBJECT_H
#define CNANO_OBJECT_H

#include "common.h"
#include "value.h"

typedef enum {
  OBJ_STRING,
} ObjType;

// The common header shared by every heap object. Because it is the first field
// of every concrete object, `(Obj*)somethingConcrete` is always valid.
struct Obj {
  ObjType type;
  struct Obj *next; // intrusive linked list: the VM threads all objects here
};

// A heap string. `chars` is a separately-allocated, NUL-terminated buffer of
// `length` bytes. `hash` is cached at creation time so hash-table lookups never
// re-hash the string — a standard and important optimisation.
// Named struct (not anonymous) so table.h can forward-declare it as
// `struct ObjString` to break the include cycle.
struct ObjString {
  Obj obj; // MUST be first: enables the Obj "base class" cast
  int length;
  char *chars;
  uint32_t hash;
};
typedef struct ObjString ObjString;

// Convenience predicate + accessors, mirroring the Value macros.
#define IS_STRING(value) isObjType(value, OBJ_STRING)
#define AS_STRING(value) ((ObjString *)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjString *)AS_OBJ(value))->chars)

// Safe because the macro evaluates `value` once into a local before reading the
// tag (writing it as a function avoids the double-evaluation a macro would risk).
static inline bool isObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

// Create a string by COPYING `length` bytes from `chars` (the source may be a
// transient slice of program text). Interning (see below) means identical
// strings share one ObjString.
ObjString *copyString(const char *chars, int length);

// Print an object value (dispatched from printValue).
void printObject(Value value);

// Free every object the VM has allocated. Called at VM shutdown.
void freeObjects(void);

#endif // CNANO_OBJECT_H
