// ir.h — a small three-address intermediate representation.
//
// This is the representation cnano's bytecode lacks: every value has a NAME (a
// temporary `t0, t1, …`), so the classic optimizations — constant propagation,
// common-subexpression elimination, dead-code elimination — become simple walks
// instead of fighting the operand stack (see the GUIDE's middle-end chapter for
// why stack bytecode resists them). It is a teaching/analysis layer: `cnano --ir`
// lowers straight-line code to IR and shows it before and after optimisation.
// (Wiring it in as the execution backend is the natural next project.)
#ifndef CNANO_IR_H
#define CNANO_IR_H

#include "ast.h"
#include "value.h"

typedef enum {
  IR_CONST,  // dest = <constant>
  IR_LOAD,   // dest = load <var>
  IR_STORE,  // store <var> = a            (an effect; keeps the value too)
  IR_UNARY,  // dest = <op> a
  IR_BINARY, // dest = a <op> b
  IR_PRINT,  // print a                    (an effect)
  // --- control flow (step 70): turn the IR from straight-line into a real CFG --
  IR_LABEL,         // L<a>:                     a jump TARGET (id in `a`)
  IR_JUMP,          // goto L<a>                 unconditional branch
  IR_JUMP_IF_FALSE, // if !t<a> goto L<b>        branch when the bool temp is false
} IROp;

typedef struct {
  IROp op;
  int dest;          // temp id this defines, or -1 for an effect with no value
  int a, b;          // operand temp ids (-1 when unused); also a label id for
                     // IR_LABEL/IR_JUMP (in a) and IR_JUMP_IF_FALSE (target in b)
  Value constant;    // IR_CONST
  ObjString *var;    // IR_LOAD / IR_STORE
  NodeOp nodeOp;     // IR_UNARY / IR_BINARY
  bool dead;         // set by dead-code elimination; skipped when printing
} IRInstr;

typedef struct {
  IRInstr *code;
  int count, capacity;
  int nextTemp;      // next fresh temporary id
  int nextLabel;     // next fresh label id (control flow)
  const char *name;  // function name (for the header)
} IRFunc;

// Lower a straight-line statement list into IR. Returns NULL if `body` contains
// anything outside the supported subset (control flow, calls, collections, …) —
// the IR demo is about local optimisation of straight-line scalar code.
IRFunc *lowerToIR(Program *body, const char *name);
void freeIR(IRFunc *fn);
void printIR(IRFunc *fn, const char *title);

// Optimise the IR in place — constant propagation + folding (step 64b), then
// CSE + dead-temp elimination (step 65). Prints the IR before ("lowered") and
// after ("optimised") so `--ir` shows the transformation.
void optimizeIR(IRFunc *fn);

// The same passes, without the before/after printing — for the x86-64 backend.
void optimizeIRPasses(IRFunc *fn);

#endif // CNANO_IR_H
