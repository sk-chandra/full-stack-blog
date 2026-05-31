// type.h — the static type system for cnano's gradual type checker.
//
// This is the data the type checker (typecheck.{h,c}) computes and reasons about.
// It is a COMPILE-TIME concept only: types are erased before the VM runs (the VM
// is still dynamically typed). The design is GRADUAL typing — there is an `any`
// type that is compatible with everything, so unannotated code is simply `any`
// and never causes a type error. You only get static checking where you opt in
// with annotations. That is the static-vs-dynamic trade-off made concrete:
// annotate for safety, leave dynamic for flexibility.
#ifndef CNANO_TYPE_H
#define CNANO_TYPE_H

#include "common.h"

typedef enum {
  TY_ANY,  // unknown / dynamic — compatible with every type (the gradual escape hatch)
  TY_INT,
  TY_BOOL,
  TY_STR,
  TY_NIL,
  TY_FUNCTION, // a callable; carries param/return types (see Type.fn)
} TypeKind;

// A type. Primitive types need only the `kind` tag; function types also carry
// their signature so calls can be checked. Types are heap-allocated and owned by
// a TypeArena (see typecheck.c) that frees them all at once — far simpler than
// tracking each type's lifetime individually.
typedef struct Type {
  TypeKind kind;
  struct {
    struct Type **params; // parameter types (heap array)
    int paramCount;
    struct Type *returnType;
  } fn; // valid only when kind == TY_FUNCTION
} Type;

// Human-readable name for error messages ("int", "bool", "fn", ...).
const char *typeName(const Type *type);

#endif // CNANO_TYPE_H
