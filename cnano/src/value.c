#include <stdio.h>
#include <stdlib.h>

#include "object.h"
#include "value.h"

int formatFloat(char *buf, int size, double v) {
  // %g is compact, but prints whole values without a point (3.0 -> "3"); append
  // ".0" so a float always reads as a float and never looks like an int.
  int n = snprintf(buf, size, "%g", v);
  bool looksFloat = false;
  for (int i = 0; i < n; i++)
    if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E' || buf[i] == 'n' ||
        buf[i] == 'i') { // '.', exponent, nan, inf
      looksFloat = true;
      break;
    }
  if (!looksFloat && n + 2 < size) {
    buf[n++] = '.';
    buf[n++] = '0';
    buf[n] = '\0';
  }
  return n;
}

void initValueArray(ValueArray *array) {
  array->values = NULL;
  array->capacity = 0;
  array->count = 0;
}

void freeValueArray(ValueArray *array) {
  free(array->values);
  initValueArray(array); // leave the struct in a safe, reusable state
}

int writeValueArray(ValueArray *array, Value value) {
  // Grow when full. Doubling (with a small floor of 8) keeps the *total* cost of
  // N appends linear, even though individual appends occasionally copy.
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
    array->values = realloc(array->values, sizeof(Value) * array->capacity);
    if (array->values == NULL) {
      fprintf(stderr, "cnano: out of memory growing constant pool\n");
      exit(70); // 70 = EX_SOFTWARE, a conventional "internal error" exit code
    }
  }
  array->values[array->count] = value;
  return array->count++; // return index, THEN increment
}

void printValue(Value value) {
  // Printing must now dispatch on the tag: a value no longer has a single
  // textual form. This switch is the value-level mirror of the VM's opcode
  // switch — both are "decode the tag, act accordingly".
  switch (value.type) {
  case VAL_NIL:
    printf("nil");
    break;
  case VAL_BOOL:
    printf(AS_BOOL(value) ? "true" : "false");
    break;
  case VAL_INT:
    // PRId64 would be the fully portable way; %lld after a cast is simpler.
    printf("%lld", (long long)AS_INT(value));
    break;
  case VAL_FLOAT: {
    char buf[32];
    formatFloat(buf, sizeof(buf), AS_FLOAT(value));
    printf("%s", buf);
    break;
  }
  case VAL_OBJ:
    printObject(value); // dispatch to the heap-object printer
    break;
  }
}

bool valuesEqual(Value a, Value b) {
  // NUMBERS are the one cross-type exception: `1 == 1.0` is true (int and float
  // compare numerically). Everything else: different types are never equal —
  // `1 == true` is false, no coercion. A deliberate language-design stance.
  if (IS_NUM(a) && IS_NUM(b)) {
    if (IS_INT(a) && IS_INT(b))
      return AS_INT(a) == AS_INT(b); // exact (avoids huge-int precision loss)
    return AS_NUM(a) == AS_NUM(b);
  }
  if (a.type != b.type)
    return false;
  switch (a.type) {
  case VAL_NIL:
    return true; // nil == nil
  case VAL_BOOL:
    return AS_BOOL(a) == AS_BOOL(b);
  case VAL_INT:
    return AS_INT(a) == AS_INT(b);
  case VAL_OBJ:
    // Thanks to interning, equal strings are the SAME object, so pointer
    // equality is correct AND fast — no byte-by-byte comparison needed.
    return AS_OBJ(a) == AS_OBJ(b);
  default:
    return false; // unreachable
  }
}
