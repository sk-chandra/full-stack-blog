// builtins.h — the built-in (native) functions and methods.
//
// Two kinds of builtin live here, both implemented in C:
//   * FREE functions registered as globals: clock(), str(x). They are ordinary
//     values, looked up by name and called like any function.
//   * METHODS dispatched by OP_INVOKE on a receiver's type: "abc".len(). cnano
//     has no user-defined types, so methods exist only on the built-in types
//     (strings now; arrays and maps in later steps). A small per-type table maps
//     a method name to its C implementation.
// Keeping all of this in one module keeps the VM's dispatch code tiny — it just
// calls into here.
#ifndef CNANO_BUILTINS_H
#define CNANO_BUILTINS_H

#include "value.h"

typedef struct ObjString ObjString;

// Register the free builtin functions as globals. Called once from initVM.
void defineBuiltins(void);

// Dispatch `receiver.name(args)` to a builtin method for the receiver's type.
// On success stores the result through `*result` and returns true; on a missing
// method or a bad call it reports a runtime error and returns false. `args`
// points at the first argument on the VM stack (there are `argCount` of them).
bool invokeMethod(Value receiver, ObjString *name, int argCount, Value *args,
                  Value *result);

#endif // CNANO_BUILTINS_H
