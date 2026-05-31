#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "builtins.h"
#include "object.h"
#include "table.h"
#include "vm.h" // vm.globals, runtimeError

// Find `needle` (nlen bytes) inside `hay` (hlen bytes); return its start index or
// -1. A plain byte search (not strstr) so embedded NULs and explicit lengths work.
static int findSub(const char *hay, int hlen, const char *needle, int nlen) {
  if (nlen == 0)
    return 0;
  for (int i = 0; i + nlen <= hlen; i++)
    if (memcmp(hay + i, needle, nlen) == 0)
      return i;
  return -1;
}

// cnano's truthiness, duplicated from the VM for assert() (nil/false are falsey).
static bool isFalseyValue(Value v) {
  return IS_NIL(v) || (IS_BOOL(v) && !AS_BOOL(v));
}

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
  else if (IS_FLOAT(v))
    len = formatFloat(buf, sizeof(buf), AS_FLOAT(v)); // matches printValue
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

// $for_iter(coll) -> array : the sequence a `for (let x in coll)` loop walks.
// Arrays iterate their elements (the array itself); maps iterate their keys. This
// hidden builtin lets for-in desugar to one index loop regardless of the
// collection's (possibly dynamic) type. Its name is unlexable ($), so user code
// can neither call nor shadow it.
static bool forIterNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  Value coll = args[0];
  if (IS_ARRAY(coll)) {
    *result = coll; // iterate the elements directly
    return true;
  }
  if (IS_MAP(coll)) {
    ObjMap *map = AS_MAP(coll);
    ObjArray *keys = newArrayObject(); // coll is rooted on the stack; GC-safe
    for (int i = 0; i < map->capacity; i++)
      if (map->entries[i].state == MAP_OCCUPIED)
        writeValueArray(&keys->elements, map->entries[i].key);
    *result = OBJ_VAL(keys);
    return true;
  }
  runtimeError("can only iterate over arrays and maps");
  return false;
}

// len(x) -> int : the length of a string, array, or map (free-function form of
// the .len() method, which many languages also provide).
static bool lenNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  Value v = args[0];
  if (IS_STRING(v))
    *result = INT_VAL(AS_STRING(v)->length);
  else if (IS_ARRAY(v))
    *result = INT_VAL(AS_ARRAY(v)->elements.count);
  else if (IS_MAP(v))
    *result = INT_VAL(AS_MAP(v)->count);
  else {
    runtimeError("len() expects a string, array, or map");
    return false;
  }
  return true;
}

// type(x) -> str : the runtime type name of any value.
static bool typeNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  Value v = args[0];
  const char *name = IS_INT(v)     ? "int"
                     : IS_FLOAT(v) ? "float"
                     : IS_BOOL(v)  ? "bool"
                     : IS_NIL(v)   ? "nil"
                     : IS_STRING(v) ? "str"
                     : IS_ARRAY(v) ? "array"
                     : IS_MAP(v)   ? "map"
                                   : "fn"; // closures and natives
  *result = OBJ_VAL(copyString(name, (int)strlen(name)));
  return true;
}

// assert(cond) -> nil : abort with a runtime error if cond is falsey.
static bool assertNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  if (isFalseyValue(args[0])) {
    runtimeError("assertion failed");
    return false;
  }
  *result = NIL_VAL;
  return true;
}

static bool absNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  if (IS_INT(args[0])) {
    int64_t n = AS_INT(args[0]);
    *result = INT_VAL(n < 0 ? -n : n);
  } else if (IS_FLOAT(args[0])) {
    double d = AS_FLOAT(args[0]);
    *result = FLOAT_VAL(d < 0 ? -d : d);
  } else {
    runtimeError("abs() expects a number");
    return false;
  }
  return true;
}

// min/max keep the ORIGINAL value (preserving int vs float), comparing as numbers.
static bool minNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  if (!IS_NUM(args[0]) || !IS_NUM(args[1])) {
    runtimeError("min() expects two numbers");
    return false;
  }
  *result = AS_NUM(args[0]) <= AS_NUM(args[1]) ? args[0] : args[1];
  return true;
}

static bool maxNative(int argCount, Value *args, Value *result) {
  (void)argCount;
  if (!IS_NUM(args[0]) || !IS_NUM(args[1])) {
    runtimeError("max() expects two numbers");
    return false;
  }
  *result = AS_NUM(args[0]) >= AS_NUM(args[1]) ? args[0] : args[1];
  return true;
}

// Float math, all returning a float. sqrt/floor/ceil/round take one number;
// pow takes two. They accept ints (promoted) and yield floats.
static bool mathUnary(Value *args, Value *result, double (*fn)(double),
                      const char *name) {
  if (!IS_NUM(args[0])) {
    runtimeError("%s() expects a number", name);
    return false;
  }
  *result = FLOAT_VAL(fn(AS_NUM(args[0])));
  return true;
}
static bool sqrtNative(int a, Value *args, Value *r) { (void)a; return mathUnary(args, r, sqrt, "sqrt"); }
static bool floorNative(int a, Value *args, Value *r) { (void)a; return mathUnary(args, r, floor, "floor"); }
static bool ceilNative(int a, Value *args, Value *r) { (void)a; return mathUnary(args, r, ceil, "ceil"); }
static bool roundNative(int a, Value *args, Value *r) { (void)a; return mathUnary(args, r, round, "round"); }
static bool powNative(int a, Value *args, Value *r) {
  (void)a;
  if (!IS_NUM(args[0]) || !IS_NUM(args[1])) {
    runtimeError("pow() expects two numbers");
    return false;
  }
  *r = FLOAT_VAL(pow(AS_NUM(args[0]), AS_NUM(args[1])));
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
      {"len", lenNative, 1},
      {"type", typeNative, 1},
      {"assert", assertNative, 1},
      {"abs", absNative, 1},
      {"min", minNative, 2},
      {"max", maxNative, 2},
      {"sqrt", sqrtNative, 1},
      {"floor", floorNative, 1},
      {"ceil", ceilNative, 1},
      {"round", roundNative, 1},
      {"pow", powNative, 2},
      {"$for_iter", forIterNative, 1}, // internal: backs for-in (unlexable name)
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

// Build a new string by applying a per-char transform (toupper/tolower). The temp
// buffer is plain malloc (not a GC object); the receiver stays rooted on the stack.
static bool strMapCase(Value receiver, Value *result, int (*fn)(int)) {
  ObjString *s = AS_STRING(receiver);
  char *buf = malloc(s->length + 1);
  if (buf == NULL) {
    runtimeError("out of memory");
    return false;
  }
  for (int i = 0; i < s->length; i++)
    buf[i] = (char)fn((unsigned char)s->chars[i]);
  *result = OBJ_VAL(copyString(buf, s->length));
  free(buf);
  return true;
}
static bool strUpper(Value r, int a, Value *args, Value *out) {
  (void)a; (void)args; return strMapCase(r, out, toupper);
}
static bool strLower(Value r, int a, Value *args, Value *out) {
  (void)a; (void)args; return strMapCase(r, out, tolower);
}

// "abc".contains("b") -> bool ; "abc".indexOf("b") -> int (-1 if absent).
static bool strContains(Value r, int a, Value *args, Value *out) {
  (void)a;
  if (!IS_STRING(args[0])) { runtimeError("contains() expects a string"); return false; }
  ObjString *s = AS_STRING(r), *sub = AS_STRING(args[0]);
  *out = BOOL_VAL(findSub(s->chars, s->length, sub->chars, sub->length) >= 0);
  return true;
}
static bool strIndexOf(Value r, int a, Value *args, Value *out) {
  (void)a;
  if (!IS_STRING(args[0])) { runtimeError("indexOf() expects a string"); return false; }
  ObjString *s = AS_STRING(r), *sub = AS_STRING(args[0]);
  *out = INT_VAL(findSub(s->chars, s->length, sub->chars, sub->length));
  return true;
}

// "hello".substring(start, end) -> str : the half-open slice [start, end).
static bool strSubstring(Value r, int a, Value *args, Value *out) {
  (void)a;
  if (!IS_INT(args[0]) || !IS_INT(args[1])) {
    runtimeError("substring() expects two int indices");
    return false;
  }
  ObjString *s = AS_STRING(r);
  int64_t start = AS_INT(args[0]), end = AS_INT(args[1]);
  if (start < 0 || end > s->length || start > end) {
    runtimeError("substring(%lld, %lld) out of range (length %d)",
                 (long long)start, (long long)end, s->length);
    return false;
  }
  *out = OBJ_VAL(copyString(s->chars + start, (int)(end - start)));
  return true;
}

static Method stringMethods[] = {
    {"len", 0, strLen},
    {"upper", 0, strUpper},
    {"lower", 0, strLower},
    {"contains", 1, strContains},
    {"indexOf", 1, strIndexOf},
    {"substring", 2, strSubstring},
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

// array.removeAt(i) -> any : remove and return element i, shifting the rest down.
static bool arrayRemoveAt(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  if (!IS_INT(args[0])) {
    runtimeError("removeAt() expects an int index");
    return false;
  }
  ValueArray *e = &AS_ARRAY(receiver)->elements;
  int64_t i = AS_INT(args[0]);
  if (i < 0 || i >= e->count) {
    runtimeError("removeAt index %lld out of range (length %d)", (long long)i,
                 e->count);
    return false;
  }
  *result = e->values[i];
  for (int j = (int)i; j < e->count - 1; j++)
    e->values[j] = e->values[j + 1];
  e->count--;
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

// [1,2,3].contains(2) -> bool ; .indexOf(2) -> int (-1 if absent). Uses the
// language's own value equality, so it works for any element type.
static bool arrayContains(Value r, int a, Value *args, Value *out) {
  (void)a;
  ValueArray *e = &AS_ARRAY(r)->elements;
  for (int i = 0; i < e->count; i++)
    if (valuesEqual(e->values[i], args[0])) { *out = BOOL_VAL(true); return true; }
  *out = BOOL_VAL(false);
  return true;
}
static bool arrayIndexOf(Value r, int a, Value *args, Value *out) {
  (void)a;
  ValueArray *e = &AS_ARRAY(r)->elements;
  for (int i = 0; i < e->count; i++)
    if (valuesEqual(e->values[i], args[0])) { *out = INT_VAL(i); return true; }
  *out = INT_VAL(-1);
  return true;
}

// Grow a heap byte buffer and append `n` bytes — used to assemble join() without
// ever holding an unrooted intermediate string across an allocation.
static void appendBytes(char **buf, int *len, int *cap, const char *src, int n) {
  if (*len + n > *cap) {
    while (*len + n > *cap)
      *cap = *cap < 16 ? 16 : *cap * 2;
    *buf = realloc(*buf, *cap);
    if (*buf == NULL) { fprintf(stderr, "cnano: out of memory in join()\n"); exit(70); }
  }
  memcpy(*buf + *len, src, n);
  *len += n;
}

// [a,b,c].join(sep) -> str : string each element and concatenate with `sep`.
static bool arrayJoin(Value r, int a, Value *args, Value *out) {
  (void)a;
  if (!IS_STRING(args[0])) { runtimeError("join() separator must be a string"); return false; }
  ObjString *sep = AS_STRING(args[0]);
  ValueArray *e = &AS_ARRAY(r)->elements;
  char *buf = NULL;
  int len = 0, cap = 0;
  for (int i = 0; i < e->count; i++) {
    if (i > 0)
      appendBytes(&buf, &len, &cap, sep->chars, sep->length);
    // Copy each element's bytes immediately; don't retain the ObjString* across
    // the next stringify (which may allocate and trigger a collection).
    ObjString *s = stringify(e->values[i]);
    appendBytes(&buf, &len, &cap, s->chars, s->length);
  }
  *out = OBJ_VAL(copyString(buf ? buf : "", len));
  free(buf);
  return true;
}

// qsort comparator: numeric for ints, lexicographic for strings (arrSort has
// already verified the array is homogeneous, so only one branch ever fires).
static int compareValues(const void *pa, const void *pb) {
  Value a = *(const Value *)pa, b = *(const Value *)pb;
  if (IS_INT(a) && IS_INT(b))
    return (AS_INT(a) > AS_INT(b)) - (AS_INT(a) < AS_INT(b));
  ObjString *x = AS_STRING(a), *y = AS_STRING(b);
  int n = x->length < y->length ? x->length : y->length;
  int c = memcmp(x->chars, y->chars, n);
  return c != 0 ? c : (x->length > y->length) - (x->length < y->length);
}

// [3,1,2].sort() -> nil : sort IN PLACE, ascending. The array must be all ints or
// all strings (mixed types have no obvious order, so that is a clean error).
static bool arraySort(Value r, int a, Value *args, Value *out) {
  (void)a; (void)args;
  ValueArray *e = &AS_ARRAY(r)->elements;
  bool allInt = true, allStr = true;
  for (int i = 0; i < e->count; i++) {
    if (!IS_INT(e->values[i])) allInt = false;
    if (!IS_STRING(e->values[i])) allStr = false;
  }
  if (e->count > 1 && !allInt && !allStr) {
    runtimeError("sort() needs an array of all ints or all strings");
    return false;
  }
  qsort(e->values, e->count, sizeof(Value), compareValues);
  *out = NIL_VAL;
  return true;
}

// Higher-order methods call a cnano function back from C (callFromVM). The killer
// GC subtlety: a callback can allocate and trigger a collection, so any value we
// are accumulating must be reachable from a GC root. We therefore push the result
// array (or accumulator) onto the VM stack for the duration and pop it at the end
// — the same discipline the VM itself uses. The receiver array stays rooted too
// (it is still on the stack as OP_INVOKE's receiver), so reading its elements is
// safe even though a callback might mutate it.

// [1,2,3].map(fn) -> array : a new array of fn(element) for each element.
static bool arrayMap(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  Value fn = args[0];
  ObjArray *out = newArrayObject();
  push(OBJ_VAL(out)); // root the result across callbacks
  for (int i = 0; i < AS_ARRAY(receiver)->elements.count; i++) {
    Value elem = AS_ARRAY(receiver)->elements.values[i];
    Value mapped;
    if (!callFromVM(fn, &elem, 1, &mapped)) {
      pop();
      return false;
    }
    writeValueArray(&out->elements, mapped);
  }
  pop(); // unroot
  *result = OBJ_VAL(out);
  return true;
}

// [1,2,3].filter(fn) -> array : the elements for which fn(element) is truthy.
static bool arrayFilter(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  Value fn = args[0];
  ObjArray *out = newArrayObject();
  push(OBJ_VAL(out));
  for (int i = 0; i < AS_ARRAY(receiver)->elements.count; i++) {
    Value elem = AS_ARRAY(receiver)->elements.values[i];
    Value keep;
    if (!callFromVM(fn, &elem, 1, &keep)) {
      pop();
      return false;
    }
    if (!isFalseyValue(keep))
      writeValueArray(&out->elements, elem);
  }
  pop();
  *result = OBJ_VAL(out);
  return true;
}

// [1,2,3].reduce(fn, init) -> any : fold left, acc = fn(acc, element).
static bool arrayReduce(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  Value fn = args[0];
  Value acc = args[1];
  push(acc); // root the running accumulator across callbacks
  for (int i = 0; i < AS_ARRAY(receiver)->elements.count; i++) {
    Value callArgs[2] = {acc, AS_ARRAY(receiver)->elements.values[i]};
    if (!callFromVM(fn, callArgs, 2, &acc)) {
      pop();
      return false;
    }
    vm.stackTop[-1] = acc; // keep the rooted slot in sync with the new accumulator
  }
  pop();
  *result = acc;
  return true;
}

static Method arrayMethods[] = {
    {"len", 0, arrayLen},
    {"push", 1, arrayPush},
    {"pop", 0, arrayPop},
    {"contains", 1, arrayContains},
    {"indexOf", 1, arrayIndexOf},
    {"join", 1, arrayJoin},
    {"sort", 0, arraySort},
    {"map", 1, arrayMap},
    {"filter", 1, arrayFilter},
    {"reduce", 2, arrayReduce},
    {"removeAt", 1, arrayRemoveAt},
    {NULL, 0, NULL},
};

// map.len() -> int : the number of entries.
static bool mapLen(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  *result = INT_VAL(AS_MAP(receiver)->count);
  return true;
}

// map.has(key) -> bool : whether `key` is present. A non-hashable key can never
// be present, so it answers false rather than erroring.
static bool mapHas(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  Value ignored;
  bool present =
      isHashableKey(args[0]) && mapGet(AS_MAP(receiver), args[0], &ignored);
  *result = BOOL_VAL(present);
  return true;
}

// map.keys() -> array : a new array of the map's keys (in bucket order).
static bool mapKeys(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  ObjMap *map = AS_MAP(receiver);
  // GC-safe: the map (receiver) is on the stack; newArrayObject may collect but
  // sees it, and writeValueArray never collects.
  ObjArray *keys = newArrayObject();
  for (int i = 0; i < map->capacity; i++)
    if (map->entries[i].state == MAP_OCCUPIED)
      writeValueArray(&keys->elements, map->entries[i].key);
  *result = OBJ_VAL(keys);
  return true;
}

// map.values() -> array : a new array of the map's values (bucket order).
static bool mapValues(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  ObjMap *map = AS_MAP(receiver);
  ObjArray *values = newArrayObject(); // receiver rooted; GC-safe
  for (int i = 0; i < map->capacity; i++)
    if (map->entries[i].state == MAP_OCCUPIED)
      writeValueArray(&values->elements, map->entries[i].value);
  *result = OBJ_VAL(values);
  return true;
}

// map.remove(key) -> bool : delete an entry; true if it was present.
static bool mapRemove(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  *result = BOOL_VAL(isHashableKey(args[0]) &&
                     mapDelete(AS_MAP(receiver), args[0]));
  return true;
}

static Method mapMethods[] = {
    {"len", 0, mapLen},
    {"has", 1, mapHas},
    {"keys", 0, mapKeys},
    {"values", 0, mapValues},
    {"remove", 1, mapRemove},
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
  if (IS_MAP(receiver)) {
    *typeName = "map";
    return mapMethods;
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
