// value.h — the runtime value representation and the constant pool.
//
// Until now a Value was just `int64_t`: cnano had exactly one type. The moment a
// language gains a SECOND type (here: booleans and nil), every value must carry
// two things — its data AND a tag describing what kind of data it is. The
// universal solution is a *tagged union*: a small struct with a `type` field and
// a `union` of the possible payloads. This is exactly how CPython, Lua, Ruby and
// friends represent dynamically typed values under the hood.
#ifndef CNANO_VALUE_H
#define CNANO_VALUE_H

#include "common.h"

// Forward declaration. Heap-allocated values (currently just strings) are
// represented by an `Obj` defined in object.h. value.h must not include
// object.h (object.h includes value.h), so we only name the type here and hold
// it as a pointer. This is the standard way to break an include cycle in C.
typedef struct Obj Obj;

// The set of runtime types cnano knows about. Adding a type to the language
// starts by adding a tag here.
typedef enum {
  VAL_NIL,  // the absence of a value
  VAL_BOOL, // true / false
  VAL_INT,  // 64-bit signed integer
  VAL_OBJ,  // a heap-allocated object (string, ...): payload is a pointer
} ValueType;

// A value = a tag + a union of payloads. The union means a Value is only as big
// as its largest member (here 8 bytes) plus the tag — booleans and ints share
// the same storage, and `type` tells you which member is currently valid. You
// must NEVER read `as.integer` from a value whose type is VAL_BOOL; the helper
// macros below exist to make that mistake hard.
typedef struct {
  ValueType type;
  union {
    bool boolean;
    int64_t integer;
    Obj *obj; // for VAL_OBJ: points at a heap object (see object.h)
  } as;
} Value;

// --- constructors: C value -> cnano Value ----------------------------------
// Compound literals ((Value){...}) build a struct inline. Wrapping them in
// macros keeps construction readable and centralised.
#define NIL_VAL ((Value){VAL_NIL, {.integer = 0}})
#define BOOL_VAL(b) ((Value){VAL_BOOL, {.boolean = (b)}})
#define INT_VAL(i) ((Value){VAL_INT, {.integer = (i)}})
#define OBJ_VAL(object) ((Value){VAL_OBJ, {.obj = (Obj *)(object)}})

// --- type predicates: ask what a Value is ----------------------------------
#define IS_NIL(value) ((value).type == VAL_NIL)
#define IS_BOOL(value) ((value).type == VAL_BOOL)
#define IS_INT(value) ((value).type == VAL_INT)
#define IS_OBJ(value) ((value).type == VAL_OBJ)

// --- accessors: cnano Value -> C value -------------------------------------
// Only valid when the matching predicate is true. The VM checks types BEFORE
// calling these, turning "wrong type" into a clean runtime error rather than a
// silent misread of the union.
#define AS_BOOL(value) ((value).as.boolean)
#define AS_INT(value) ((value).as.integer)
#define AS_OBJ(value) ((value).as.obj)

// The constant pool: a growable array of Values, unchanged in spirit from
// before — it just stores tagged Values now instead of bare ints.
typedef struct {
  int count;
  int capacity;
  Value *values;
} ValueArray;

void initValueArray(ValueArray *array);
void freeValueArray(ValueArray *array);
int writeValueArray(ValueArray *array, Value value);
void printValue(Value value);

// Value-level equality, used by the `==` / `!=` operators. Values of different
// types are never equal; same-type values compare their payloads.
bool valuesEqual(Value a, Value b);

#endif // CNANO_VALUE_H
