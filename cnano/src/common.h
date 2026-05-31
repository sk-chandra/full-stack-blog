// common.h — shared includes and types used across the whole compiler.
//
// In a C project it is conventional to have one small header that every other
// translation unit can include to get the basic toolbox (sized integers,
// booleans, the standard library). Keeping it tiny avoids slow compiles.
#ifndef CNANO_COMMON_H
#define CNANO_COMMON_H

#include <stdbool.h> // bool, true, false
#include <stddef.h>  // size_t, NULL
#include <stdint.h>  // int64_t, uint8_t — sized integer types

// The single numeric type cnano understands in this first slice: a 64-bit
// signed integer. Giving it a name (Value) means that when we later add floats,
// booleans, or pointers we change ONE typedef and a handful of helpers instead
// of hunting down every `int64_t` in the codebase. This indirection is a core
// language-design lesson: name the concept, not the representation.
typedef int64_t Value;

#endif // CNANO_COMMON_H
