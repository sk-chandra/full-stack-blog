// value.h — the "constant pool" and value helpers.
//
// Bytecode instructions are tiny (one byte for the opcode). But programs contain
// big literals like 1000000. We cannot stuff a 64-bit number into a one-byte
// instruction. The classic solution every real VM uses (CPython, the JVM, Lua):
// store literals once in a side table — the *constant pool* — and let the
// instruction carry a small *index* into that table. ValueArray is that pool.
#ifndef CNANO_VALUE_H
#define CNANO_VALUE_H

#include "common.h"

// A growable array of Values. This is the textbook dynamic-array pattern:
// keep a `count` of live elements and a `capacity` of allocated slots; when
// they meet, double the capacity. Amortised O(1) appends.
typedef struct {
  int count;
  int capacity;
  Value *values;
} ValueArray;

void initValueArray(ValueArray *array);
void freeValueArray(ValueArray *array);
// Appends `value` and returns the index it landed at — that index is what the
// bytecode will reference.
int writeValueArray(ValueArray *array, Value value);
void printValue(Value value);

#endif // CNANO_VALUE_H
