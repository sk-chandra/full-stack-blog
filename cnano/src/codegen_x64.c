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

// Does instruction `in` read temporary in a / in b? (Defines which operands form
// the live ranges, and which to load when emitting.) IR_JUMP_IF_FALSE reads its
// condition temp `a`; its `b` is a LABEL, not a temp.
static bool readsA(const IRInstr *in) {
  return in->op == IR_STORE || in->op == IR_PRINT || in->op == IR_UNARY ||
         in->op == IR_BINARY || in->op == IR_JUMP_IF_FALSE;
}
static bool readsB(const IRInstr *in) { return in->op == IR_BINARY; }

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
                          bool *varIsBool, int varCount) {
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
      else if (in->op == IR_LOAD) {
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

bool emitX64(IRFunc *fn, FILE *out) {
  int n = fn->nextTemp;

  // Collect the distinct variables (each gets a stack slot) — needed before we
  // can classify which temporaries are boolean.
  ObjString **vars = NULL;
  int varCount = 0, varCap = 0;
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead || (in->op != IR_LOAD && in->op != IR_STORE))
      continue;
    bool seen = false;
    for (int v = 0; v < varCount; v++)
      if (vars[v] == in->var) { seen = true; break; }
    if (!seen) {
      if (varCount + 1 > varCap) { varCap = varCap < 8 ? 8 : varCap * 2; vars = realloc(vars, sizeof(ObjString *) * varCap); }
      vars[varCount++] = in->var;
    }
  }

  // Classify temps as bool/int, then reject what this integer backend can't model
  // faithfully: a non-int/bool constant (float/nil), PRINTING a boolean (the VM
  // prints true/false, not 0/1), or BRANCHING on a non-boolean (cnano's int
  // truthiness — 0 is truthy — would disagree with a zero-test).
  bool *isBool = n > 0 ? calloc(n, sizeof(bool)) : NULL;
  bool *varIsBool = varCount > 0 ? calloc(varCount, sizeof(bool)) : NULL;
  classifyBools(fn, isBool, vars, varIsBool, varCount);
  bool ok = true;
  for (int i = 0; i < fn->count && ok; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (in->op == IR_CONST && !IS_INT(in->constant) && !IS_BOOL(in->constant))
      ok = false;
    else if (in->op == IR_PRINT && in->a >= 0 && isBool[in->a])
      ok = false;
    else if (in->op == IR_JUMP_IF_FALSE && in->a >= 0 && !isBool[in->a])
      ok = false;
  }
  if (!ok) {
    free(vars);
    free(isBool);
    free(varIsBool);
    return false;
  }

  Loc *loc = n > 0 ? calloc(n, sizeof(Loc)) : NULL;
  int *lastUse = n > 0 ? malloc(sizeof(int) * n) : NULL;
  for (int i = 0; i < n; i++)
    lastUse[i] = -1;

  // --- live ranges: the last instruction index that reads each temp ----------
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (readsA(in) && in->a >= 0)
      lastUse[in->a] = i;
    if (readsB(in) && in->b >= 0)
      lastUse[in->b] = i;
  }

  // --- linear-scan allocation ------------------------------------------------
  int freeRegs[NUM_REGS];
  int freeCount = NUM_REGS;
  for (int i = 0; i < NUM_REGS; i++)
    freeRegs[i] = NUM_REGS - 1 - i; // hand out rbx first
  int active[NUM_REGS]; // temps currently holding a register
  int activeCount = 0;
  int spillCount = 0;
  bool usedReg[NUM_REGS] = {false};

  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead || in->dest < 0)
      continue;
    // Expire temps whose last use is strictly before this instruction.
    for (int k = activeCount - 1; k >= 0; k--) {
      int t = active[k];
      if (lastUse[t] < i) {
        freeRegs[freeCount++] = loc[t].idx; // return its register
        active[k] = active[--activeCount];
      }
    }
    int d = in->dest;
    if (lastUse[d] < i)
      lastUse[d] = i; // a value used immediately (or never) still lives at its def
    if (freeCount > 0) {
      int r = freeRegs[--freeCount];
      loc[d] = (Loc){L_REG, r};
      usedReg[r] = true;
      active[activeCount++] = d;
    } else {
      // No free register: spill the active temp whose range ends LATEST, unless
      // the new temp itself ends even later (then spill the new one instead).
      int victimSlot = 0;
      for (int k = 1; k < activeCount; k++)
        if (lastUse[active[k]] > lastUse[active[victimSlot]])
          victimSlot = k;
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

  // Frame: NUM_REGS reserved register-save slots + variables + spills, 16-aligned.
  int slots = NUM_REGS + varCount + spillCount;
  int frame = ((slots * 8) + 15) & ~15;

  // --- emit ------------------------------------------------------------------
  fprintf(out, "  .section .rodata\n.LCfmt:\n  .string \"%%lld\\n\"\n");
  fprintf(out, "  .text\n  .globl main\nmain:\n");
  fprintf(out, "  pushq %%rbp\n  movq %%rsp, %%rbp\n");
  if (frame > 0)
    fprintf(out, "  subq $%d, %%rsp\n", frame);
  for (int r = 0; r < NUM_REGS; r++)
    if (usedReg[r]) // preserve only the callee-saved registers we actually clobber
      fprintf(out, "  movq %s, -%d(%%rbp)\n", kRegNames[r], (r + 1) * 8);

  char ba[32], bb[32], bd[32];
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (in->dest >= 0)
      tempOperand(loc[in->dest], NUM_REGS, varCount, bd, sizeof(bd));
    if (in->a >= 0 && readsA(in))
      tempOperand(loc[in->a], NUM_REGS, varCount, ba, sizeof(ba));
    if (in->b >= 0 && readsB(in))
      tempOperand(loc[in->b], NUM_REGS, varCount, bb, sizeof(bb));

    switch (in->op) {
    case IR_CONST: {
      // A bool constant is materialised as the integer 0/1.
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
      case OP_NODE_DIV: fprintf(out, "  cqto\n  idivq %s\n", bb); break; // quotient -> rax
      case OP_NODE_MOD: fprintf(out, "  cqto\n  idivq %s\n  movq %%rdx, %%rax\n", bb); break;
      // Comparisons: cmp, then set the 0/1 result with the matching condition code.
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
    case IR_LABEL:
      fprintf(out, ".Lbl%d:\n", in->a);
      break;
    case IR_JUMP:
      fprintf(out, "  jmp .Lbl%d\n", in->a);
      break;
    case IR_JUMP_IF_FALSE:
      // The condition is a boolean (0/1): branch when it is zero (false).
      fprintf(out, "  movq %s, %%rax\n  testq %%rax, %%rax\n  jz .Lbl%d\n", ba, in->b);
      break;
    }
  }

  // Epilogue: restore the clobbered callee-saved registers, return 0.
  for (int r = 0; r < NUM_REGS; r++)
    if (usedReg[r])
      fprintf(out, "  movq -%d(%%rbp), %s\n", (r + 1) * 8, kRegNames[r]);
  fprintf(out, "  movq %%rbp, %%rsp\n  popq %%rbp\n  xorl %%eax, %%eax\n  ret\n");
  // Mark the stack non-executable (modern toolchains warn without this).
  fprintf(out, "  .section .note.GNU-stack,\"\",@progbits\n");

  free(loc);
  free(lastUse);
  free(vars);
  free(isBool);
  free(varIsBool);
  return true;
}
