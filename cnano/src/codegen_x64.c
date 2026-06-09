// codegen_x64.c — emit AT&T x86-64 assembly from the IR (steps 69–72, 78).
//
// The classic back-end jobs, made concrete:
//
//   1. REGISTER ALLOCATION by LINEAR SCAN. The IR has unboundedly many
//      temporaries (t0, t1, …); the CPU has a handful of registers. We compute
//      each temp's live range [definition … last use], then sweep the
//      instructions once, handing out registers and reclaiming them as ranges
//      end. When more values are live at once than there are registers, we SPILL
//      the one whose range ends latest to a stack slot — the textbook heuristic.
//      We use the five CALLEE-SAVED registers (rbx, r12–r15) on purpose: their
//      values survive the `call printf`, so there is nothing to save around calls.
//
//   2. INSTRUCTION SELECTION. Each IR op maps to one or two machine instructions
//      (`t = a + b` → `mov a→rax; add b→rax; mov rax→t`). rax is the scratch
//      accumulator; rcx/rdx are borrowed for shifts and division.
//
//   3. TYPE-CLASS ANALYSIS (step 78). cnano values are dynamically tagged, but
//      machine code is not: an integer add, a float add, and a boolean test are
//      DIFFERENT instructions on different registers. A module-wide fixpoint
//      classifies every temp, variable, and function return as INT, BOOL, or
//      FLOAT (flowing classes through stores→loads, call arguments→parameters,
//      and returns→call results); anything that would need a runtime tag — a
//      value that is an int on one path and a float on another — is rejected
//      rather than miscompiled.
//
// FLOATS use the SSE2 scalar instructions (addsd/mulsd/ucomisd/…) and live in
// MEMORY slots, not registers: the System V ABI has no callee-saved xmm
// registers, so a float held in xmm would die across every `call printf` — the
// memory-resident choice is the honest one, and why the int story and the float
// story differ. `print` of a float calls an emitted helper that reproduces the
// VM's formatFloat exactly (%g, then append ".0" unless it already looks like a
// float; nan/inf spelled like the VM).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codegen_x64.h"
#include "object.h" // ObjString->chars

// The five callee-saved general registers we allocate (int/bool) IR temps into.
#define NUM_REGS 5
static const char *kRegNames[NUM_REGS] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};

typedef enum { L_NONE, L_REG, L_SPILL } LocKind;
typedef struct {
  LocKind kind;
  int idx; // register index (0..4) or spill-slot number
} Loc;

// The System V integer and float argument registers, in order. Each argument
// consumes the next register of ITS class (gpr and xmm counters run separately).
static const char *kArgRegs[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static const char *kXmmArgs[8] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3",
                                  "%xmm4", "%xmm5", "%xmm6", "%xmm7"};

// --- the type-class lattice --------------------------------------------------
// UNKNOWN is the starting point; INT/BOOL/FLOAT are the usable classes; CONFLICT
// means two classes met (an int on one path, a float on another) — the machine
// can't represent that without a runtime tag, so the module is rejected.
typedef enum { TC_UNKNOWN = 0, TC_INT, TC_BOOL, TC_FLOAT, TC_CONFLICT } TClass;

// Join `c` into `*slot`; returns true if the slot changed.
static bool joinClass(TClass *slot, TClass c) {
  if (c == TC_UNKNOWN || *slot == c || *slot == TC_CONFLICT)
    return false;
  *slot = (*slot == TC_UNKNOWN) ? c : TC_CONFLICT;
  return true;
}

// Everything the classifier learned about one function.
typedef struct {
  ObjString **vars; // the variables (params first), each owning a stack slot
  int varCount;
  TClass *varClass;  // per variable
  TClass *tempClass; // per temporary
  TClass retClass;   // join of every `return` value's class
} FnInfo;

// Does instruction `in` read temporary in a / in b? (Defines which operands form
// the live ranges, and which to load when emitting.) IR_JUMP_IF_FALSE reads its
// condition temp `a`; IR_RETURN reads its value `a`; IR_CALL reads its callArgs
// (handled separately). A JUMP/JUMP_IF_FALSE `b` is a LABEL, not a temp.
static bool readsA(const IRInstr *in) {
  return in->op == IR_STORE || in->op == IR_PRINT || in->op == IR_UNARY ||
         in->op == IR_BINARY || in->op == IR_JUMP_IF_FALSE ||
         in->op == IR_RETURN;
}
static bool readsB(const IRInstr *in) { return in->op == IR_BINARY; }

// The index of the function named `name` within the module, or -1 (external).
static int findFuncIndex(IRModule *m, ObjString *name) {
  for (int k = 0; k < m->count; k++)
    if (strcmp(m->funcs[k]->name, name->chars) == 0)
      return k;
  return -1;
}

static bool isComparison(NodeOp op) {
  return op == OP_NODE_EQUAL || op == OP_NODE_LESS || op == OP_NODE_GREATER;
}
static bool isArith(NodeOp op) {
  return op == OP_NODE_ADD || op == OP_NODE_SUB || op == OP_NODE_MUL ||
         op == OP_NODE_DIV;
}

// The rbp-relative byte offset of variable `name`'s stack slot (variables follow
// the reserved register-save slots).
static int varOffset(ObjString *name, ObjString **vars, int varCount) {
  for (int v = 0; v < varCount; v++)
    if (vars[v] == name)
      return (NUM_REGS + v + 1) * 8;
  return 0;
}

static int varIndex(ObjString *name, FnInfo *info) {
  for (int v = 0; v < info->varCount; v++)
    if (info->vars[v] == name)
      return v;
  return -1;
}

// Write the assembly operand for a temp's location into `buf` (e.g. "%rbx" or
// "-48(%rbp)"). Spill slots follow the register-save slots and the variables.
static void tempOperand(Loc l, int varCount, char *buf, size_t n) {
  if (l.kind == L_REG)
    snprintf(buf, n, "%s", kRegNames[l.idx]);
  else
    snprintf(buf, n, "-%d(%%rbp)", (NUM_REGS + varCount + l.idx + 1) * 8);
}

// Collect the distinct variables that need a stack slot: the function's
// PARAMETERS first (so argument registers can be spilled into them in order),
// then every variable loaded or stored in the body.
static ObjString **collectVars(IRFunc *fn, int *outCount) {
  ObjString **vars = NULL;
  int count = 0, cap = 0;
  for (int p = 0; p < fn->paramCount; p++) {
    if (count + 1 > cap) { cap = cap < 8 ? 8 : cap * 2; vars = realloc(vars, sizeof(ObjString *) * cap); }
    vars[count++] = fn->params[p];
  }
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead || (in->op != IR_LOAD && in->op != IR_STORE))
      continue;
    bool seen = false;
    for (int v = 0; v < count; v++)
      if (vars[v] == in->var) { seen = true; break; }
    if (!seen) {
      if (count + 1 > cap) { cap = cap < 8 ? 8 : cap * 2; vars = realloc(vars, sizeof(ObjString *) * cap); }
      vars[count++] = in->var;
    }
  }
  *outCount = count;
  return vars;
}

// One classification sweep over `fn`, flowing classes forward (and across the
// module: call arguments into the callee's parameters, callee returns into call
// results). Returns true if anything changed — the module driver iterates to a
// fixpoint, which handles loops, mutual recursion, and any declaration order.
static bool classifySweep(IRModule *m, int fi, FnInfo *infos) {
  IRFunc *fn = m->funcs[fi];
  FnInfo *info = &infos[fi];
  bool changed = false;
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    TClass out = TC_UNKNOWN;
    switch (in->op) {
    case IR_CONST:
      out = IS_BOOL(in->constant) ? TC_BOOL
            : IS_FLOAT(in->constant) ? TC_FLOAT
            : IS_INT(in->constant) ? TC_INT
                                   : TC_CONFLICT; // nil: unrepresentable
      break;
    case IR_LOAD: {
      int v = varIndex(in->var, info);
      if (v >= 0)
        out = info->varClass[v];
      break;
    }
    case IR_STORE: {
      int v = varIndex(in->var, info);
      if (v >= 0)
        changed |= joinClass(&info->varClass[v], info->tempClass[in->a]);
      continue;
    }
    case IR_UNARY:
      out = in->nodeOp == OP_NODE_NOT      ? TC_BOOL
            : in->nodeOp == OP_NODE_BITNOT ? TC_INT
                                           : info->tempClass[in->a]; // negate
      break;
    case IR_BINARY:
      if (isComparison(in->nodeOp))
        out = TC_BOOL;
      else if (isArith(in->nodeOp)) {
        TClass a = info->tempClass[in->a], b = info->tempClass[in->b];
        // Mixed int/float arithmetic promotes to float, mirroring the VM.
        out = (a == TC_FLOAT || b == TC_FLOAT) ? TC_FLOAT
              : (a == TC_INT && b == TC_INT)   ? TC_INT
                                               : TC_UNKNOWN; // wait for operands
      } else
        out = TC_INT; // %, bitwise, shifts: integer-only (checked later)
      break;
    case IR_CALL: {
      int ci = findFuncIndex(m, in->var);
      if (ci >= 0) {
        out = infos[ci].retClass;
        // Flow each argument's class into the callee's parameter variable.
        IRFunc *callee = m->funcs[ci];
        for (int k = 0; k < in->callArgCount && k < callee->paramCount; k++) {
          int pv = varIndex(callee->params[k], &infos[ci]);
          if (pv >= 0)
            changed |=
                joinClass(&infos[ci].varClass[pv], info->tempClass[in->callArgs[k]]);
        }
      }
      break;
    }
    case IR_RETURN:
      if (in->a >= 0)
        changed |= joinClass(&info->retClass, info->tempClass[in->a]);
      continue;
    default:
      continue; // print/label/jump produce no value
    }
    if (in->dest >= 0)
      changed |= joinClass(&info->tempClass[in->dest], out);
  }
  return changed;
}

// Can this function be compiled faithfully? Anything the machine code would get
// WRONG relative to the VM is rejected (the whole module, never half-emitted).
static bool functionSupported(IRFunc *fn, IRModule *m, FnInfo *info) {
  if (fn->paramCount > 6)
    return false; // only register args; no stack-passed parameters
  if (info->retClass == TC_CONFLICT)
    return false;
  for (int v = 0; v < info->varCount; v++)
    if (info->varClass[v] == TC_CONFLICT)
      return false; // a variable holding different classes on different paths
  for (int t = 0; t < fn->nextTemp; t++)
    if (info->tempClass[t] == TC_CONFLICT)
      return false;
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    TClass a = in->a >= 0 ? info->tempClass[in->a] : TC_UNKNOWN;
    TClass b = in->b >= 0 ? info->tempClass[in->b] : TC_UNKNOWN;
    switch (in->op) {
    case IR_CONST:
      if (IS_NIL(in->constant))
        return false; // nil has no machine representation here
      break;
    case IR_PRINT:
      break; // int, bool and float prints all have faithful machine paths now
    case IR_JUMP_IF_FALSE:
      if (a != TC_BOOL)
        return false; // cnano makes 0 truthy; a zero-test would disagree
      break;
    case IR_UNARY:
      if (in->nodeOp == OP_NODE_NOT && a != TC_BOOL)
        return false; // !int is about truthiness (0 is truthy) — int path wrong
      if (in->nodeOp == OP_NODE_BITNOT && a != TC_INT)
        return false;
      if (in->nodeOp == OP_NODE_NEGATE && a == TC_BOOL)
        return false;
      break;
    case IR_BINARY:
      if (in->nodeOp == OP_NODE_EQUAL) {
        // The VM's == across DIFFERENT types is always false; and bool==bool
        // works on 0/1 ints. Same-class only, so the machine compare agrees.
        if (a != b || a == TC_UNKNOWN)
          return false;
      } else if (in->nodeOp == OP_NODE_LESS || in->nodeOp == OP_NODE_GREATER) {
        if (a == TC_BOOL || b == TC_BOOL)
          return false; // numeric comparison only
      } else if (isArith(in->nodeOp)) {
        if (a == TC_BOOL || b == TC_BOOL)
          return false;
      } else { // %, bitwise, shifts: the VM requires integers
        if (a != TC_INT || b != TC_INT)
          return false;
      }
      break;
    case IR_CALL:
      if (findFuncIndex(m, in->var) < 0 || in->callArgCount > 6)
        return false; // unknown callee, or more than six register args
      break;
    default:
      break;
    }
  }
  return true;
}

// Load temp `t` into xmm register `xmm` for a float operation, converting an
// integer operand on the fly (the VM's int→float promotion, in hardware).
static void loadXmm(FILE *out, FnInfo *info, Loc *loc, int varCount, int t,
                    const char *xmm) {
  char op[32];
  tempOperand(loc[t], varCount, op, sizeof(op));
  if (info->tempClass[t] == TC_FLOAT)
    fprintf(out, "  movsd %s, %s\n", op, xmm);
  else
    fprintf(out, "  cvtsi2sdq %s, %s\n", op, xmm);
}

// Emit one function: prologue (save callee-saved regs, spill incoming argument
// registers into the parameter slots), the body, and a shared epilogue.
static void emitFunction(IRModule *m, int fi, FnInfo *infos, bool isMain,
                         FILE *out) {
  IRFunc *fn = m->funcs[fi];
  FnInfo *info = &infos[fi];
  int n = fn->nextTemp;
  ObjString **vars = info->vars;
  int varCount = info->varCount;

  // --- live ranges -----------------------------------------------------------
  int *lastUse = n > 0 ? malloc(sizeof(int) * n) : NULL;
  for (int i = 0; i < n; i++)
    lastUse[i] = -1;
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (readsA(in) && in->a >= 0) lastUse[in->a] = i;
    if (readsB(in) && in->b >= 0) lastUse[in->b] = i;
    if (in->op == IR_CALL)
      for (int k = 0; k < in->callArgCount; k++)
        lastUse[in->callArgs[k]] = i; // every argument is live up to the call
  }

  // --- linear-scan allocation --------------------------------------------------
  // FLOAT temps always get a MEMORY slot: the ABI has no callee-saved xmm
  // registers, so a float in xmm would not survive the calls we emit.
  Loc *loc = n > 0 ? calloc(n, sizeof(Loc)) : NULL;
  int freeRegs[NUM_REGS];
  int freeCount = NUM_REGS;
  for (int i = 0; i < NUM_REGS; i++)
    freeRegs[i] = NUM_REGS - 1 - i;
  int active[NUM_REGS];
  int activeCount = 0, spillCount = 0;
  bool usedReg[NUM_REGS] = {false};
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead || in->dest < 0)
      continue;
    for (int k = activeCount - 1; k >= 0; k--) {
      int t = active[k];
      if (lastUse[t] < i) { freeRegs[freeCount++] = loc[t].idx; active[k] = active[--activeCount]; }
    }
    int d = in->dest;
    if (lastUse[d] < i) lastUse[d] = i;
    if (info->tempClass[d] == TC_FLOAT) {
      loc[d] = (Loc){L_SPILL, spillCount++};
      continue;
    }
    if (freeCount > 0) {
      int r = freeRegs[--freeCount];
      loc[d] = (Loc){L_REG, r}; usedReg[r] = true; active[activeCount++] = d;
    } else {
      int victimSlot = 0;
      for (int k = 1; k < activeCount; k++)
        if (lastUse[active[k]] > lastUse[active[victimSlot]]) victimSlot = k;
      int victim = active[victimSlot];
      if (lastUse[victim] > lastUse[d]) {
        loc[d] = (Loc){L_REG, loc[victim].idx};
        active[victimSlot] = d;
        loc[victim] = (Loc){L_SPILL, spillCount++};
      } else {
        loc[d] = (Loc){L_SPILL, spillCount++};
      }
    }
  }
  int slots = NUM_REGS + varCount + spillCount;
  int frame = ((slots * 8) + 15) & ~15;

  // --- prologue ----------------------------------------------------------------
  if (isMain)
    fprintf(out, "main:\n");
  else
    fprintf(out, "cn_%s:\n", fn->name);
  fprintf(out, "  pushq %%rbp\n  movq %%rsp, %%rbp\n");
  if (frame > 0)
    fprintf(out, "  subq $%d, %%rsp\n", frame);
  for (int r = 0; r < NUM_REGS; r++)
    if (usedReg[r])
      fprintf(out, "  movq %s, -%d(%%rbp)\n", kRegNames[r], (r + 1) * 8);
  // Spill the incoming arguments into the parameter slots. The gpr and xmm
  // argument registers are consumed independently, per the System V ABI.
  {
    int gp = 0, xp = 0;
    for (int p = 0; p < fn->paramCount; p++) {
      int off = varOffset(fn->params[p], vars, varCount);
      if (info->varClass[varIndex(fn->params[p], info)] == TC_FLOAT)
        fprintf(out, "  movsd %s, -%d(%%rbp)\n", kXmmArgs[xp++], off);
      else
        fprintf(out, "  movq %s, -%d(%%rbp)\n", kArgRegs[gp++], off);
    }
  }

  // --- body ----------------------------------------------------------------
  char ba[32], bb[32], bd[32];
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (in->dest >= 0) tempOperand(loc[in->dest], varCount, bd, sizeof(bd));
    if (in->a >= 0 && readsA(in)) tempOperand(loc[in->a], varCount, ba, sizeof(ba));
    if (in->b >= 0 && readsB(in)) tempOperand(loc[in->b], varCount, bb, sizeof(bb));

    switch (in->op) {
    case IR_CONST: {
      long long imm;
      if (IS_FLOAT(in->constant)) {
        // A float constant is its IEEE-754 BITS, moved as a plain integer (the
        // slot is just 8 bytes; only the float ops interpret them as a double).
        double d = AS_FLOAT(in->constant);
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        imm = (long long)bits;
      } else {
        imm = IS_BOOL(in->constant) ? (AS_BOOL(in->constant) ? 1 : 0)
                                    : (long long)AS_INT(in->constant);
      }
      if (loc[in->dest].kind == L_REG)
        fprintf(out, "  movabsq $%lld, %s\n", imm, bd);
      else
        fprintf(out, "  movabsq $%lld, %%rax\n  movq %%rax, %s\n", imm, bd);
      break;
    }
    case IR_LOAD: // an 8-byte bit copy, whatever the class
      fprintf(out, "  movq -%d(%%rbp), %%rax\n  movq %%rax, %s\n",
              varOffset(in->var, vars, varCount), bd);
      break;
    case IR_STORE:
      fprintf(out, "  movq %s, %%rax\n  movq %%rax, -%d(%%rbp)\n", ba,
              varOffset(in->var, vars, varCount));
      break;
    case IR_UNARY:
      if (in->nodeOp == OP_NODE_NOT) {
        // Logical not on a 0/1 boolean — NOT bitwise notq, which would turn
        // true (1) into -2 (still truthy). One bit flips: xor 1.
        fprintf(out, "  movq %s, %%rax\n  xorq $1, %%rax\n  movq %%rax, %s\n",
                ba, bd);
      } else if (info->tempClass[in->dest] == TC_FLOAT) {
        // Float negate: flip the sign BIT (IEEE-754), the hardware idiom.
        loadXmm(out, info, loc, varCount, in->a, "%xmm0");
        fprintf(out, "  xorpd .LCsgn(%%rip), %%xmm0\n  movsd %%xmm0, %s\n", bd);
      } else {
        fprintf(out, "  movq %s, %%rax\n", ba);
        fprintf(out, in->nodeOp == OP_NODE_NEGATE ? "  negq %%rax\n"
                                                  : "  notq %%rax\n");
        fprintf(out, "  movq %%rax, %s\n", bd);
      }
      break;
    case IR_BINARY: {
      bool floatOp =
          info->tempClass[in->a] == TC_FLOAT || info->tempClass[in->b] == TC_FLOAT;
      if (floatOp && isArith(in->nodeOp)) {
        loadXmm(out, info, loc, varCount, in->a, "%xmm0");
        loadXmm(out, info, loc, varCount, in->b, "%xmm1");
        const char *op = in->nodeOp == OP_NODE_ADD   ? "addsd"
                         : in->nodeOp == OP_NODE_SUB ? "subsd"
                         : in->nodeOp == OP_NODE_MUL ? "mulsd"
                                                     : "divsd";
        fprintf(out, "  %s %%xmm1, %%xmm0\n  movsd %%xmm0, %s\n", op, bd);
        break;
      }
      if (floatOp && isComparison(in->nodeOp)) {
        loadXmm(out, info, loc, varCount, in->a, "%xmm0");
        loadXmm(out, info, loc, varCount, in->b, "%xmm1");
        // ucomisd sets CF/ZF/PF; an UNORDERED result (nan) sets all three, so:
        //   a > b  -> seta on (a cmp b)      (unordered: CF=1 -> false, right)
        //   a < b  -> seta on (b cmp a)      (swap instead of setb, same reason)
        //   a == b -> sete AND setnp         (unordered sets ZF, PF screens it)
        if (in->nodeOp == OP_NODE_GREATER)
          fprintf(out, "  ucomisd %%xmm1, %%xmm0\n  seta %%al\n");
        else if (in->nodeOp == OP_NODE_LESS)
          fprintf(out, "  ucomisd %%xmm0, %%xmm1\n  seta %%al\n");
        else
          fprintf(out, "  ucomisd %%xmm1, %%xmm0\n  setnp %%al\n  sete %%cl\n"
                       "  andb %%cl, %%al\n");
        fprintf(out, "  movzbq %%al, %%rax\n  movq %%rax, %s\n", bd);
        break;
      }
      fprintf(out, "  movq %s, %%rax\n", ba);
      switch (in->nodeOp) {
      case OP_NODE_ADD: fprintf(out, "  addq %s, %%rax\n", bb); break;
      case OP_NODE_SUB: fprintf(out, "  subq %s, %%rax\n", bb); break;
      case OP_NODE_MUL: fprintf(out, "  imulq %s, %%rax\n", bb); break;
      case OP_NODE_BITAND: fprintf(out, "  andq %s, %%rax\n", bb); break;
      case OP_NODE_BITOR: fprintf(out, "  orq %s, %%rax\n", bb); break;
      case OP_NODE_BITXOR: fprintf(out, "  xorq %s, %%rax\n", bb); break;
      case OP_NODE_SHL: fprintf(out, "  movq %s, %%rcx\n  salq %%cl, %%rax\n", bb); break;
      case OP_NODE_SHR: fprintf(out, "  movq %s, %%rcx\n  sarq %%cl, %%rax\n", bb); break;
      case OP_NODE_DIV: fprintf(out, "  cqto\n  idivq %s\n", bb); break;
      case OP_NODE_MOD: fprintf(out, "  cqto\n  idivq %s\n  movq %%rdx, %%rax\n", bb); break;
      case OP_NODE_EQUAL:   fprintf(out, "  cmpq %s, %%rax\n  sete %%al\n  movzbq %%al, %%rax\n", bb); break;
      case OP_NODE_LESS:    fprintf(out, "  cmpq %s, %%rax\n  setl %%al\n  movzbq %%al, %%rax\n", bb); break;
      case OP_NODE_GREATER: fprintf(out, "  cmpq %s, %%rax\n  setg %%al\n  movzbq %%al, %%rax\n", bb); break;
      default: break;
      }
      fprintf(out, "  movq %%rax, %s\n", bd);
      break;
    }
    case IR_PRINT:
      if (info->tempClass[in->a] == TC_FLOAT) {
        fprintf(out, "  movsd %s, %%xmm0\n  call cn_print_f64\n", ba);
      } else if (info->tempClass[in->a] == TC_BOOL) {
        // Print the WORD, exactly like the VM — never the 0/1 underneath.
        fprintf(out, "  movq %s, %%rax\n  leaq .LCtrue(%%rip), %%rsi\n", ba);
        fprintf(out, "  testq %%rax, %%rax\n  jnz 1f\n");
        fprintf(out, "  leaq .LCfalse(%%rip), %%rsi\n1:\n");
        fprintf(out, "  leaq .LCsfmt(%%rip), %%rdi\n");
        fprintf(out, "  xorl %%eax, %%eax\n  call printf@PLT\n");
      } else {
        fprintf(out, "  leaq .LCfmt(%%rip), %%rdi\n  movq %s, %%rsi\n", ba);
        fprintf(out, "  xorl %%eax, %%eax\n  call printf@PLT\n");
      }
      break;
    case IR_CALL: {
      // Marshal arguments into the System V registers — gpr and xmm counters
      // advance independently, one per argument of that class. Our temps live in
      // callee-saved registers or the stack, so they survive the call.
      int gp = 0, xp = 0;
      for (int k = 0; k < in->callArgCount; k++) {
        int t = in->callArgs[k];
        char ab[32];
        tempOperand(loc[t], varCount, ab, sizeof(ab));
        if (info->tempClass[t] == TC_FLOAT)
          fprintf(out, "  movsd %s, %s\n", ab, kXmmArgs[xp++]);
        else
          fprintf(out, "  movq %s, %s\n", ab, kArgRegs[gp++]);
      }
      fprintf(out, "  call cn_%s\n", in->var->chars);
      if (info->tempClass[in->dest] == TC_FLOAT)
        fprintf(out, "  movsd %%xmm0, %s\n", bd); // float result arrives in xmm0
      else
        fprintf(out, "  movq %%rax, %s\n", bd);
      break;
    }
    case IR_RETURN:
      if (in->a >= 0) {
        if (info->tempClass[in->a] == TC_FLOAT)
          fprintf(out, "  movsd %s, %%xmm0\n", ba); // float returns in xmm0
        else
          fprintf(out, "  movq %s, %%rax\n", ba);
      }
      fprintf(out, "  jmp .Lret%d\n", fi);
      break;
    case IR_LABEL:
      fprintf(out, ".LF%d_%d:\n", fi, in->a);
      break;
    case IR_JUMP:
      fprintf(out, "  jmp .LF%d_%d\n", fi, in->a);
      break;
    case IR_JUMP_IF_FALSE:
      fprintf(out, "  movq %s, %%rax\n  testq %%rax, %%rax\n  jz .LF%d_%d\n", ba,
              fi, in->b);
      break;
    }
  }

  // --- epilogue --------------------------------------------------------------
  // Fall-through default return value is 0; an explicit `return` jumps PAST this
  // (preserving rax/xmm0), so it only runs when control reaches the end.
  fprintf(out, "  xorl %%eax, %%eax\n.Lret%d:\n", fi);
  for (int r = 0; r < NUM_REGS; r++)
    if (usedReg[r])
      fprintf(out, "  movq -%d(%%rbp), %s\n", (r + 1) * 8, kRegNames[r]);
  fprintf(out, "  movq %%rbp, %%rsp\n  popq %%rbp\n  ret\n");

  free(loc);
  free(lastUse);
}

// The float `print` helper, emitted once when the module uses floats. It
// reproduces the VM's formatFloat EXACTLY: nan/inf spelled by hand (libc's %g
// prints "-nan"), else %g, then ".0" appended unless the text already contains
// '.', 'e', 'E', 'n' or 'i' — so 3.0 prints as "3.0", never "3".
static void emitFloatPrinter(FILE *out) {
  fprintf(out,
    "cn_print_f64:\n"
    "  pushq %%rbp\n  movq %%rsp, %%rbp\n"
    // Classify in the INTEGER domain: shift out the sign; all-ones exponent
    // with a zero mantissa is +/-inf, with a nonzero mantissa it is nan.
    "  movq %%xmm0, %%rax\n"
    "  movq %%rax, %%rcx\n  shlq $1, %%rcx\n"
    "  movabsq $-9007199254740992, %%rdx\n" // 0xFFE0000000000000
    "  cmpq %%rdx, %%rcx\n"
    "  ja .Lpf_nan\n"
    "  je .Lpf_inf\n"
    // Finite: snprintf(buf, 64, \"%%g\", v) — v already in xmm0; al = 1 says
    // \"one vector-register vararg\" (the varargs ABI's little handshake).
    "  leaq cn_fbuf(%%rip), %%rdi\n"
    "  movl $64, %%esi\n"
    "  leaq .LCgfmt(%%rip), %%rdx\n"
    "  movl $1, %%eax\n"
    "  call snprintf@PLT\n"
    // Scan for '.'(46) 'e'(101) 'E'(69) 'n'(110) 'i'(105); append \".0\" if none.
    "  leaq cn_fbuf(%%rip), %%rdi\n"
    ".Lpf_scan:\n"
    "  movzbl (%%rdi), %%eax\n"
    "  testb %%al, %%al\n  je .Lpf_append\n"
    "  cmpb $46, %%al\n  je .Lpf_show\n"
    "  cmpb $101, %%al\n  je .Lpf_show\n"
    "  cmpb $69, %%al\n  je .Lpf_show\n"
    "  cmpb $110, %%al\n  je .Lpf_show\n"
    "  cmpb $105, %%al\n  je .Lpf_show\n"
    "  incq %%rdi\n  jmp .Lpf_scan\n"
    ".Lpf_append:\n"
    "  movb $46, (%%rdi)\n  movb $48, 1(%%rdi)\n  movb $0, 2(%%rdi)\n"
    ".Lpf_show:\n"
    "  leaq cn_fbuf(%%rip), %%rsi\n"
    "  jmp .Lpf_out\n"
    ".Lpf_nan:\n"
    "  leaq .LCnan(%%rip), %%rsi\n  jmp .Lpf_out\n"
    ".Lpf_inf:\n"
    "  leaq .LCinf(%%rip), %%rsi\n"
    "  testq %%rax, %%rax\n  jns .Lpf_out\n"
    "  leaq .LCninf(%%rip), %%rsi\n"
    ".Lpf_out:\n"
    "  leaq .LCsfmt(%%rip), %%rdi\n"
    "  xorl %%eax, %%eax\n  call printf@PLT\n"
    "  popq %%rbp\n  ret\n");
}

bool emitX64Module(IRModule *m, FILE *out) {
  // Classify every temp/variable/return in the module to a FIXPOINT (classes
  // flow through stores, across calls into parameters, and back out of returns).
  FnInfo *infos = calloc(m->count, sizeof(FnInfo));
  for (int f = 0; f < m->count; f++) {
    infos[f].vars = collectVars(m->funcs[f], &infos[f].varCount);
    infos[f].varClass = calloc(infos[f].varCount > 0 ? infos[f].varCount : 1,
                               sizeof(TClass));
    int nt = m->funcs[f]->nextTemp;
    infos[f].tempClass = calloc(nt > 0 ? nt : 1, sizeof(TClass));
    infos[f].retClass = TC_UNKNOWN;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (int f = 0; f < m->count; f++)
      changed |= classifySweep(m, f, infos);
  }

  // Reject the whole module up front if any function is out of subset.
  bool ok = true;
  bool usesFloat = false;
  for (int f = 0; f < m->count && ok; f++) {
    ok = functionSupported(m->funcs[f], m, &infos[f]);
    for (int t = 0; t < m->funcs[f]->nextTemp; t++)
      if (infos[f].tempClass[t] == TC_FLOAT)
        usesFloat = true;
  }
  if (ok) {
    fprintf(out, "  .section .rodata\n.LCfmt:\n  .string \"%%lld\\n\"\n");
    fprintf(out, ".LCsfmt:\n  .string \"%%s\\n\"\n");
    fprintf(out, ".LCtrue:\n  .string \"true\"\n.LCfalse:\n  .string \"false\"\n");
    if (usesFloat) {
      fprintf(out, ".LCgfmt:\n  .string \"%%g\"\n");
      fprintf(out, ".LCnan:\n  .string \"nan\"\n.LCinf:\n  .string \"inf\"\n");
      fprintf(out, ".LCninf:\n  .string \"-inf\"\n");
      // The sign-bit mask for float negation: xorpd needs 16 aligned bytes.
      fprintf(out, "  .align 16\n.LCsgn:\n  .quad -9223372036854775808\n  .quad 0\n");
    }
    fprintf(out, "  .text\n  .globl main\n");
    if (usesFloat) {
      fprintf(out, "  .lcomm cn_fbuf, 64\n"); // the %g scratch buffer (.bss)
      emitFloatPrinter(out);
    }
    for (int f = 0; f < m->count; f++)
      emitFunction(m, f, infos, strcmp(m->funcs[f]->name, "main") == 0, out);
    fprintf(out, "  .section .note.GNU-stack,\"\",@progbits\n");
  }

  for (int f = 0; f < m->count; f++) {
    free(infos[f].vars);
    free(infos[f].varClass);
    free(infos[f].tempClass);
  }
  free(infos);
  return ok;
}
