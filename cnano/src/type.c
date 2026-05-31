// type.c — construction, ownership, and naming of static types.
//
// Primitive types (any/int/bool/str/nil) are shared SINGLETONS: there is only
// one `int` type, so equal primitives compare by pointer. The parametric types —
// array, map, function — are built from other types (`[int]`, `{str: int}`), so
// each distinct one is a separate heap allocation. Rather than track each type's
// lifetime, we drop them all into one ARENA and free it in a single call once
// every pass that reads types (type-checking, native codegen) has finished.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "type.h"

// --- primitive singletons --------------------------------------------------

static Type anyType = {TY_ANY, NULL, {NULL, NULL}, {NULL, 0, NULL}};
static Type intType = {TY_INT, NULL, {NULL, NULL}, {NULL, 0, NULL}};
static Type boolType = {TY_BOOL, NULL, {NULL, NULL}, {NULL, 0, NULL}};
static Type strType = {TY_STR, NULL, {NULL, NULL}, {NULL, 0, NULL}};
static Type nilType = {TY_NIL, NULL, {NULL, NULL}, {NULL, 0, NULL}};

Type *typeAny(void) { return &anyType; }
Type *typeInt(void) { return &intType; }
Type *typeBool(void) { return &boolType; }
Type *typeStr(void) { return &strType; }
Type *typeNil(void) { return &nilType; }

// --- the arena of parametric types -----------------------------------------

static Type **arena;     // every array/map/function type ever built
static int arenaCount;
static int arenaCapacity;

static Type *allocType(TypeKind kind) {
  if (arenaCount + 1 > arenaCapacity) {
    arenaCapacity = arenaCapacity < 8 ? 8 : arenaCapacity * 2;
    arena = (Type **)realloc(arena, sizeof(Type *) * arenaCapacity);
    if (arena == NULL) {
      fprintf(stderr, "cnano: out of memory in type arena\n");
      exit(70);
    }
  }
  Type *t = (Type *)calloc(1, sizeof(Type));
  if (t == NULL) {
    fprintf(stderr, "cnano: out of memory allocating a type\n");
    exit(70);
  }
  t->kind = kind;
  arena[arenaCount++] = t;
  return t;
}

Type *typeArray(Type *element) {
  Type *t = allocType(TY_ARRAY);
  t->element = element;
  return t;
}

Type *typeMap(Type *key, Type *value) {
  Type *t = allocType(TY_MAP);
  t->map.key = key;
  t->map.value = value;
  return t;
}

Type *typeFunction(Type **params, int paramCount, Type *returnType) {
  Type *t = allocType(TY_FUNCTION);
  t->fn.params = params; // ownership transferred to the arena entry
  t->fn.paramCount = paramCount;
  t->fn.returnType = returnType;
  return t;
}

void freeTypes(void) {
  for (int i = 0; i < arenaCount; i++) {
    free(arena[i]->fn.params); // NULL for non-function types — free(NULL) is ok
    free(arena[i]);
  }
  free(arena);
  arena = NULL;
  arenaCount = 0;
  arenaCapacity = 0;
}

// --- naming ----------------------------------------------------------------

// typeName must build composite names like "[int]" and "{str: int}", which need
// a buffer. But callers routinely use TWO names in one printf (`expects %s got
// %s`), so a single static buffer would let the second call clobber the first.
// A small RING of buffers fixes that: each call gets the next slot, so several
// recent results stay valid simultaneously. Not thread-safe, but the checker is
// single-threaded and only ever needs a few live names at once.
#define NAME_RING 4
#define NAME_LEN 128
static char nameRing[NAME_RING][NAME_LEN];
static int nameSlot;

const char *typeName(const Type *type) {
  switch (type->kind) {
  case TY_ANY:
    return "any";
  case TY_INT:
    return "int";
  case TY_BOOL:
    return "bool";
  case TY_STR:
    return "str";
  case TY_NIL:
    return "nil";
  case TY_FUNCTION:
    return "fn";
  case TY_ARRAY: {
    char *buf = nameRing[nameSlot++ % NAME_RING];
    snprintf(buf, NAME_LEN, "[%s]", typeName(type->element));
    return buf;
  }
  case TY_MAP: {
    // Resolve the inner names first (they each grab a ring slot), then format —
    // so the key and value names don't share the slot we are about to write.
    const char *k = typeName(type->map.key);
    const char *v = typeName(type->map.value);
    char *buf = nameRing[nameSlot++ % NAME_RING];
    snprintf(buf, NAME_LEN, "{%s: %s}", k, v);
    return buf;
  }
  default:
    return "?";
  }
}
