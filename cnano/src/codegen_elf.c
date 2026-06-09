// codegen_elf.c — emit a native ELF executable directly. See codegen_elf.h.
//
// Three layers, bottom-up:
//   1. ENCODING: tiny helpers that append x86-64 instruction bytes to a buffer
//      (REX.W prefix, opcode, ModRM, displacement/immediate). Each helper is
//      one instruction form — exactly what an assembler's tables hold.
//   2. LABELS & FIXUPS: jumps, branches and calls are emitted with placeholder
//      rel32 offsets and recorded; once every target's offset is known, the
//      placeholders are patched. This is the SAME backpatching the bytecode
//      compiler does for its jumps — at the machine level it is also most of
//      what a linker does.
//   3. THE FILE: an ELF64 header, two PT_LOAD program headers (R+X for the
//      code, R+W for data + a scratch buffer), and the bytes. Because we are
//      our own linker, data addresses are known absolute virtual addresses —
//      no relocations needed anywhere.
//
// Code generation itself is deliberately the SIMPLEST possible: every temp and
// variable lives in a stack slot (no register allocation — that lesson lives in
// --asm); rax/rcx/rdx are scratch. `print` converts the integer to decimal by
// hand and issues a raw write(2) syscall; the process ends with exit(2).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // chmod

#include "codegen_elf.h"
#include "object.h" // ObjString->chars
#include "tclass.h"

// --- layout constants --------------------------------------------------------
#define CODE_VADDR 0x400000ULL // where the R+X segment (headers + code) maps
#define DATA_VADDR 0x600000ULL // where the R+W segment (strings + scratch) maps
#define HDR_SIZE (64 + 2 * 56) // Ehdr + two Phdrs; code starts right after
#define SCRATCH_LEN 32         // the decimal-conversion buffer (in the RW seg)

// The RW data segment's fixed contents: the two boolean words (with newlines).
static const char kData[] = "true\nfalse\n";
#define TRUE_VADDR (DATA_VADDR + 0)
#define FALSE_VADDR (DATA_VADDR + 5)
#define SCRATCH_VADDR (DATA_VADDR + sizeof(kData)) // scratch right after
#define SCRATCH_END (SCRATCH_VADDR + SCRATCH_LEN)

// --- 1. the byte buffer and instruction encoders ------------------------------
typedef struct {
  uint8_t *b;
  int len, cap;
} Buf;

static void put8(Buf *o, uint8_t v) {
  if (o->len + 1 > o->cap) {
    o->cap = o->cap < 256 ? 256 : o->cap * 2;
    o->b = realloc(o->b, o->cap);
    if (o->b == NULL) {
      fprintf(stderr, "cnano: out of memory emitting code\n");
      exit(70);
    }
  }
  o->b[o->len++] = v;
}
static void put32(Buf *o, uint32_t v) {
  for (int i = 0; i < 4; i++)
    put8(o, (v >> (8 * i)) & 0xFF);
}
static void put64(Buf *o, uint64_t v) {
  for (int i = 0; i < 8; i++)
    put8(o, (v >> (8 * i)) & 0xFF);
}

// Register numbers (the low 3 bits go in ModRM; bit 3 rides in REX.B/REX.R).
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
       R8 = 8, R9 = 9 };

// REX.W prefix for a 64-bit op on `reg` (ModRM.reg) and `rm` (ModRM.rm).
static void rexWRR(Buf *o, int reg, int rm) {
  put8(o, 0x48 | ((reg >> 3) << 2) | (rm >> 3));
}
static void modrmRR(Buf *o, int reg, int rm) { // mod=11: register-register
  put8(o, 0xC0 | ((reg & 7) << 3) | (rm & 7));
}
static void modrmMem(Buf *o, int reg, int32_t disp) { // mod=10 rm=rbp: [rbp+disp32]
  put8(o, 0x80 | ((reg & 7) << 3) | RBP);
  put32(o, (uint32_t)disp);
}

// mov reg, imm64 (REX.W B8+rd io)
static void movImm64(Buf *o, int reg, uint64_t v) {
  put8(o, 0x48 | (reg >> 3));
  put8(o, 0xB8 + (reg & 7));
  put64(o, v);
}
// mov reg, [rbp+disp] / mov [rbp+disp], reg
static void loadSlot(Buf *o, int reg, int32_t disp) {
  rexWRR(o, reg, RBP); put8(o, 0x8B); modrmMem(o, reg, disp);
}
static void storeSlot(Buf *o, int32_t disp, int reg) {
  rexWRR(o, reg, RBP); put8(o, 0x89); modrmMem(o, reg, disp);
}
// mov dst, src (register-register)
static void movRR(Buf *o, int dst, int src) {
  rexWRR(o, src, dst); put8(o, 0x89); modrmRR(o, src, dst);
}
// ALU op (add 01, sub 29, and 21, or 09, xor 31, cmp 39, test 85): dst OP= src
static void aluRR(Buf *o, uint8_t opcode, int dst, int src) {
  rexWRR(o, src, dst); put8(o, opcode); modrmRR(o, src, dst);
}
// imul dst, src (REX.W 0F AF /r — note reg=dst here, unlike the ALU group)
static void imulRR(Buf *o, int dst, int src) {
  rexWRR(o, dst, src); put8(o, 0x0F); put8(o, 0xAF); modrmRR(o, dst, src);
}
// One-operand group F7: neg /3, not /2, div /6, idiv /7 — on a register.
static void grpF7(Buf *o, int ext, int rm) {
  rexWRR(o, 0, rm); put8(o, 0xF7); put8(o, 0xC0 | (ext << 3) | (rm & 7));
}
// setcc al (0F 9x C0) then movzx rax, al (REX.W 0F B6 C0)
static void setccToRax(Buf *o, uint8_t cc) {
  put8(o, 0x0F); put8(o, cc); put8(o, 0xC0);
  put8(o, 0x48); put8(o, 0x0F); put8(o, 0xB6); put8(o, 0xC0);
}

// --- 2. labels and fixups -----------------------------------------------------
typedef struct {
  int site;  // offset of the rel32 to patch (the 4 bytes at b[site..site+4))
  int label; // label id (per function), or function index for calls
} Fixup;

typedef struct {
  Buf code;
  int *labelOff;  // per active function: label id -> code offset (-1 unset)
  int labelCap;
  Fixup *jumps;   // pending intra-function jumps (label-relative)
  int jumpCount, jumpCap;
  Fixup *calls;   // pending calls (function-index-relative)
  int callCount, callCap;
  int *funcOff;   // function index -> code offset
  int helperPrintInt; // code offsets of the two runtime helpers
  int helperPrintStr;
} Emitter;

static void addFixup(Fixup **arr, int *count, int *cap, int site, int label) {
  if (*count + 1 > *cap) {
    *cap = *cap < 8 ? 8 : *cap * 2;
    *arr = realloc(*arr, sizeof(Fixup) * (*cap));
  }
  (*arr)[(*count)++] = (Fixup){site, label};
}

static void defineLabel(Emitter *e, int label) {
  if (label >= e->labelCap) {
    int newCap = label + 16;
    e->labelOff = realloc(e->labelOff, sizeof(int) * newCap);
    for (int i = e->labelCap; i < newCap; i++)
      e->labelOff[i] = -1;
    e->labelCap = newCap;
  }
  e->labelOff[label] = e->code.len;
}

// jmp rel32 (E9) / jcc rel32 (0F 8x) / call rel32 (E8), targets patched later.
static void emitJmp(Emitter *e, int label) {
  put8(&e->code, 0xE9);
  addFixup(&e->jumps, &e->jumpCount, &e->jumpCap, e->code.len, label);
  put32(&e->code, 0);
}
static void emitJcc(Emitter *e, uint8_t cc, int label) {
  put8(&e->code, 0x0F); put8(&e->code, cc);
  addFixup(&e->jumps, &e->jumpCount, &e->jumpCap, e->code.len, label);
  put32(&e->code, 0);
}
static void emitCallFixed(Emitter *e, int codeOff) { // target offset known now
  put8(&e->code, 0xE8);
  put32(&e->code, (uint32_t)(codeOff - (e->code.len + 4)));
}
static void emitCallFunc(Emitter *e, int funcIdx) { // patched once laid out
  put8(&e->code, 0xE8);
  addFixup(&e->calls, &e->callCount, &e->callCap, e->code.len, funcIdx);
  put32(&e->code, 0);
}

// Patch this function's pending intra-function jumps, then reset for the next.
static void patchJumps(Emitter *e) {
  for (int i = 0; i < e->jumpCount; i++) {
    int target = e->labelOff[e->jumps[i].label];
    int32_t rel = target - (e->jumps[i].site + 4);
    memcpy(&e->code.b[e->jumps[i].site], &rel, 4);
  }
  e->jumpCount = 0;
  for (int i = 0; i < e->labelCap; i++)
    e->labelOff[i] = -1;
}

// --- the runtime helpers (hand-emitted once, before any function) -------------
// print_i64: rax = the value. Builds the decimal text BACKWARD from the end of
// the scratch buffer (newline first), prepends '-' for negatives, then issues
// write(1, start, len). NEG on INT64_MIN leaves it unchanged — which is exactly
// the right unsigned magnitude, so the edge case costs nothing.
static void emitPrintInt(Emitter *e, int lblPos, int lblLoop, int lblNoSign) {
  Buf *o = &e->code;
  e->helperPrintInt = o->len;
  movImm64(o, R9, SCRATCH_END);
  // r8 = sign flag; place the newline (built backward, printed forward)
  put8(o, 0x49); put8(o, 0xFF); put8(o, 0xC9);             // dec r9
  put8(o, 0x41); put8(o, 0xC6); put8(o, 0x01); put8(o, 10); // mov byte [r9], '\n'
  movImm64(o, R8, 0);
  aluRR(o, 0x85, RAX, RAX);                                // test rax, rax
  emitJcc(e, 0x89, lblPos);                                // jns .pos
  movImm64(o, R8, 1);
  grpF7(o, 3, RAX);                                        // neg rax
  defineLabel(e, lblPos);
  defineLabel(e, lblLoop);
  put8(o, 0x31); put8(o, 0xD2);                            // xor edx, edx
  movImm64(o, RCX, 10);
  grpF7(o, 6, RCX);                                        // div rcx (unsigned)
  put8(o, 0x80); put8(o, 0xC2); put8(o, '0');              // add dl, '0'
  put8(o, 0x49); put8(o, 0xFF); put8(o, 0xC9);             // dec r9
  put8(o, 0x41); put8(o, 0x88); put8(o, 0x11);             // mov [r9], dl
  aluRR(o, 0x85, RAX, RAX);                                // test rax, rax
  emitJcc(e, 0x85, lblLoop);                               // jnz .loop
  put8(o, 0x4D); put8(o, 0x85); put8(o, 0xC0);             // test r8, r8
  emitJcc(e, 0x84, lblNoSign);                             // jz .nosign
  put8(o, 0x49); put8(o, 0xFF); put8(o, 0xC9);             // dec r9
  put8(o, 0x41); put8(o, 0xC6); put8(o, 0x01); put8(o, '-'); // mov byte [r9],'-'
  defineLabel(e, lblNoSign);
  movImm64(o, RDX, SCRATCH_END);
  put8(o, 0x4C); put8(o, 0x29); put8(o, 0xCA);             // sub rdx, r9 (len)
  put8(o, 0x4C); put8(o, 0x89); put8(o, 0xCE);             // mov rsi, r9 (ptr)
  movImm64(o, RDI, 1);                                     // fd = stdout
  movImm64(o, RAX, 1);                                     // SYS_write
  put8(o, 0x0F); put8(o, 0x05);                            // syscall
  put8(o, 0xC3);                                           // ret
  patchJumps(e);
}

// print_str: rsi = pointer, rdx = length. write(1, rsi, rdx); ret.
static void emitPrintStr(Emitter *e) {
  Buf *o = &e->code;
  e->helperPrintStr = o->len;
  movImm64(o, RDI, 1);
  movImm64(o, RAX, 1);
  put8(o, 0x0F); put8(o, 0x05); // syscall
  put8(o, 0xC3);                // ret
}

// --- per-function emission -----------------------------------------------------
// Slot layout: variable v at [rbp - 8(v+1)]; temp t at [rbp - 8(varCount+t+1)].
static int32_t varDisp(int v) { return -8 * (v + 1); }
static int32_t tempDisp(FnInfo *info, int t) {
  return -8 * (info->varCount + t + 1);
}

static const char *kArgRegsElf[6] = {0}; // (names unused; numbers below)
static const int kArgRegNum[6] = {RDI, RSI, RDX, RCX, R8, R9};

static void emitElfFunction(IRModule *m, int fi, FnInfo *infos, Emitter *e,
                            bool isMain) {
  IRFunc *fn = m->funcs[fi];
  FnInfo *info = &infos[fi];
  Buf *o = &e->code;
  e->funcOff[fi] = o->len;
  int synth = fn->nextLabel; // synthetic labels for bool-print branches

  // Prologue: push rbp; mov rbp, rsp; sub rsp, frame
  put8(o, 0x55);
  put8(o, 0x48); put8(o, 0x89); put8(o, 0xE5);
  int frame = ((info->varCount + fn->nextTemp) * 8 + 15) & ~15;
  if (frame > 0) {
    put8(o, 0x48); put8(o, 0x81); put8(o, 0xEC); put32(o, (uint32_t)frame);
  }
  // Spill incoming arguments (our internal convention: rdi/rsi/rdx/rcx/r8/r9).
  for (int p = 0; p < fn->paramCount; p++)
    storeSlot(o, varDisp(varIndexIn(fn->params[p], info)), kArgRegNum[p]);

  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    switch (in->op) {
    case IR_CONST: {
      uint64_t v = IS_BOOL(in->constant) ? (AS_BOOL(in->constant) ? 1 : 0)
                                         : (uint64_t)AS_INT(in->constant);
      movImm64(o, RAX, v);
      storeSlot(o, tempDisp(info, in->dest), RAX);
      break;
    }
    case IR_LOAD:
      loadSlot(o, RAX, varDisp(varIndexIn(in->var, info)));
      storeSlot(o, tempDisp(info, in->dest), RAX);
      break;
    case IR_STORE:
      loadSlot(o, RAX, tempDisp(info, in->a));
      storeSlot(o, varDisp(varIndexIn(in->var, info)), RAX);
      break;
    case IR_UNARY:
      loadSlot(o, RAX, tempDisp(info, in->a));
      if (in->nodeOp == OP_NODE_NOT) { // flip the 0/1 bit, not all the bits
        put8(o, 0x48); put8(o, 0x83); put8(o, 0xF0); put8(o, 0x01); // xor rax,1
      } else {
        grpF7(o, in->nodeOp == OP_NODE_NEGATE ? 3 : 2, RAX); // neg / not
      }
      storeSlot(o, tempDisp(info, in->dest), RAX);
      break;
    case IR_BINARY:
      loadSlot(o, RAX, tempDisp(info, in->a));
      loadSlot(o, RCX, tempDisp(info, in->b));
      switch (in->nodeOp) {
      case OP_NODE_ADD: aluRR(o, 0x01, RAX, RCX); break;
      case OP_NODE_SUB: aluRR(o, 0x29, RAX, RCX); break;
      case OP_NODE_MUL: imulRR(o, RAX, RCX); break;
      case OP_NODE_BITAND: aluRR(o, 0x21, RAX, RCX); break;
      case OP_NODE_BITOR: aluRR(o, 0x09, RAX, RCX); break;
      case OP_NODE_BITXOR: aluRR(o, 0x31, RAX, RCX); break;
      case OP_NODE_SHL: // sal rax, cl (48 D3 E0)
        put8(o, 0x48); put8(o, 0xD3); put8(o, 0xE0); break;
      case OP_NODE_SHR: // sar rax, cl (48 D3 F8)
        put8(o, 0x48); put8(o, 0xD3); put8(o, 0xF8); break;
      case OP_NODE_DIV:
        put8(o, 0x48); put8(o, 0x99); // cqo
        grpF7(o, 7, RCX);             // idiv rcx -> quotient in rax
        break;
      case OP_NODE_MOD:
        put8(o, 0x48); put8(o, 0x99);
        grpF7(o, 7, RCX);
        movRR(o, RAX, RDX); // remainder
        break;
      case OP_NODE_EQUAL:   aluRR(o, 0x39, RAX, RCX); setccToRax(o, 0x94); break;
      case OP_NODE_LESS:    aluRR(o, 0x39, RAX, RCX); setccToRax(o, 0x9C); break;
      case OP_NODE_GREATER: aluRR(o, 0x39, RAX, RCX); setccToRax(o, 0x9F); break;
      default: break;
      }
      storeSlot(o, tempDisp(info, in->dest), RAX);
      break;
    case IR_PRINT:
      if (info->tempClass[in->a] == TC_BOOL) {
        int lblFalse = synth++, lblEnd = synth++;
        loadSlot(o, RAX, tempDisp(info, in->a));
        aluRR(o, 0x85, RAX, RAX); // test rax, rax
        emitJcc(e, 0x84, lblFalse);
        movImm64(o, RSI, TRUE_VADDR);
        movImm64(o, RDX, 5); // "true\n"
        emitJmp(e, lblEnd);
        defineLabel(e, lblFalse);
        movImm64(o, RSI, FALSE_VADDR);
        movImm64(o, RDX, 6); // "false\n"
        defineLabel(e, lblEnd);
        emitCallFixed(e, e->helperPrintStr);
      } else {
        loadSlot(o, RAX, tempDisp(info, in->a));
        emitCallFixed(e, e->helperPrintInt);
      }
      break;
    case IR_CALL:
      for (int k = 0; k < in->callArgCount; k++)
        loadSlot(o, kArgRegNum[k], tempDisp(info, in->callArgs[k]));
      emitCallFunc(e, findFuncIndex(m, in->var));
      storeSlot(o, tempDisp(info, in->dest), RAX);
      break;
    case IR_RETURN:
      if (in->a >= 0)
        loadSlot(o, RAX, tempDisp(info, in->a));
      else
        aluRR(o, 0x31, RAX, RAX); // xor: return 0
      if (isMain) { // returning from main = exit(0): there is no caller
        movImm64(o, RDI, 0);
        movImm64(o, RAX, 60); // SYS_exit
        put8(o, 0x0F); put8(o, 0x05);
      } else {
        put8(o, 0xC9); // leave
        put8(o, 0xC3); // ret
      }
      break;
    case IR_LABEL:
      defineLabel(e, in->a);
      break;
    case IR_JUMP:
      emitJmp(e, in->a);
      break;
    case IR_JUMP_IF_FALSE:
      loadSlot(o, RAX, tempDisp(info, in->a));
      aluRR(o, 0x85, RAX, RAX); // test rax, rax
      emitJcc(e, 0x84, in->b);  // jz
      break;
    }
  }

  // Fall-through epilogue.
  if (isMain) { // end of the program: exit(0)
    movImm64(o, RDI, 0);
    movImm64(o, RAX, 60);
    put8(o, 0x0F); put8(o, 0x05);
  } else { // return 0 by default
    put8(o, 0x31); put8(o, 0xC0); // xor eax, eax
    put8(o, 0xC9); put8(o, 0xC3); // leave; ret
  }
  patchJumps(e);
}

// Same faithfulness rules as the assembly backend, minus floats (no libc = no
// %g, so float printing is out of reach here) — see tclass.h for the classes.
static bool elfSupported(IRFunc *fn, IRModule *m, FnInfo *info) {
  if (fn->paramCount > 6 || info->retClass == TC_CONFLICT ||
      info->retClass == TC_FLOAT)
    return false;
  for (int v = 0; v < info->varCount; v++)
    if (info->varClass[v] == TC_CONFLICT || info->varClass[v] == TC_FLOAT)
      return false;
  for (int t = 0; t < fn->nextTemp; t++)
    if (info->tempClass[t] == TC_CONFLICT || info->tempClass[t] == TC_FLOAT)
      return false;
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    TClass a = in->a >= 0 ? info->tempClass[in->a] : TC_UNKNOWN;
    TClass b = in->b >= 0 ? info->tempClass[in->b] : TC_UNKNOWN;
    switch (in->op) {
    case IR_CONST:
      if (IS_NIL(in->constant) || IS_FLOAT(in->constant))
        return false;
      break;
    case IR_JUMP_IF_FALSE:
      if (a != TC_BOOL)
        return false;
      break;
    case IR_UNARY:
      if (in->nodeOp == OP_NODE_NOT && a != TC_BOOL)
        return false;
      if (in->nodeOp == OP_NODE_BITNOT && a != TC_INT)
        return false;
      if (in->nodeOp == OP_NODE_NEGATE && a != TC_INT)
        return false;
      break;
    case IR_BINARY:
      if (in->nodeOp == OP_NODE_EQUAL) {
        if (a != b || a == TC_UNKNOWN)
          return false;
      } else if (!(a == TC_INT && b == TC_INT)) {
        return false; // everything else here is integer-only
      }
      break;
    case IR_CALL:
      if (findFuncIndex(m, in->var) < 0 || in->callArgCount > 6)
        return false;
      break;
    default:
      break;
    }
  }
  return true;
}

// --- 3. the ELF file -----------------------------------------------------------
static void putN(Buf *o, const void *p, int n) {
  for (int i = 0; i < n; i++)
    put8(o, ((const uint8_t *)p)[i]);
}

bool emitElfExecutable(IRModule *m, const char *path) {
  FnInfo *infos = moduleClassify(m);
  bool ok = true;
  for (int f = 0; f < m->count && ok; f++)
    ok = elfSupported(m->funcs[f], m, &infos[f]);
  if (!ok) {
    freeModuleClasses(m, infos);
    return false;
  }

  Emitter e;
  memset(&e, 0, sizeof(e));
  e.funcOff = calloc(m->count, sizeof(int));

  // Helpers first (they use the same label machinery; ids are arbitrary).
  emitPrintInt(&e, 0, 1, 2);
  emitPrintStr(&e);
  // Then every function, recording offsets; the script's `main` is last.
  int mainIdx = -1;
  for (int f = 0; f < m->count; f++) {
    bool isMain = strcmp(m->funcs[f]->name, "main") == 0;
    if (isMain)
      mainIdx = f;
    emitElfFunction(m, f, infos, &e, isMain);
  }
  // Patch every call now that all function offsets are known.
  for (int i = 0; i < e.callCount; i++) {
    int32_t rel = e.funcOff[e.calls[i].label] - (e.calls[i].site + 4);
    memcpy(&e.code.b[e.calls[i].site], &rel, 4);
  }

  // Lay out the file: [Ehdr][Phdr x2][code] … page-aligned … [data].
  int codeFileEnd = HDR_SIZE + e.code.len;
  int dataOff = (codeFileEnd + 0xFFF) & ~0xFFF;
  uint64_t entry = CODE_VADDR + HDR_SIZE + (uint64_t)e.funcOff[mainIdx];

  Buf out = {0};
  // ELF64 header.
  const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0,
                             0, 0, 0, 0, 0, 0, 0, 0};
  putN(&out, ident, 16);
  uint16_t type = 2, machine = 0x3E;        // ET_EXEC, EM_X86_64
  uint32_t version = 1;
  putN(&out, &type, 2); putN(&out, &machine, 2); putN(&out, &version, 4);
  put64(&out, entry);
  put64(&out, 64);  // e_phoff: program headers follow the Ehdr
  put64(&out, 0);   // e_shoff: no section headers — the LOADER never needs them
  put32(&out, 0);   // e_flags
  uint16_t ehsize = 64, phentsize = 56, phnum = 2, zero16 = 0;
  putN(&out, &ehsize, 2); putN(&out, &phentsize, 2); putN(&out, &phnum, 2);
  putN(&out, &zero16, 2); putN(&out, &zero16, 2); putN(&out, &zero16, 2);
  // Program header 1: R+X — the headers and code.
  uint32_t pt_load = 1, flags_rx = 5, flags_rw = 6;
  putN(&out, &pt_load, 4); putN(&out, &flags_rx, 4);
  put64(&out, 0); put64(&out, CODE_VADDR); put64(&out, CODE_VADDR);
  put64(&out, (uint64_t)codeFileEnd); put64(&out, (uint64_t)codeFileEnd);
  put64(&out, 0x1000);
  // Program header 2: R+W — the strings, plus SCRATCH_LEN of zero-fill (the
  // memsz > filesz tail is exactly how .bss works).
  putN(&out, &pt_load, 4); putN(&out, &flags_rw, 4);
  put64(&out, (uint64_t)dataOff); put64(&out, DATA_VADDR); put64(&out, DATA_VADDR);
  put64(&out, sizeof(kData)); put64(&out, sizeof(kData) + SCRATCH_LEN);
  put64(&out, 0x1000);
  // Code, padding to the data segment's page-aligned offset, then data.
  putN(&out, e.code.b, e.code.len);
  while (out.len < dataOff)
    put8(&out, 0);
  putN(&out, kData, (int)sizeof(kData));

  FILE *f = fopen(path, "wb");
  bool wrote = f != NULL && fwrite(out.b, 1, out.len, f) == (size_t)out.len;
  if (f != NULL)
    fclose(f);
  if (wrote)
    chmod(path, 0755);

  free(out.b);
  free(e.code.b);
  free(e.labelOff);
  free(e.jumps);
  free(e.calls);
  free(e.funcOff);
  freeModuleClasses(m, infos);
  (void)kArgRegsElf;
  return wrote;
}
