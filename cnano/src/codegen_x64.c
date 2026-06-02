// codegen_x64.c — emit AT&T x86-64 assembly from the IR (step 69).
//
// Two classic back-end jobs, made concrete:
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
//   2. INSTRUCTION SELECTION. Each IR op maps to one or two instructions
//      (`t = a + b` → `mov a→rax; add b→rax; mov rax→t`). rax is the scratch
//      accumulator; rcx/rdx are borrowed for shifts and division.
//
// Scope is the IR's straight-line INTEGER subset; bool/float-producing ops are
// rejected up front (emitX64 returns false), keeping the backend honest.
#include <stdlib.h>
#include <string.h>

#include "codegen_x64.h"
#include "object.h" // ObjString->chars

// The five callee-saved general registers we allocate IR temporaries into.
#define NUM_REGS 5
static const char *kRegNames[NUM_REGS] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};

typedef enum { L_NONE, L_REG, L_SPILL } LocKind;
typedef struct {
  LocKind kind;
  int idx; // register index (0..4) or spill-slot number
} Loc;

// The System V integer argument registers, in order (up to six register args).
static const char *kArgRegs[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};

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

// Is a node-op a comparison (it produces a boolean, represented here as 0/1)?
static bool isComparison(NodeOp op) {
  return op == OP_NODE_EQUAL || op == OP_NODE_LESS || op == OP_NODE_GREATER;
}

// Classify each temporary as boolean or integer. A bool is born from a
// comparison, a logical `!`, a `true`/`false` constant, or a load of a variable
// that has had a bool stored to it. Because a load can precede its store across a
// loop's back-edge, we iterate to a FIXPOINT. (We don't print booleans or branch
// on integers, so this is what lets us keep the int/bool worlds from mixing.)
static void classifyBools(IRFunc *fn, bool *isBool, ObjString **vars,
                          bool *varIsBool, int varCount, IRModule *m,
                          bool *modReturnsBool) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < fn->count; i++) {
      IRInstr *in = &fn->code[i];
      if (in->dead)
        continue;
      bool nb = false;
      if (in->op == IR_CONST)
        nb = IS_BOOL(in->constant);
      else if (in->op == IR_BINARY)
        nb = isComparison(in->nodeOp);
      else if (in->op == IR_UNARY)
        nb = in->nodeOp == OP_NODE_NOT;
      else if (in->op == IR_CALL) {
        // The call's result is a boolean iff the callee returns one.
        int ci = findFuncIndex(m, in->var);
        nb = ci >= 0 && modReturnsBool[ci];
      } else if (in->op == IR_LOAD) {
        for (int v = 0; v < varCount; v++)
          if (vars[v] == in->var) { nb = varIsBool[v]; break; }
      } else if (in->op == IR_STORE) {
        // A bool stored into a variable taints it (so later loads are bool too).
        if (isBool[in->a])
          for (int v = 0; v < varCount; v++)
            if (vars[v] == in->var && !varIsBool[v]) { varIsBool[v] = true; changed = true; }
        continue;
      } else
        continue;
      if (in->dest >= 0 && nb && !isBool[in->dest]) { isBool[in->dest] = true; changed = true; }
    }
  }
}

// The rbp-relative byte offset of variable `name`'s stack slot (variables follow
// the reserved register-save slots).
static int varOffset(ObjString *name, ObjString **vars, int varCount) {
  for (int v = 0; v < varCount; v++)
    if (vars[v] == name)
      return (NUM_REGS + v + 1) * 8;
  return 0;
}

// Write the assembly operand for a temp's location into `buf` (e.g. "%rbx" or
// "-48(%rbp)"). `savedSlots`/`varCount` fix where spill slots begin in the frame.
static void tempOperand(Loc l, int reservedSlots, int varCount, char *buf, size_t n) {
  if (l.kind == L_REG)
    snprintf(buf, n, "%s", kRegNames[l.idx]);
  else // spill slots follow the reserved register-save slots and the variables
    snprintf(buf, n, "-%d(%%rbp)", (reservedSlots + varCount + l.idx + 1) * 8);
}

// Collect the distinct variables that need a stack slot: the function's
// PARAMETERS first (so they occupy the lowest var slots and always have storage,
// even if unused), then every variable loaded or stored in the body.
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

// Does this function return a boolean on any path? (Classifies its temps using
// the module's current return-type estimates; called to a fixpoint.)
static bool funcReturnsBool(IRFunc *fn, IRModule *m, bool *modReturnsBool) {
  int varCount;
  ObjString **vars = collectVars(fn, &varCount);
  int n = fn->nextTemp;
  bool *isBool = n > 0 ? calloc(n, sizeof(bool)) : NULL;
  bool *varIsBool = varCount > 0 ? calloc(varCount, sizeof(bool)) : NULL;
  classifyBools(fn, isBool, vars, varIsBool, varCount, m, modReturnsBool);
  bool rb = false;
  for (int i = 0; i < fn->count; i++)
    if (!fn->code[i].dead && fn->code[i].op == IR_RETURN && fn->code[i].a >= 0 &&
        isBool[fn->code[i].a])
      rb = true;
  free(vars);
  free(isBool);
  free(varIsBool);
  return rb;
}

// Can this function be compiled by the integer backend? Rejects what it cannot
// model faithfully (so the whole module is rejected rather than miscompiled).
static bool functionSupported(IRFunc *fn, IRModule *m, bool *modReturnsBool) {
  if (fn->paramCount > 6)
    return false; // only register args; no stack-passed parameters
  int varCount;
  ObjString **vars = collectVars(fn, &varCount);
  int n = fn->nextTemp;
  bool *isBool = n > 0 ? calloc(n, sizeof(bool)) : NULL;
  bool *varIsBool = varCount > 0 ? calloc(varCount, sizeof(bool)) : NULL;
  classifyBools(fn, isBool, vars, varIsBool, varCount, m, modReturnsBool);
  bool ok = true;
  for (int i = 0; i < fn->count && ok; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (in->op == IR_CONST && !IS_INT(in->constant) && !IS_BOOL(in->constant))
      ok = false; // float / nil constant
    else if (in->op == IR_PRINT && in->a >= 0 && isBool[in->a])
      ok = false; // printing a bool would show 0/1, not true/false
    else if (in->op == IR_JUMP_IF_FALSE && in->a >= 0 && !isBool[in->a])
      ok = false; // cnano makes 0 truthy; a zero-test would disagree
    else if (in->op == IR_CALL &&
             (findFuncIndex(m, in->var) < 0 || in->callArgCount > 6))
      ok = false; // unknown callee, or more than six register args
  }
  free(vars);
  free(isBool);
  free(varIsBool);
  return ok;
}

// Emit one function: its label, prologue (saving callee-saved regs and spilling
// the incoming argument registers into the parameter slots), the body, and an
// epilogue. `funcIdx` makes labels unique across functions.
static void emitFunction(IRFunc *fn, int funcIdx, bool isMain, FILE *out) {
  int n = fn->nextTemp;
  int varCount;
  ObjString **vars = collectVars(fn, &varCount);

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

  // --- linear-scan allocation ------------------------------------------------
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

  // --- prologue --------------------------------------------------------------
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
  // Spill the incoming argument registers into the parameter slots.
  for (int p = 0; p < fn->paramCount; p++)
    fprintf(out, "  movq %s, -%d(%%rbp)\n", kArgRegs[p],
            varOffset(fn->params[p], vars, varCount));

  // --- body ------------------------------------------------------------------
  char ba[32], bb[32], bd[32];
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (in->dest >= 0) tempOperand(loc[in->dest], NUM_REGS, varCount, bd, sizeof(bd));
    if (in->a >= 0 && readsA(in)) tempOperand(loc[in->a], NUM_REGS, varCount, ba, sizeof(ba));
    if (in->b >= 0 && readsB(in)) tempOperand(loc[in->b], NUM_REGS, varCount, bb, sizeof(bb));

    switch (in->op) {
    case IR_CONST: {
      long long imm = IS_BOOL(in->constant) ? (AS_BOOL(in->constant) ? 1 : 0)
                                             : (long long)AS_INT(in->constant);
      if (loc[in->dest].kind == L_REG)
        fprintf(out, "  movabsq $%lld, %s\n", imm, bd);
      else
        fprintf(out, "  movabsq $%lld, %%rax\n  movq %%rax, %s\n", imm, bd);
      break;
    }
    case IR_LOAD:
      fprintf(out, "  movq -%d(%%rbp), %%rax\n  movq %%rax, %s\n",
              varOffset(in->var, vars, varCount), bd);
      break;
    case IR_STORE:
      fprintf(out, "  movq %s, %%rax\n  movq %%rax, -%d(%%rbp)\n", ba,
              varOffset(in->var, vars, varCount));
      break;
    case IR_UNARY:
      fprintf(out, "  movq %s, %%rax\n", ba);
      fprintf(out, in->nodeOp == OP_NODE_NEGATE ? "  negq %%rax\n" : "  notq %%rax\n");
      fprintf(out, "  movq %%rax, %s\n", bd);
      break;
    case IR_BINARY:
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
    case IR_PRINT:
      fprintf(out, "  leaq .LCfmt(%%rip), %%rdi\n  movq %s, %%rsi\n", ba);
      fprintf(out, "  xorl %%eax, %%eax\n  call printf@PLT\n");
      break;
    case IR_CALL:
      // Marshal arguments into the System V integer registers, then call. Our
      // temps live in callee-saved registers (or the stack), so they survive the
      // call and the argument sources are never the arg registers themselves.
      for (int k = 0; k < in->callArgCount; k++) {
        char ab[32];
        tempOperand(loc[in->callArgs[k]], NUM_REGS, varCount, ab, sizeof(ab));
        fprintf(out, "  movq %s, %s\n", ab, kArgRegs[k]);
      }
      fprintf(out, "  call cn_%s\n", in->var->chars);
      fprintf(out, "  movq %%rax, %s\n", bd); // result -> dest temp
      break;
    case IR_RETURN:
      if (in->a >= 0)
        fprintf(out, "  movq %s, %%rax\n", ba);
      fprintf(out, "  jmp .Lret%d\n", funcIdx);
      break;
    case IR_LABEL:
      fprintf(out, ".LF%d_%d:\n", funcIdx, in->a);
      break;
    case IR_JUMP:
      fprintf(out, "  jmp .LF%d_%d\n", funcIdx, in->a);
      break;
    case IR_JUMP_IF_FALSE:
      fprintf(out, "  movq %s, %%rax\n  testq %%rax, %%rax\n  jz .LF%d_%d\n", ba, funcIdx, in->b);
      break;
    }
  }

  // --- epilogue --------------------------------------------------------------
  // Fall-through default return value is 0; an explicit `return` jumps PAST this
  // (preserving rax), so it only runs when control reaches the end with no return.
  fprintf(out, "  xorl %%eax, %%eax\n.Lret%d:\n", funcIdx);
  for (int r = 0; r < NUM_REGS; r++)
    if (usedReg[r])
      fprintf(out, "  movq -%d(%%rbp), %s\n", (r + 1) * 8, kRegNames[r]);
  fprintf(out, "  movq %%rbp, %%rsp\n  popq %%rbp\n  ret\n");

  free(loc);
  free(lastUse);
  free(vars);
}

bool emitX64Module(IRModule *m, FILE *out) {
  // Resolve each function's "returns a boolean?" by fixpoint (mutual recursion
  // means a call's result type can depend on a function not yet classified).
  bool *modReturnsBool = calloc(m->count, sizeof(bool));
  bool changed = true;
  while (changed) {
    changed = false;
    for (int f = 0; f < m->count; f++)
      if (!modReturnsBool[f] && funcReturnsBool(m->funcs[f], m, modReturnsBool)) {
        modReturnsBool[f] = true;
        changed = true;
      }
  }

  // Reject the whole module up front if any function is out of subset, so we
  // never emit partial assembly.
  for (int f = 0; f < m->count; f++)
    if (!functionSupported(m->funcs[f], m, modReturnsBool)) {
      free(modReturnsBool);
      return false;
    }

  fprintf(out, "  .section .rodata\n.LCfmt:\n  .string \"%%lld\\n\"\n");
  fprintf(out, "  .text\n  .globl main\n");
  for (int f = 0; f < m->count; f++)
    emitFunction(m->funcs[f], f, strcmp(m->funcs[f]->name, "main") == 0, out);
  fprintf(out, "  .section .note.GNU-stack,\"\",@progbits\n");

  free(modReturnsBool);
  return true;
}
