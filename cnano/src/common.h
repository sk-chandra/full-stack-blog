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

// NOTE: the `Value` type used to live here as `typedef int64_t Value`. Now that
// cnano has more than one runtime type (integers, booleans, nil), a value must
// carry BOTH a payload and a tag saying what it is. That richer definition lives
// in value.h as a tagged union. This file is now just the shared toolbox.

#endif // CNANO_COMMON_H
