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

typedef struct ObjString ObjString; // forward decl; full type in object.h

typedef enum {
  TY_ANY,  // unknown / dynamic — compatible with every type (the gradual escape hatch)
  TY_INT,
  TY_FLOAT,
  TY_BOOL,
  TY_STR,
  TY_NIL,
  TY_ARRAY,    // a homogeneous list; carries an element type (see Type.element)
  TY_NULLABLE, // `T?` — T or nil; the inner T is stored in Type.element
  TY_MAP,      // a dictionary; carries key + value types (see Type.map)
  TY_STRUCT,   // a user-defined record type, by name (see Type.strct)
  TY_ENUM,     // a user-defined enum type, by name (reuses Type.strct.name)
  TY_UNION,    // `A | B | ...` — one of several member types (see Type.uni)
  TY_FUNCTION, // a callable; carries param/return types (see Type.fn)
} TypeKind;

// A type. Primitive types (any/int/bool/str/nil) need only the `kind` tag and are
// shared SINGLETONS. The PARAMETRIC types — array, map, function — also carry the
// types they are built from, which is exactly why a flat enum is no longer enough:
// `[int]` and `[bool]` are both arrays but different types. Composite types are
// allocated in a small arena (see type.c) and freed all at once.
typedef struct Type {
  TypeKind kind;
  struct Type *element; // TY_ARRAY: the element type
  struct {
    struct Type *key;
    struct Type *value;
  } map; // valid only when kind == TY_MAP
  struct {
    struct Type **params; // parameter types (heap array)
    int paramCount;
    struct Type *returnType;
  } fn; // valid only when kind == TY_FUNCTION
  struct {
    ObjString *name;          // the struct's name (interned; nominal identity)
    ObjString **fieldNames;   // borrowed from the AST declaration
    struct Type **fieldTypes; // parallel; borrowed from the AST
    int fieldCount;           // -1 means an UNRESOLVED reference (just a name)
  } strct; // valid only when kind == TY_STRUCT
  struct {
    struct Type **members; // the alternatives (heap array, arena-owned)
    int count;
  } uni; // valid only when kind == TY_UNION
} Type;

// --- type constructors -----------------------------------------------------
// Primitives are shared singletons (one `int` type, etc.) and are never freed.
Type *typeAny(void);
Type *typeInt(void);
Type *typeFloat(void);
Type *typeBool(void);
Type *typeStr(void);
Type *typeNil(void);

// Parametric types are allocated in the type arena and freed together by
// freeTypes(). `typeFunction` takes ownership of the `params` array.
Type *typeArray(Type *element);
// `T?` — T or nil. Idempotent, and `any?`/`nil?` collapse sensibly.
Type *typeNullable(Type *inner);
Type *typeMap(Type *key, Type *value);
Type *typeFunction(Type **params, int paramCount, Type *returnType);

// A fully-resolved struct type (fieldNames/fieldTypes are borrowed, not owned).
Type *typeStruct(ObjString *name, ObjString **fieldNames, Type **fieldTypes,
                 int fieldCount);
// An unresolved struct reference — just a name, as produced by a `: Name`
// annotation before the checker has matched it to a declaration.
Type *typeStructRef(ObjString *name);

// A nominal enum type, identified by name (reuses the `strct.name` slot). Like a
// struct, two enum types are equal iff they share a name.
Type *typeEnum(ObjString *name);

// Combine two types into a union (`a | b`), normalising: `any` absorbs, duplicate
// members collapse, and a 1-member union degrades to that member.
Type *typeUnite(Type *a, Type *b);

// Free every parametric type allocated since the last call. Primitive singletons
// are untouched. Called once after all type-consuming passes (check + codegen).
void freeTypes(void);

// Human-readable name for error messages ("int", "[int]", "{str: int}", "fn").
const char *typeName(const Type *type);

// Could a `nil` legitimately have this type? True for any/nil/`T?` and a union
// that includes nil. Used to decide whether a function may fall through to its
// implicit `return nil` (the all-paths-return check).
bool typeAcceptsNil(const Type *type);

#endif // CNANO_TYPE_H
