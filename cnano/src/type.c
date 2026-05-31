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

#include "object.h" // for ObjString->chars in typeName
#include "type.h"

// --- primitive singletons --------------------------------------------------

static Type anyType = {.kind = TY_ANY};
static Type intType = {.kind = TY_INT};
static Type floatType = {.kind = TY_FLOAT};
static Type boolType = {.kind = TY_BOOL};
static Type strType = {.kind = TY_STR};
static Type nilType = {.kind = TY_NIL};

Type *typeAny(void) { return &anyType; }
Type *typeInt(void) { return &intType; }
Type *typeFloat(void) { return &floatType; }
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

Type *typeNullable(Type *inner) {
  // `any` already includes nil; `T??` is just `T?`; `nil?` is `nil`. Otherwise
  // wrap, reusing the `element` field to hold the inner type.
  if (inner->kind == TY_ANY || inner->kind == TY_NULLABLE || inner->kind == TY_NIL)
    return inner;
  Type *t = allocType(TY_NULLABLE);
  t->element = inner;
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

Type *typeStruct(ObjString *name, ObjString **fieldNames, Type **fieldTypes,
                 int fieldCount) {
  Type *t = allocType(TY_STRUCT);
  t->strct.name = name;
  t->strct.fieldNames = fieldNames; // borrowed from the AST; not freed here
  t->strct.fieldTypes = fieldTypes;
  t->strct.fieldCount = fieldCount;
  return t;
}

Type *typeStructRef(ObjString *name) {
  Type *t = allocType(TY_STRUCT);
  t->strct.name = name;
  t->strct.fieldNames = NULL;
  t->strct.fieldTypes = NULL;
  t->strct.fieldCount = -1; // unresolved: the checker will match it by name
  return t;
}

// Whether two union members are "the same" for dedup purposes.
static bool sameMember(Type *a, Type *b) {
  if (a == b)
    return true;
  if (a->kind != b->kind)
    return false;
  if (a->kind == TY_STRUCT)
    return a->strct.name == b->strct.name;
  // Primitive kinds (int/bool/str/nil) are singletons, so equal-kind == same.
  // Composite members (array/map/...) only dedup when pointer-identical (above).
  return a->kind == TY_INT || a->kind == TY_BOOL || a->kind == TY_STR ||
         a->kind == TY_NIL;
}

#define MAX_UNION_MEMBERS 32

Type *typeUnite(Type *a, Type *b) {
  if (a->kind == TY_ANY || b->kind == TY_ANY)
    return typeAny(); // `any` already covers everything
  Type *tmp[MAX_UNION_MEMBERS];
  int n = 0;
  Type *srcs[2] = {a, b};
  for (int s = 0; s < 2; s++) {
    // Flatten a nested union into its members; otherwise add the type itself.
    int mc = srcs[s]->kind == TY_UNION ? srcs[s]->uni.count : 1;
    for (int i = 0; i < mc; i++) {
      Type *m = srcs[s]->kind == TY_UNION ? srcs[s]->uni.members[i] : srcs[s];
      bool dup = false;
      for (int j = 0; j < n; j++)
        if (sameMember(tmp[j], m)) {
          dup = true;
          break;
        }
      if (!dup && n < MAX_UNION_MEMBERS)
        tmp[n++] = m;
    }
  }
  if (n == 1)
    return tmp[0]; // a union of one is just that type
  Type **members = (Type **)malloc(sizeof(Type *) * n);
  if (members == NULL) {
    fprintf(stderr, "cnano: out of memory building a union type\n");
    exit(70);
  }
  for (int i = 0; i < n; i++)
    members[i] = tmp[i];
  Type *u = allocType(TY_UNION);
  u->uni.members = members;
  u->uni.count = n;
  return u;
}

void freeTypes(void) {
  for (int i = 0; i < arenaCount; i++) {
    free(arena[i]->fn.params); // NULL for non-function types — free(NULL) is ok
    if (arena[i]->kind == TY_UNION)
      free(arena[i]->uni.members);
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
  case TY_FLOAT:
    return "float";
  case TY_BOOL:
    return "bool";
  case TY_STR:
    return "str";
  case TY_NIL:
    return "nil";
  case TY_FUNCTION:
    return "fn";
  case TY_STRUCT:
    return type->strct.name->chars; // the declared struct name
  case TY_ARRAY: {
    char *buf = nameRing[nameSlot++ % NAME_RING];
    snprintf(buf, NAME_LEN, "[%s]", typeName(type->element));
    return buf;
  }
  case TY_NULLABLE: {
    const char *inner = typeName(type->element);
    char *buf = nameRing[nameSlot++ % NAME_RING];
    snprintf(buf, NAME_LEN, "%s?", inner);
    return buf;
  }
  case TY_UNION: {
    // Build into a local first (member typeName calls churn the ring), then take
    // a ring slot at the very end — so the result survives one more typeName call.
    char local[NAME_LEN];
    int off = 0;
    for (int i = 0; i < type->uni.count && off < NAME_LEN - 1; i++) {
      const char *m = typeName(type->uni.members[i]);
      off += snprintf(local + off, NAME_LEN - off, "%s%s", i ? " | " : "", m);
    }
    char *buf = nameRing[nameSlot++ % NAME_RING];
    snprintf(buf, NAME_LEN, "%s", local);
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
