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
// the live ranges, and which to load when emitting.)
static bool readsA(const IRInstr *in) {
  return in->op == IR_STORE || in->op == IR_PRINT || in->op == IR_UNARY ||
         in->op == IR_BINARY;
}
static bool readsB(const IRInstr *in) { return in->op == IR_BINARY; }

// Reject anything outside the integer subset: a non-int constant, or an op whose
// result is a boolean (logical-not, comparisons). Everything else the IR can
// produce here is integer-valued.
static bool supported(IRFunc *fn) {
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    if (in->op == IR_CONST && !IS_INT(in->constant))
      return false;
    if (in->op == IR_UNARY && in->nodeOp == OP_NODE_NOT)
      return false;
    if (in->op == IR_BINARY &&
        (in->nodeOp == OP_NODE_EQUAL || in->nodeOp == OP_NODE_LESS ||
         in->nodeOp == OP_NODE_GREATER))
      return false;
  }
  return true;
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
  if (!supported(fn))
    return false;

  int n = fn->nextTemp;
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

  // --- collect the distinct variables (each gets a stack slot) ---------------
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
    case IR_CONST:
      // 64-bit immediate -> the destination (via rax if the dest is a memory slot).
      if (loc[in->dest].kind == L_REG)
        fprintf(out, "  movabsq $%lld, %s\n", (long long)AS_INT(in->constant), bd);
      else
        fprintf(out, "  movabsq $%lld, %%rax\n  movq %%rax, %s\n",
                (long long)AS_INT(in->constant), bd);
      break;
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
      default: break; // comparisons were rejected by supported()
      }
      fprintf(out, "  movq %%rax, %s\n", bd);
      break;
    case IR_PRINT:
      fprintf(out, "  leaq .LCfmt(%%rip), %%rdi\n  movq %s, %%rsi\n", ba);
      fprintf(out, "  xorl %%eax, %%eax\n  call printf@PLT\n");
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
  return true;
}
