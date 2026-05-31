// typecheck.h — the static (gradual) type-checking pass.
//
// This pass runs AFTER parsing and BEFORE compilation. It walks the AST,
// computes a Type for every expression, and reports errors for provably-wrong
// operations (e.g. `true + 1`, calling a non-function, wrong argument types) —
// all WITHOUT running the program. This is your first taste of *static analysis*:
// reasoning about a program's behaviour from its structure alone.
//
// It is GRADUAL: the `any` type is compatible with everything, and anything left
// unannotated is `any`, so existing untyped programs type-check trivially and run
// exactly as before. You opt into safety with annotations. Types are then
// ERASED — the compiler and VM are unchanged and remain dynamically typed.
#ifndef CNANO_TYPECHECK_H
#define CNANO_TYPECHECK_H

#include "ast.h"

// Type-check a whole program. Returns true if it is well-typed (no errors).
// Prints a message per error. A false result means: do not compile/run.
bool typecheckProgram(Program *program);

#endif // CNANO_TYPECHECK_H
