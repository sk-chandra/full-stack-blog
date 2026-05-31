#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "table.h"
#include "vm.h" // for the global object list head and the intern table

// Allocate a heap object of `size` bytes, tag it, and thread it onto the VM's
// intrusive object list so it can be freed at shutdown. Every object is born
// through here — the single choke point a garbage collector would later hook.
static Obj *allocateObject(size_t size, ObjType type) {
  Obj *object = malloc(size);
  if (object == NULL) {
    fprintf(stderr, "cnano: out of memory allocating object\n");
    exit(70);
  }
  object->type = type;
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
  // because the source is often a transient slice of the program text.
  char *heapChars = malloc(length + 1);
  if (heapChars == NULL) {
    fprintf(stderr, "cnano: out of memory copying string\n");
    exit(70);
  }
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';
  return allocateString(heapChars, length, hash);
}

void printObject(Value value) {
  switch (AS_OBJ(value)->type) {
  case OBJ_STRING:
    printf("%s", AS_CSTRING(value));
    break;
  }
}

// Free one object. Strings own two allocations: the char buffer and the struct.
static void freeObject(Obj *object) {
  switch (object->type) {
  case OBJ_STRING: {
    ObjString *string = (ObjString *)object;
    free(string->chars);
    free(string);
    break;
  }
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
