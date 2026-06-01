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

#include "chunk.h"
#include "common.h"
#include "table.h" // instances store their fields in a Table
#include "value.h"

typedef enum {
  OBJ_STRING,
  OBJ_FUNCTION,
  OBJ_NATIVE,
  OBJ_ARRAY,
  OBJ_MAP,
  OBJ_STRUCT,   // a struct TYPE / constructor (e.g. Point)
  OBJ_INSTANCE, // an instance of a struct (e.g. Point(1, 2))
  OBJ_ENUM,     // an enum TYPE (e.g. Color), a namespace of named members
  OBJ_ENUM_MEMBER, // one member of an enum (e.g. Color.Red), a singleton value
  OBJ_GENERATOR, // a suspended generator: a function's frozen execution state
  OBJ_UPVALUE,
  OBJ_CLOSURE,
} ObjType;

// The common header shared by every heap object. Because it is the first field
// of every concrete object, `(Obj*)somethingConcrete` is always valid.
struct Obj {
  ObjType type;
  bool isMarked;    // GC mark bit: set during the mark phase, cleared by sweep
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

// A function is a first-class heap object. The crucial idea: each function owns
// ITS OWN Chunk of bytecode. The top-level program is itself compiled as a
// function (an implicit "main"), so the VM only ever runs functions — uniform
// and simple. `arity` is the declared parameter count; `name` is for error
// messages and disassembly (NULL for the top-level script).
typedef struct {
  Obj obj;          // MUST be first
  int arity;        // number of parameters
  int upvalueCount; // how many enclosing variables this function captures
  bool isGenerator; // body contains `yield`: calling it returns a Generator
  Chunk chunk;      // the function's own compiled bytecode
  ObjString *name;
} ObjFunction;

// A NATIVE function: a builtin implemented in C rather than cnano bytecode (e.g.
// clock(), str(x)). It wraps a C function pointer; the VM calls it directly with
// the arguments already on the stack, with no call frame. `arity` is the expected
// argument count (checked by the VM); `name` is for errors and printing. The C
// function returns false (after reporting a runtime error) to signal failure, and
// otherwise stores its result through `*result` — an error channel the simplest
// "return a Value" signature lacks, and one arrays/maps will need for bounds.
typedef bool (*NativeFn)(int argCount, Value *args, Value *result);

typedef struct {
  Obj obj; // MUST be first
  NativeFn function;
  const char *name; // a static C string; not owned
  int arity;
} ObjNative;

// An ARRAY: a growable, ordered list of values. It reuses ValueArray — the same
// dynamic-array (count/capacity/double-on-full) the constant pool uses — so the
// only new code is the object wrapper, indexing, and methods. Elements are
// arbitrary Values, so an array is heterogeneous at runtime (the [T] type is a
// compile-time promise the checker enforces, then erases).
typedef struct {
  Obj obj; // MUST be first
  ValueArray elements;
} ObjArray;

// A MAP: a dictionary from keys to values. Unlike the global/intern Table (which
// is keyed by interned ObjString* and compared by pointer), a map keys on ARBITRARY
// values — ints, bools, nil, strings — so it needs value hashing (hashValue) and
// value equality (valuesEqual). It is its own open-addressed, linear-probing hash
// table; because maps don't support deletion (yet), there are no tombstones — an
// entry is simply occupied or not (we can't use a sentinel key, since nil is a
// legal key).
// A bucket's state. Tombstones (deleted-but-not-empty) keep linear-probe chains
// intact after a `.remove`, so a later lookup of a still-present key doesn't stop
// early at the hole.
enum { MAP_EMPTY = 0, MAP_OCCUPIED, MAP_TOMBSTONE };

typedef struct {
  Value key;
  Value value;
  uint8_t state; // MAP_EMPTY / MAP_OCCUPIED / MAP_TOMBSTONE
} MapEntry;

typedef struct {
  Obj obj; // MUST be first
  int count;      // LIVE entries (what .len() reports)
  int tombstones; // deleted slots still occupying the probe sequence
  int capacity;
  MapEntry *entries;
} ObjMap;

// A STRUCT type — the runtime object a `struct Point { ... }` declaration binds
// to the name `Point`. It is callable: `Point(1, 2)` constructs an instance. It
// stores the field names in declaration order, which is all the runtime needs
// (field TYPES are a compile-time concern, checked then erased).
typedef struct {
  Obj obj; // MUST be first
  ObjString *name;
  ObjString **fieldNames; // declaration order; positional construction
  int fieldCount;
  Table methods; // method name -> ObjClosure, filled at declaration by OP_METHOD
} ObjStruct;

// An INSTANCE of a struct. Fields live in a Table keyed by interned field name —
// the same hash table that backs globals — so field get/set is a table lookup.
// `type` points back to the struct it was built from (for printing and for
// rejecting writes to undeclared fields).
typedef struct {
  Obj obj; // MUST be first
  ObjStruct *type;
  Table fields;
} ObjInstance;

// Forward decl: a member points back at its enum, and an enum's table holds them.
typedef struct ObjEnum ObjEnum;

// An ENUM TYPE — the object a `enum Color { Red, Green }` declaration binds to the
// name `Color`. It is a namespace: `members` maps each member name to that
// member's singleton value, so `Color.Red` is the same field-access (OP_GET_FIELD)
// machinery structs use. Unlike a struct it is not callable (you don't construct
// an enum), it just hands out its pre-made members.
struct ObjEnum {
  Obj obj; // MUST be first
  ObjString *name;
  Table members; // member name -> ObjEnumMember value (built at declaration)
};

// One MEMBER of an enum (e.g. `Color.Red`). Members are SINGLETONS created once at
// the declaration, so equality is identity (pointer comparison, which valuesEqual
// already does for objects) — `Color.Red == Color.Red` is true, and two different
// members are never equal. `ordinal` is the 0-based declaration position.
typedef struct {
  Obj obj; // MUST be first
  ObjEnum *parent;     // the enum this belongs to (for printing + type())
  ObjString *name;     // this member's name
  int ordinal;         // declaration order, 0-based
} ObjEnumMember;

// An "upvalue": the runtime representation of a variable captured by a closure
// from an enclosing function. The whole problem closures solve is that a captured
// local lives on the stack but may OUTLIVE the frame that created it. An upvalue
// solves this with one level of indirection:
//   - while the variable is still on the stack, `location` points AT that stack
//     slot, so reads/writes go straight through ("open" upvalue);
//   - when the slot is about to disappear, we COPY the value into `closed` and
//     repoint `location` at it, moving the variable to the heap ("closed").
// `next` threads open upvalues onto a VM list so they can be shared and closed.
typedef struct ObjUpvalue {
  Obj obj;
  Value *location;        // points at the live variable (stack slot or &closed)
  Value closed;           // the value's heap home once closed
  struct ObjUpvalue *next; // intrusive list of OPEN upvalues, sorted by slot
} ObjUpvalue;

// A "closure": a function paired with the array of upvalues it captured. At
// runtime we never call a bare ObjFunction — we call a closure. A plain function
// with no captures still gets a closure (with zero upvalues), keeping the VM's
// call path uniform.
typedef struct {
  Obj obj;
  ObjFunction *function;
  ObjUpvalue **upvalues; // captured variables, indexed as the compiler assigned
  int upvalueCount;
} ObjClosure;

// A GENERATOR: a generator function's suspended execution state. Calling a
// generator function doesn't run it — it returns one of these, frozen at its
// start. Each `.next()` thaws the saved stack window + instruction pointer back
// onto the VM, runs to the next `yield` (or the final return), then re-freezes.
// This is a coroutine: the whole feature is "save and restore a slice of the
// value stack plus an ip".
typedef struct {
  Obj obj; // MUST be first
  ObjClosure *closure; // the generator function (also holds its upvalues)
  uint8_t *ip;         // where to resume (valid once `started`)
  Value *saved;        // the frozen stack window (slot 0 .. top), heap-allocated
  int savedCount;      // live values in `saved`
  bool started;        // false until the first .next()
  bool done;           // true once the generator has returned
} ObjGenerator;

#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define AS_FUNCTION(value) ((ObjFunction *)AS_OBJ(value))
#define IS_NATIVE(value) isObjType(value, OBJ_NATIVE)
#define AS_NATIVE(value) ((ObjNative *)AS_OBJ(value))
#define IS_ARRAY(value) isObjType(value, OBJ_ARRAY)
#define AS_ARRAY(value) ((ObjArray *)AS_OBJ(value))
#define IS_MAP(value) isObjType(value, OBJ_MAP)
#define AS_MAP(value) ((ObjMap *)AS_OBJ(value))
#define IS_STRUCT(value) isObjType(value, OBJ_STRUCT)
#define AS_STRUCT(value) ((ObjStruct *)AS_OBJ(value))
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define AS_INSTANCE(value) ((ObjInstance *)AS_OBJ(value))
#define IS_ENUM(value) isObjType(value, OBJ_ENUM)
#define AS_ENUM(value) ((ObjEnum *)AS_OBJ(value))
#define IS_ENUM_MEMBER(value) isObjType(value, OBJ_ENUM_MEMBER)
#define AS_ENUM_MEMBER(value) ((ObjEnumMember *)AS_OBJ(value))
#define IS_GENERATOR(value) isObjType(value, OBJ_GENERATOR)
#define AS_GENERATOR(value) ((ObjGenerator *)AS_OBJ(value))
#define IS_CLOSURE(value) isObjType(value, OBJ_CLOSURE)
#define AS_CLOSURE(value) ((ObjClosure *)AS_OBJ(value))

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

// Allocate a fresh, empty function (arity 0, empty chunk, no name). The compiler
// fills in the chunk and arity as it compiles the body.
ObjFunction *newFunction(void);

// Wrap a function in a closure, allocating (but not yet filling) its upvalue
// array. The VM fills the upvalues in immediately after, via OP_CLOSURE.
ObjClosure *newClosure(ObjFunction *function);
// A fresh, not-yet-started generator wrapping `closure`. `saved`/`savedCount`
// are filled by the VM with the initial call window (callee + args).
ObjGenerator *newGenerator(ObjClosure *closure);

// Allocate a native-function object wrapping the C function `fn`. `name` must be
// a string literal / static string (it is borrowed, not copied or freed).
ObjNative *newNative(NativeFn fn, const char *name, int arity);

// Allocate a fresh, empty array. The VM appends elements (e.g. from a literal).
ObjArray *newArrayObject(void);

// Allocate a fresh, empty map.
ObjMap *newMapObject(void);

// Allocate a struct type. Takes ownership of the `fieldNames` array.
ObjStruct *newStruct(ObjString *name, ObjString **fieldNames, int fieldCount);

// Allocate a fresh instance of `type` with an empty field table.
ObjInstance *newInstance(ObjStruct *type);

// Create an empty enum type named `name` (members are added afterwards), and one
// member belonging to `parent`. Both are GC-managed.
ObjEnum *newEnum(ObjString *name);
ObjEnumMember *newEnumMember(ObjEnum *parent, ObjString *name, int ordinal);

// Whether `s` declares a field named `name` (linear search; structs are small).
bool structHasField(ObjStruct *s, ObjString *name);

// Map operations. mapGet copies the value for `key` into *out and returns true if
// present. mapSet inserts or overwrites, growing as needed. Both assume the key
// is hashable — callers MUST check isHashableKey first and report a clean error.
bool mapGet(ObjMap *map, Value key, Value *out);
void mapSet(ObjMap *map, Value key, Value value);
// Remove `key`; returns true if it was present (leaves a tombstone behind).
bool mapDelete(ObjMap *map, Value key);

// Only primitive values and strings can be map keys. Heap aggregates (arrays,
// maps, functions, ...) are not hashable — using one as a key is a runtime error.
bool isHashableKey(Value key);

// Allocate a fresh open upvalue pointing at the stack `slot`.
ObjUpvalue *newUpvalue(Value *slot);

// Print an object value (dispatched from printValue).
void printObject(Value value);

// Free a single object and the memory it owns. Called by the GC's sweep phase
// (for unreachable objects) and by freeObjects at shutdown.
void freeObject(Obj *object);

// Free every object the VM has allocated. Called at VM shutdown.
void freeObjects(void);

#endif // CNANO_OBJECT_H
