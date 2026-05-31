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
  ObjUpvalue **upvalues = malloc(sizeof(ObjUpvalue *) * function->upvalueCount);
  if (upvalues == NULL && function->upvalueCount > 0) {
    fprintf(stderr, "cnano: out of memory allocating closure upvalues\n");
    exit(70);
  }
  for (int i = 0; i < function->upvalueCount; i++)
    upvalues[i] = NULL;

  ObjClosure *closure =
      (ObjClosure *)allocateObject(sizeof(ObjClosure), OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  return closure;
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
  case OBJ_UPVALUE:
    // Upvalues never appear as first-class values; this is here for completeness.
    printf("<upvalue>");
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
  case OBJ_FUNCTION: {
    // A function owns its chunk, so free that too, then the struct. (The name
    // ObjString is owned by the object list / intern pool, not freed here.)
    ObjFunction *function = (ObjFunction *)object;
    freeChunk(&function->chunk);
    free(function);
    break;
  }
  case OBJ_CLOSURE: {
    // A closure owns its upvalue POINTER array, but NOT the upvalues themselves
    // (those are shared, and freed as their own objects on the VM list).
    ObjClosure *closure = (ObjClosure *)object;
    free(closure->upvalues);
    free(closure);
    break;
  }
  case OBJ_UPVALUE:
    // The upvalue does not own the value it points at; just free the struct.
    free(object);
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
