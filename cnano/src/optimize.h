// optimize.h — AST-level optimisation passes.
//
// Optimisations run AFTER type-checking and BEFORE compilation. Working on the
// AST (rather than bytecode) keeps them simple: we rewrite the tree into an
// equivalent but cheaper one, and the existing compiler turns the result into
// bytecode unchanged. The classic starter optimisation lives here: constant
// folding.
#ifndef CNANO_OPTIMIZE_H
#define CNANO_OPTIMIZE_H

#include "ast.h"

// CONSTANT FOLDING: evaluate constant subexpressions at COMPILE time so the VM
// never has to. `2 + 3 * 4` becomes the literal `14`; `!true` becomes `false`;
// `"a" + "b"` becomes `"ab"`. This both shrinks the bytecode and speeds
// execution, at zero runtime cost — the work happens once, here.
//
// Mutates `program` in place (replacing folded subtrees, freeing the originals).
// Returns the number of subexpressions folded, purely for reporting/curiosity.
int foldConstants(Program *program);

#endif // CNANO_OPTIMIZE_H
