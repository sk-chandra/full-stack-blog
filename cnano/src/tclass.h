// tclass.h — the IR type-class analysis, shared by the native backends.
//
// cnano values are dynamically TAGGED; machine code is not — an integer add, a
// float add, and a boolean test are different instructions on different
// registers. This module classifies every temporary, variable, and function
// return in an IR module as INT, BOOL, or FLOAT by a module-wide fixpoint
// (classes flow through stores→loads, call arguments→parameters, and
// returns→call results). A value that would need a runtime tag — an int on one
// path, a float on another — becomes CONFLICT, which the backends reject rather
// than miscompile. (Step 78; factored out in step 80 so the direct-ELF backend
// shares it with the assembly backend.)
#ifndef CNANO_TCLASS_H
#define CNANO_TCLASS_H

#include "ir.h"

typedef enum { TC_UNKNOWN = 0, TC_INT, TC_BOOL, TC_FLOAT, TC_CONFLICT } TClass;

// Everything the classifier learned about one function.
typedef struct {
  ObjString **vars; // the variables (params first), each owning a stack slot
  int varCount;
  TClass *varClass;  // per variable
  TClass *tempClass; // per temporary
  TClass retClass;   // join of every `return` value's class
} FnInfo;

// Classify the whole module to a fixpoint. Returns one FnInfo per function
// (parallel to m->funcs); free with freeModuleClasses.
FnInfo *moduleClassify(IRModule *m);
void freeModuleClasses(IRModule *m, FnInfo *infos);

// Collect the distinct variables of `fn` that need a stack slot: parameters
// first, then every variable loaded or stored in the body.
ObjString **collectVars(IRFunc *fn, int *outCount);

// The index of the function named `name` within the module, or -1 (unknown —
// e.g. a builtin, which the native backends cannot call).
int findFuncIndex(IRModule *m, ObjString *name);

// The index of `name` in an FnInfo's variable list, or -1.
int varIndexIn(ObjString *name, FnInfo *info);

// Node-op groupings the backends share.
bool tcIsComparison(NodeOp op);
bool tcIsArith(NodeOp op);

#endif // CNANO_TCLASS_H
