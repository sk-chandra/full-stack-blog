#include <stdio.h>
#include <string.h>
#include <time.h>

#include "builtins.h"
#include "object.h"
#include "table.h"
#include "vm.h" // vm.globals, runtimeError

// Compare an interned method/argument name to a C string literal. Method names
// arrive as ObjString*; the dispatch tables below use plain C strings.
static bool nameIs(ObjString *name, const char *literal) {
  size_t len = strlen(literal);
  return (size_t)name->length == len && memcmp(name->chars, literal, len) == 0;
}

// --- value -> string, the core of str() ------------------------------------

// Render `v` to a fresh ObjString. Mirrors printValue, but builds a string
// instead of writing to stdout. Strings render as themselves (so str() is the
// identity on strings); everything else uses a small fixed buffer.
static ObjString *stringify(Value v) {
  if (IS_STRING(v))
    return AS_STRING(v);
  char buf[32];
  int len = 0;
  if (IS_INT(v))
    len = snprintf(buf, sizeof(buf), "%lld", (long long)AS_INT(v));
  else if (IS_BOOL(v))
    len = snprintf(buf, sizeof(buf), "%s", AS_BOOL(v) ? "true" : "false");
  else if (IS_NIL(v))
    len = snprintf(buf, sizeof(buf), "nil");
  else
    len = snprintf(buf, sizeof(buf), "<object>");
  return copyString(buf, len);
}

// --- free builtin functions ------------------------------------------------

// clock() -> int : milliseconds of CPU time since the program started. Useful
// for rough benchmarking; non-deterministic, so programs shouldn't golden-test it.
static bool clockNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  *result = INT_VAL((int64_t)(clock() * 1000 / CLOCKS_PER_SEC));
  return true;
}

// str(x) -> str : the string representation of any value.
static bool strNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  *result = OBJ_VAL(stringify(args[0]));
  return true;
}

void defineBuiltins(void) {
  struct {
    const char *name;
    NativeFn fn;
    int arity;
  } table[] = {
      {"clock", clockNative, 0},
      {"str", strNative, 1},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    ObjString *name = copyString(table[i].name, (int)strlen(table[i].name));
    ObjNative *native = newNative(table[i].fn, table[i].name, table[i].arity);
    tableSet(&vm.globals, name, OBJ_VAL(native));
  }
}

// --- methods ----------------------------------------------------------------

// A builtin method: a name, an expected argument count, and the C implementation
// (receiver plus its arguments). Each built-in TYPE has its own small table.
typedef bool (*MethodFn)(Value receiver, int argCount, Value *args,
                         Value *result);
typedef struct {
  const char *name;
  int arity;
  MethodFn fn;
} Method;

// string.len() -> int : the length in bytes.
static bool strLen(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  *result = INT_VAL(AS_STRING(receiver)->length);
  return true;
}

static Method stringMethods[] = {
    {"len", 0, strLen},
    {NULL, 0, NULL},
};

// array.len() -> int : the number of elements.
static bool arrayLen(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  *result = INT_VAL(AS_ARRAY(receiver)->elements.count);
  return true;
}

// array.push(x) -> nil : append x to the end (mutates the array).
static bool arrayPush(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  // The receiver and arg are on the VM stack (rooted); writeValueArray grows with
  // plain realloc and never collects, so this is GC-safe.
  writeValueArray(&AS_ARRAY(receiver)->elements, args[0]);
  *result = NIL_VAL;
  return true;
}

// array.pop() -> any : remove and return the last element (error if empty).
static bool arrayPop(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  ValueArray *e = &AS_ARRAY(receiver)->elements;
  if (e->count == 0) {
    runtimeError("pop() from an empty array");
    return false;
  }
  *result = e->values[--e->count];
  return true;
}

static Method arrayMethods[] = {
    {"len", 0, arrayLen},
    {"push", 1, arrayPush},
    {"pop", 0, arrayPop},
    {NULL, 0, NULL},
};

// Find the method table for a receiver's type, plus a human-readable type name
// for error messages. Returns NULL if the type has no methods.
static Method *methodsFor(Value receiver, const char **typeName) {
  if (IS_STRING(receiver)) {
    *typeName = "str";
    return stringMethods;
  }
  if (IS_ARRAY(receiver)) {
    *typeName = "array";
    return arrayMethods;
  }
  *typeName = NULL;
  return NULL;
}

bool invokeMethod(Value receiver, ObjString *name, int argCount, Value *args,
                  Value *result) {
  const char *typeName = NULL;
  Method *methods = methodsFor(receiver, &typeName);
  if (methods != NULL) {
    for (Method *m = methods; m->name != NULL; m++) {
      if (nameIs(name, m->name)) {
        if (m->arity != argCount) {
          runtimeError("method '%s' expects %d arguments but got %d", m->name,
                       m->arity, argCount);
          return false;
        }
        return m->fn(receiver, argCount, args, result);
      }
    }
    runtimeError("%s has no method '%s'", typeName, name->chars);
    return false;
  }
  runtimeError("only built-in types have methods (cannot call '%s' here)",
               name->chars);
  return false;
}
