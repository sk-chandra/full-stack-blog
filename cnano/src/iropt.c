// iropt.c — constant propagation + folding on the three-address IR (step 64b).
//
// This is the payoff of having an IR. On stack bytecode these analyses fought
// the operand stack (see the GUIDE's middle-end chapter); here every value has a
// name (`t0, t1, …`) and every variable's current value is just the last thing
// stored to it (the code is straight-line, so the most recent store dominates
// every later load). That makes the two classic passes almost trivial:
//
//   - CONSTANT PROPAGATION: replace `load x` with the constant last stored to x,
//     and feed the constant onward through the temporaries that use it.
//   - CONSTANT FOLDING: when both operands of an op are known constants, compute
//     the result at compile time and replace the op with a `const`.
//
// The golden rule is SOUNDNESS: fold ONLY when the result is exactly what the VM
// would compute at runtime. So we mirror vm.c's arithmetic precisely (int/float
// promotion, the truthiness rule, value equality) and REFUSE to fold anything
// the VM would turn into a runtime error — division/modulo by zero, an
// out-of-range shift, a type mismatch — leaving that instruction intact so the
// error still happens, at runtime, exactly as before.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ir.h"
#include "object.h" // ObjString, valuesEqual, printValue

// cnano's truthiness rule, copied from vm.c so `!` folds the same way the VM
// evaluates it: only nil and false are falsey (every integer, including 0, is
// truthy).
static bool irFalsey(Value v) {
  return IS_NIL(v) || (IS_BOOL(v) && !AS_BOOL(v));
}

// Fold a unary op over a known-constant operand. Returns false (don't fold) when
// the operand's type is wrong for the op — the VM would raise at runtime.
static bool foldUnary(NodeOp op, Value a, Value *out) {
  switch (op) {
  case OP_NODE_NEGATE:
    if (IS_INT(a)) { *out = INT_VAL(-AS_INT(a)); return true; }
    if (IS_FLOAT(a)) { *out = FLOAT_VAL(-AS_FLOAT(a)); return true; }
    return false;
  case OP_NODE_NOT: // defined for every value via the truthiness rule
    *out = BOOL_VAL(irFalsey(a));
    return true;
  case OP_NODE_BITNOT:
    if (IS_INT(a)) { *out = INT_VAL(~AS_INT(a)); return true; }
    return false;
  default:
    return false;
  }
}

// Fold a binary op over two known-constant operands, mirroring vm.c exactly.
// Returns false when folding would change behaviour: a type mismatch, or a case
// the VM raises (÷0, %0, shift ∉ [0,63]) — those must stay to fail at runtime.
static bool foldBinary(NodeOp op, Value a, Value b, Value *out) {
  switch (op) {
  case OP_NODE_ADD:
  case OP_NODE_SUB:
  case OP_NODE_MUL: {
    // NB: string `+` never reaches the IR (string literals aren't lowered), so
    // this is purely numeric, with int->float promotion like NUM_ARITH.
    if (!IS_NUM(a) || !IS_NUM(b))
      return false;
    if (IS_INT(a) && IS_INT(b)) {
      int64_t x = AS_INT(a), y = AS_INT(b);
      *out = INT_VAL(op == OP_NODE_ADD ? x + y : op == OP_NODE_SUB ? x - y : x * y);
    } else {
      double x = AS_NUM(a), y = AS_NUM(b);
      *out = FLOAT_VAL(op == OP_NODE_ADD ? x + y : op == OP_NODE_SUB ? x - y : x * y);
    }
    return true;
  }
  case OP_NODE_DIV:
    if (!IS_NUM(a) || !IS_NUM(b))
      return false;
    if (IS_INT(a) && IS_INT(b)) {
      if (AS_INT(b) == 0)
        return false; // leave it: the VM raises "division by zero"
      *out = INT_VAL(AS_INT(a) / AS_INT(b));
    } else {
      // Float division is IEEE-defined even by zero (inf/nan), so always foldable.
      *out = FLOAT_VAL(AS_NUM(a) / AS_NUM(b));
    }
    return true;
  case OP_NODE_MOD:
    if (!IS_INT(a) || !IS_INT(b))
      return false;
    if (AS_INT(b) == 0)
      return false; // leave it: the VM raises "modulo by zero"
    *out = INT_VAL(AS_INT(a) % AS_INT(b));
    return true;
  case OP_NODE_BITAND:
  case OP_NODE_BITOR:
  case OP_NODE_BITXOR: {
    if (!IS_INT(a) || !IS_INT(b))
      return false;
    int64_t x = AS_INT(a), y = AS_INT(b);
    *out = INT_VAL(op == OP_NODE_BITAND ? x & y
                   : op == OP_NODE_BITOR ? x | y
                                         : x ^ y);
    return true;
  }
  case OP_NODE_SHL:
  case OP_NODE_SHR: {
    if (!IS_INT(a) || !IS_INT(b))
      return false;
    if (AS_INT(b) < 0 || AS_INT(b) > 63)
      return false; // leave it: the VM raises "shift amount must be 0..63"
    int64_t x = AS_INT(a), y = AS_INT(b);
    *out = INT_VAL(op == OP_NODE_SHL ? (int64_t)((uint64_t)x << y) : x >> y);
    return true;
  }
  case OP_NODE_EQUAL: // defined for all types
    *out = BOOL_VAL(valuesEqual(a, b));
    return true;
  case OP_NODE_LESS:
  case OP_NODE_GREATER:
    if (!IS_NUM(a) || !IS_NUM(b))
      return false;
    *out = BOOL_VAL(op == OP_NODE_LESS ? AS_NUM(a) < AS_NUM(b)
                                       : AS_NUM(a) > AS_NUM(b));
    return true;
  default:
    return false;
  }
}

// The current known-constant value of a variable (or "unknown"). Names are
// interned, so identity is a pointer compare.
typedef struct {
  ObjString *var;
  bool known;
  Value val;
} VarConst;

static VarConst *findVar(VarConst *vars, int count, ObjString *name) {
  for (int i = 0; i < count; i++)
    if (vars[i].var == name)
      return &vars[i];
  return NULL;
}

// Rewrite an instruction in place into `dest = const v`, dropping its operands so
// it reads as a pure constant (which also makes it a clean leaf for the step-65
// dead-temp sweep).
static void makeConst(IRInstr *in, Value v) {
  in->op = IR_CONST;
  in->constant = v;
  in->a = in->b = -1;
  in->var = NULL;
}

// --- pass 1: constant propagation + folding (step 64b) ----------------------
// One forward pass: track which temporaries hold known constants and which
// constant each variable last had stored to it, then rewrite loads and fold ops.
static void constPropFold(IRFunc *fn) {
  int n = fn->nextTemp;
  bool *known = n ? calloc(n, sizeof(bool)) : NULL;   // does temp t hold a constant?
  Value *val = n ? calloc(n, sizeof(Value)) : NULL;   // …and which one
  VarConst *vars = NULL;
  int varCount = 0, varCap = 0;

  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    switch (in->op) {
    case IR_CONST:
      if (in->dest >= 0) { known[in->dest] = true; val[in->dest] = in->constant; }
      break;
    case IR_LOAD: {
      VarConst *vc = findVar(vars, varCount, in->var);
      if (vc != NULL && vc->known) {
        makeConst(in, vc->val); // propagate the variable's constant into the load
        known[in->dest] = true;
        val[in->dest] = vc->val;
      } else {
        known[in->dest] = false;
      }
      break;
    }
    case IR_STORE: {
      VarConst *vc = findVar(vars, varCount, in->var);
      if (vc == NULL) {
        if (varCount + 1 > varCap) {
          varCap = varCap < 8 ? 8 : varCap * 2;
          vars = realloc(vars, sizeof(VarConst) * varCap);
        }
        vc = &vars[varCount++];
        vc->var = in->var;
      }
      // The last store wins (straight-line code): record or clear its constant.
      if (in->a >= 0 && known[in->a]) { vc->known = true; vc->val = val[in->a]; }
      else vc->known = false;
      break;
    }
    case IR_UNARY: {
      Value out;
      if (in->a >= 0 && known[in->a] && foldUnary(in->nodeOp, val[in->a], &out)) {
        makeConst(in, out);
        known[in->dest] = true;
        val[in->dest] = out;
      } else if (in->dest >= 0) {
        known[in->dest] = false;
      }
      break;
    }
    case IR_BINARY: {
      Value out;
      if (in->a >= 0 && in->b >= 0 && known[in->a] && known[in->b] &&
          foldBinary(in->nodeOp, val[in->a], val[in->b], &out)) {
        makeConst(in, out);
        known[in->dest] = true;
        val[in->dest] = out;
      } else if (in->dest >= 0) {
        known[in->dest] = false;
      }
      break;
    }
    case IR_PRINT:
    default: // control-flow ops never reach here (guarded by hasControlFlow)
      break;
    }
  }

  free(known);
  free(val);
  free(vars);
}

// --- pass 2: common-subexpression elimination (step 65) ---------------------
// Local value numbering. Each computed value gets an entry keyed by its FORM
// (constant value, loaded variable, or op + operand temps). When a later
// instruction has the same form, it is redundant: we redirect every later use to
// the temp that first computed it (via `rep`) and mark the instruction dead.
//
// The soundness comes for free from the shape of the IR: a temporary is assigned
// EXACTLY ONCE and never changes, so two instructions with identical operand
// temps truly compute the same value. Only VARIABLES are mutable — so a `store`
// invalidates any cached load of that variable, and (store-to-load forwarding)
// records that a following `load` of it equals the stored temp.
typedef enum { VN_CONST, VN_LOAD, VN_UNARY, VN_BINARY } VNKind;
typedef struct {
  VNKind kind;
  bool valid;     // a store can invalidate a cached load
  NodeOp op;      // VN_UNARY / VN_BINARY
  Value value;    // VN_CONST
  ObjString *var; // VN_LOAD
  int a, b;       // operand temps (already canonicalised), -1 if unused
  int temp;       // the temp that first held this value
} VNEntry;

// Follow the representative chain to a temp's canonical name.
static int vnResolve(int *rep, int t) {
  while (t >= 0 && rep[t] != t)
    t = rep[t];
  return t;
}

static void cse(IRFunc *fn) {
  int n = fn->nextTemp;
  int *rep = n ? malloc(sizeof(int) * n) : NULL;
  for (int i = 0; i < n; i++)
    rep[i] = i;
  VNEntry *tab = NULL;
  int cnt = 0, cap = 0;

  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    // Rewrite operands to their canonical temps first, so equal expressions
    // become textually identical and later uses follow the survivor.
    if (in->a >= 0) in->a = vnResolve(rep, in->a);
    if (in->b >= 0) in->b = vnResolve(rep, in->b);

    // Look for an existing entry with the same form.
    int match = -1;
    for (int k = 0; k < cnt && match < 0; k++) {
      VNEntry *e = &tab[k];
      if (!e->valid)
        continue;
      switch (in->op) {
      case IR_CONST:
        if (e->kind == VN_CONST && valuesEqual(e->value, in->constant)) match = e->temp;
        break;
      case IR_LOAD:
        if (e->kind == VN_LOAD && e->var == in->var) match = e->temp;
        break;
      case IR_UNARY:
        if (e->kind == VN_UNARY && e->op == in->nodeOp && e->a == in->a) match = e->temp;
        break;
      case IR_BINARY:
        if (e->kind == VN_BINARY && e->op == in->nodeOp && e->a == in->a && e->b == in->b)
          match = e->temp;
        break;
      default:
        break; // stores/prints are effects: no value to number
      }
    }

    if (in->op == IR_STORE) {
      // A store kills any cached load of this variable, then publishes the new
      // value so a following load of it reuses the stored temp.
      for (int k = 0; k < cnt; k++)
        if (tab[k].valid && tab[k].kind == VN_LOAD && tab[k].var == in->var)
          tab[k].valid = false;
      if (cnt + 1 > cap) { cap = cap < 8 ? 8 : cap * 2; tab = realloc(tab, sizeof(VNEntry) * cap); }
      tab[cnt++] = (VNEntry){VN_LOAD, true, 0, NIL_VAL, in->var, in->a, -1, in->a};
      continue;
    }
    if (in->op == IR_PRINT)
      continue; // pure effect, operands already canonicalised

    if (match >= 0) {
      // Redundant: this temp is the same value as `match`.
      rep[in->dest] = match;
      in->dead = true;
      continue;
    }
    // First time we have seen this value: record it.
    if (cnt + 1 > cap) { cap = cap < 8 ? 8 : cap * 2; tab = realloc(tab, sizeof(VNEntry) * cap); }
    VNEntry e = {VN_CONST, true, in->nodeOp, in->constant, in->var, in->a, in->b, in->dest};
    e.kind = in->op == IR_CONST ? VN_CONST
             : in->op == IR_LOAD ? VN_LOAD
             : in->op == IR_UNARY ? VN_UNARY
                                  : VN_BINARY;
    tab[cnt++] = e;
  }

  free(rep);
  free(tab);
}

// --- pass 3: dead-temp elimination (step 65) --------------------------------
// A backward liveness sweep. Effects (stores, prints) are always kept and make
// their operands live; a value-producing instruction is kept only if its temp is
// live. One pass suffices because the code is straight-line — every temp is
// defined before it is used, so by the time we reach a definition we have already
// seen all of its uses. This also clears the now-dead constants left by folding.
static void deadTempElim(IRFunc *fn) {
  int n = fn->nextTemp;
  bool *live = n ? calloc(n, sizeof(bool)) : NULL;
  for (int i = fn->count - 1; i >= 0; i--) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    bool keep = (in->op == IR_STORE || in->op == IR_PRINT) // observable effects
                || (in->dest >= 0 && live[in->dest]);
    if (!keep) {
      in->dead = true;
      continue;
    }
    if (in->a >= 0) live[in->a] = true;
    if (in->b >= 0) live[in->b] = true;
  }
  free(live);
}

// These three passes are all LOCAL — they assume one straight-line basic block
// (the most-recent store dominates every load, every value flows forward once).
// Branches break those assumptions (a load could come from either side of a
// merge; a backward jump re-runs code), so when the IR contains control flow we
// must NOT run them. Making them block-aware is a future step (it needs the CFG
// + a dominator/data-flow framework).
static bool hasControlFlow(IRFunc *fn) {
  for (int i = 0; i < fn->count; i++)
    if (fn->code[i].op == IR_LABEL || fn->code[i].op == IR_JUMP ||
        fn->code[i].op == IR_JUMP_IF_FALSE)
      return true;
  return false;
}

// Run the optimisation passes in place, without printing — for backends (the
// x86-64 emitter) that want the optimised IR but not the --ir commentary.
void optimizeIRPasses(IRFunc *fn) {
  if (hasControlFlow(fn))
    return; // the local passes are unsound across branches
  constPropFold(fn); // propagate + fold constants
  cse(fn);           // share repeated subexpressions
  deadTempElim(fn);  // drop temporaries nothing reads
}

// Optimise the IR in place and show the result. Prints "lowered" (the raw
// lowering), then runs the three passes, then prints "optimised".
void optimizeIR(IRFunc *fn) {
  printIR(fn, "lowered");
  if (hasControlFlow(fn)) {
    printf("(optimiser skipped: the local passes need straight-line code; this "
           "function has control flow)\n\n");
    return;
  }
  optimizeIRPasses(fn);
  printIR(fn, "optimised");
}
