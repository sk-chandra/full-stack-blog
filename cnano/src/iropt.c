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

// --- pass 1: constant propagation + folding (step 64b; block-local in step 73)
// One forward pass. Folding and TEMP propagation are always safe (a temp is
// assigned once and never crosses a basic block). VARIABLE propagation is only
// valid within a basic block, so we drop all variable knowledge at a block
// boundary (a label is a merge point — the value could come from either
// predecessor) and at a call (which may reassign globals). That conservative
// reset is what makes the pass sound in the presence of control flow.
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
      // The last store wins (within this block): record or clear its constant.
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
    case IR_CALL:
      if (in->dest >= 0) known[in->dest] = false; // result is not a known constant
      varCount = 0; // a call may reassign globals — drop variable knowledge
      break;
    case IR_LABEL:
    case IR_JUMP:
    case IR_JUMP_IF_FALSE:
    case IR_RETURN:
      varCount = 0; // basic-block boundary: no variable's value is guaranteed
      break;
    case IR_PRINT:
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
    // A label is a merge point: values numbered in one predecessor can't be
    // assumed here, so the value table resets at every basic-block boundary.
    // A JUMP resets too — and its `a` is a LABEL id, NOT a temp, so it must
    // never go through the representative table (a hard-won lesson: an untyped
    // int field meaning two different things is a bug waiting for a pass that
    // forgets which one it is holding).
    if (in->op == IR_LABEL || in->op == IR_JUMP) { cnt = 0; continue; }
    if (in->op == IR_JUMP_IF_FALSE) {
      in->a = vnResolve(rep, in->a); // the condition IS a temp; `b` is a label
      cnt = 0;
      continue;
    }
    if (in->op == IR_RETURN) {
      if (in->a >= 0)
        in->a = vnResolve(rep, in->a);
      cnt = 0;
      continue;
    }
    if (in->op == IR_CALL) {
      // A call has side effects and may change globals: never share two calls,
      // and drop the value table (cached loads could now be stale).
      for (int k = 0; k < in->callArgCount; k++)
        in->callArgs[k] = vnResolve(rep, in->callArgs[k]);
      cnt = 0;
      continue;
    }
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

// --- pass 3: dead-temp elimination (step 65; control-flow-aware in step 73) --
// A backward liveness sweep. EFFECTS (stores, prints, calls, returns, branches,
// labels) are always kept and make their operands live; a value-producing
// instruction is kept only if its temp is live. One pass suffices because a temp
// is defined once and never crosses a basic block (loop-carried state lives in
// variables), so all of a temp's uses are seen before its definition going
// backward. This also clears the now-dead constants left by folding. NB: operand
// liveness is op-specific — a label/jump carries a label id in `a`, NOT a temp.
static void deadTempElim(IRFunc *fn) {
  int n = fn->nextTemp;
  bool *live = n ? calloc(n, sizeof(bool)) : NULL;
  for (int i = fn->count - 1; i >= 0; i--) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    bool effect = in->op == IR_STORE || in->op == IR_PRINT || in->op == IR_CALL ||
                  in->op == IR_RETURN || in->op == IR_JUMP ||
                  in->op == IR_JUMP_IF_FALSE || in->op == IR_LABEL;
    if (!effect && !(in->dest >= 0 && live[in->dest])) {
      in->dead = true;
      continue;
    }
    switch (in->op) {
    case IR_STORE:
    case IR_PRINT:
    case IR_UNARY:
    case IR_JUMP_IF_FALSE:
      if (in->a >= 0) live[in->a] = true;
      break;
    case IR_RETURN:
      if (in->a >= 0) live[in->a] = true;
      break;
    case IR_BINARY:
      if (in->a >= 0) live[in->a] = true;
      if (in->b >= 0) live[in->b] = true;
      break;
    case IR_CALL:
      for (int k = 0; k < in->callArgCount; k++) live[in->callArgs[k]] = true;
      break;
    default: // IR_CONST, IR_LOAD, IR_LABEL, IR_JUMP: no temp operands
      break;
    }
  }
  free(live);
}

// --- pass 4: loop-invariant code motion (step 79) ----------------------------
// The first WHOLE-CFG optimisation: move work that computes the same value on
// every iteration OUT of the loop, so it runs once. Loops are found by shape —
// the lowering emits every loop as `Lstart: … goto Lstart`, so a backward jump
// marks one (this is the structured-lowering dividend; irreducible CFGs would
// need real dominator analysis). Inside a loop, an instruction is INVARIANT if:
//   - it is a constant; or
//   - it loads a variable with NO store in the loop and NO call in the loop
//     (a call may reassign any global); or
//   - it is a pure op whose operands are defined outside the loop or by
//     already-invariant instructions (transitive, so we iterate to a fixpoint).
// Two ops are deliberately NOT hoisted even when invariant: `/` and `%`, which
// can FAULT (divide by zero) — hoisting one out of a loop that may run zero
// times would make a program fail that should not (speculation safety, the same
// reason real compilers model "can this instruction trap?").
//
// One hoist per call; the driver loops, so an instruction hoisted out of an
// inner loop lands in the outer loop's body and can be hoisted again (the
// classic cascade). NB: hoisting makes a temp's definition cross block
// boundaries, which is fine for `--ir` display and the local passes (defs still
// precede uses), but is why the x86-64 backend consumes the UNOPTIMISED IR.
static bool licmOnce(IRFunc *fn) {
  for (int j = 0; j < fn->count; j++) {
    if (fn->code[j].dead || fn->code[j].op != IR_JUMP)
      continue;
    int s = -1; // the matching label BEFORE the jump = a loop header
    for (int k = 0; k < j; k++)
      if (!fn->code[k].dead && fn->code[k].op == IR_LABEL &&
          fn->code[k].a == fn->code[j].a) {
        s = k;
        break;
      }
    if (s < 0)
      continue; // a forward jump (if/else), not a loop

    // Facts about the loop body (s..j): temps defined in it, variables stored
    // in it, and whether it calls anything.
    int n = fn->nextTemp;
    bool *defIn = calloc(n > 0 ? n : 1, sizeof(bool));
    bool hasCall = false;
    for (int k = s; k <= j; k++) {
      IRInstr *in = &fn->code[k];
      if (in->dead)
        continue;
      if (in->dest >= 0)
        defIn[in->dest] = true;
      if (in->op == IR_CALL)
        hasCall = true;
    }

    // Grow the invariant set to a fixpoint (invariance is transitive).
    bool *inv = calloc(fn->count, sizeof(bool));
    int found = 0;
    bool grew = true;
    while (grew) {
      grew = false;
      for (int k = s + 1; k < j; k++) {
        IRInstr *in = &fn->code[k];
        if (in->dead || inv[k] || in->dest < 0)
          continue;
        bool cand = false;
        if (in->op == IR_CONST) {
          cand = true;
        } else if (in->op == IR_LOAD) {
          cand = !hasCall;
          for (int q = s; q <= j && cand; q++)
            if (!fn->code[q].dead && fn->code[q].op == IR_STORE &&
                fn->code[q].var == in->var)
              cand = false;
        } else if (in->op == IR_UNARY || in->op == IR_BINARY) {
          if (in->op == IR_BINARY &&
              (in->nodeOp == OP_NODE_DIV || in->nodeOp == OP_NODE_MOD)) {
            cand = false; // can fault: never speculate
          } else {
            cand = true;
            int ops[2] = {in->a, in->b};
            for (int q = 0; q < 2 && cand; q++) {
              int t = ops[q];
              if (t < 0 || !defIn[t])
                continue; // defined before the loop: fine
              bool defInv = false; // defined in the loop: by an invariant instr?
              for (int w = s + 1; w < j && !defInv; w++)
                if (!fn->code[w].dead && inv[w] && fn->code[w].dest == t)
                  defInv = true;
              cand = defInv;
            }
          }
        }
        if (cand) {
          inv[k] = true;
          grew = true;
          found++;
        }
      }
    }

    if (found > 0) {
      // Rebuild the list with the invariant instructions moved to just BEFORE
      // the loop header, preserving their relative order (defs before uses).
      IRInstr *nc = malloc(sizeof(IRInstr) * fn->count);
      int w = 0;
      for (int k = 0; k < s; k++)
        nc[w++] = fn->code[k];
      for (int k = s + 1; k < j; k++)
        if (inv[k])
          nc[w++] = fn->code[k];
      for (int k = s; k < fn->count; k++)
        if (!(k > s && k < j && inv[k]))
          nc[w++] = fn->code[k];
      free(fn->code);
      fn->code = nc;
      fn->capacity = fn->count;
      free(defIn);
      free(inv);
      return true; // indices shifted: let the driver rescan
    }
    free(defIn);
    free(inv);
  }
  return false;
}

// Run the optimisation passes in place, without printing. The local passes are
// BASIC-BLOCK-LOCAL (facts reset at labels and calls); LICM is the whole-CFG
// pass layered on top.
void optimizeIRPasses(IRFunc *fn) {
  constPropFold(fn); // propagate + fold constants (within each block)
  while (licmOnce(fn)) // hoist loop-invariant work (cascading outward)
    ;
  cse(fn);           // share repeated subexpressions (within each block)
  deadTempElim(fn);  // drop temporaries nothing reads
}

// Optimise the IR in place and show the result. Prints "lowered" (the raw
// lowering), then runs the three passes, then prints "optimised".
void optimizeIR(IRFunc *fn) {
  printIR(fn, "lowered");
  optimizeIRPasses(fn);
  printIR(fn, "optimised");
}
