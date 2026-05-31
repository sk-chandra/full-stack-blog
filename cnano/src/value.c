#include <stdio.h>
#include <stdlib.h>

#include "value.h"

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
  // PRId64 would be the fully portable way; %lld after a cast is simpler to read.
  printf("%lld", (long long)value);
}
