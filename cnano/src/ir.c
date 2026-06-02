#include <stdio.h>
#include <stdlib.h>

#include "ir.h"
#include "object.h" // ObjString fields, printValue

static int emit(IRFunc *fn, IRInstr instr) {
  if (fn->capacity < fn->count + 1) {
    fn->capacity = fn->capacity < 8 ? 8 : fn->capacity * 2;
    fn->code = realloc(fn->code, sizeof(IRInstr) * fn->capacity);
    if (fn->code == NULL) {
      fprintf(stderr, "cnano: out of memory building IR\n");
      exit(70);
    }
  }
  fn->code[fn->count] = instr;
  return fn->count++;
}

static int freshTemp(IRFunc *fn) { return fn->nextTemp++; }
static int freshLabel(IRFunc *fn) { return fn->nextLabel++; }

// Emit a label definition / an unconditional jump / a conditional jump. The
// label id rides in `a` (or `b` for the conditional, whose condition temp is `a`).
static void emitLabelOp(IRFunc *fn, IROp op, int label, int condTemp) {
  if (op == IR_JUMP_IF_FALSE)
    emit(fn, (IRInstr){op, -1, condTemp, label, NIL_VAL, NULL, 0, false});
  else
    emit(fn, (IRInstr){op, -1, label, -1, NIL_VAL, NULL, 0, false});
}

// Lower an expression, returning the temp id that holds its value, or -1 if the
// expression is outside the straight-line scalar subset (the caller then bails).
static int lowerExpr(IRFunc *fn, Node *node) {
  switch (node->type) {
  case NODE_INT:
  case NODE_FLOAT:
  case NODE_BOOL:
  case NODE_NIL: {
    Value v = node->type == NODE_INT     ? INT_VAL(node->as.intValue)
              : node->type == NODE_FLOAT ? FLOAT_VAL(node->as.floatValue)
              : node->type == NODE_BOOL  ? BOOL_VAL(node->as.boolValue)
                                         : NIL_VAL;
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_CONST, t, -1, -1, v, NULL, 0, false});
    return t;
  }
  case NODE_VAR_GET: {
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_LOAD, t, -1, -1, NIL_VAL, node->as.name, 0, false});
    return t;
  }
  case NODE_ASSIGN: {
    int v = lowerExpr(fn, node->as.var.value);
    if (v < 0)
      return -1;
    emit(fn, (IRInstr){IR_STORE, -1, v, -1, NIL_VAL, node->as.var.name, 0, false});
    return v; // assignment yields the stored value
  }
  case NODE_UNARY: {
    int o = lowerExpr(fn, node->as.unary.operand);
    if (o < 0)
      return -1;
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_UNARY, t, o, -1, NIL_VAL, NULL, node->as.unary.op, false});
    return t;
  }
  case NODE_BINARY: {
    int a = lowerExpr(fn, node->as.binary.left);
    int b = lowerExpr(fn, node->as.binary.right);
    if (a < 0 || b < 0)
      return -1;
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_BINARY, t, a, b, NIL_VAL, NULL, node->as.binary.op, false});
    return t;
  }
  default:
    return -1; // calls, indexing, logicals, etc. — out of the subset
  }
}

// Lower a single statement; return false if it isn't straight-line scalar code.
static bool lowerStmt(IRFunc *fn, Node *node) {
  switch (node->type) {
  case NODE_VAR_DECL: {
    int v = lowerExpr(fn, node->as.var.value);
    if (v < 0)
      return false;
    emit(fn, (IRInstr){IR_STORE, -1, v, -1, NIL_VAL, node->as.var.name, 0, false});
    return true;
  }
  case NODE_EXPR_STMT:
    return lowerExpr(fn, node->as.stmt.expr) >= 0;
  case NODE_PRINT: {
    int v = lowerExpr(fn, node->as.stmt.expr);
    if (v < 0)
      return false;
    emit(fn, (IRInstr){IR_PRINT, -1, v, -1, NIL_VAL, NULL, 0, false});
    return true;
  }
  case NODE_BLOCK: {
    // A block just groups statements; the IR models variables by name, so there
    // is no separate scope to open (block-local shadowing is unsupported).
    Program *b = node->as.block;
    for (int i = 0; i < b->count; i++)
      if (!lowerStmt(fn, b->statements[i]))
        return false;
    return true;
  }
  case NODE_IF: {
    // if (c) then [else otherwise]:
    //     cond; JUMP_IF_FALSE cond -> Lelse; <then>; JUMP Lend; Lelse: <else>; Lend:
    int c = lowerExpr(fn, node->as.ifStmt.condition);
    if (c < 0)
      return false;
    int lElse = freshLabel(fn), lEnd = freshLabel(fn);
    emitLabelOp(fn, IR_JUMP_IF_FALSE, lElse, c);
    if (!lowerStmt(fn, node->as.ifStmt.then))
      return false;
    emitLabelOp(fn, IR_JUMP, lEnd, -1);
    emitLabelOp(fn, IR_LABEL, lElse, -1);
    if (node->as.ifStmt.otherwise != NULL && !lowerStmt(fn, node->as.ifStmt.otherwise))
      return false;
    emitLabelOp(fn, IR_LABEL, lEnd, -1);
    return true;
  }
  case NODE_WHILE: {
    // while (c) body [increment]:
    //     Lstart: cond; JUMP_IF_FALSE cond -> Lend; <body>; <increment>; JUMP Lstart; Lend:
    int lStart = freshLabel(fn), lEnd = freshLabel(fn);
    emitLabelOp(fn, IR_LABEL, lStart, -1);
    int c = lowerExpr(fn, node->as.whileStmt.condition);
    if (c < 0)
      return false;
    emitLabelOp(fn, IR_JUMP_IF_FALSE, lEnd, c);
    if (!lowerStmt(fn, node->as.whileStmt.body))
      return false;
    if (node->as.whileStmt.increment != NULL &&
        lowerExpr(fn, node->as.whileStmt.increment) < 0)
      return false; // the `for`-loop step (desugared into the while)
    emitLabelOp(fn, IR_JUMP, lStart, -1);
    emitLabelOp(fn, IR_LABEL, lEnd, -1);
    return true;
  }
  default:
    return false; // return/fun/match/break/continue/... — not yet lowered
  }
}

IRFunc *lowerToIR(Program *body, const char *name) {
  IRFunc *fn = malloc(sizeof(IRFunc));
  if (fn == NULL)
    return NULL;
  fn->code = NULL;
  fn->count = fn->capacity = 0;
  fn->nextTemp = 0;
  fn->nextLabel = 0;
  fn->name = name;
  for (int i = 0; i < body->count; i++) {
    if (body->statements[i]->type == NODE_FUN)
      continue; // skip nested function declarations
    if (!lowerStmt(fn, body->statements[i])) {
      freeIR(fn);
      return NULL; // hit something outside the subset
    }
  }
  return fn;
}

void freeIR(IRFunc *fn) {
  if (fn == NULL)
    return;
  free(fn->code);
  free(fn);
}

static const char *opName(NodeOp op) {
  switch (op) {
  case OP_NODE_ADD: return "+";
  case OP_NODE_SUB: return "-";
  case OP_NODE_MUL: return "*";
  case OP_NODE_DIV: return "/";
  case OP_NODE_MOD: return "%";
  case OP_NODE_BITAND: return "&";
  case OP_NODE_BITOR: return "|";
  case OP_NODE_BITXOR: return "^";
  case OP_NODE_SHL: return "<<";
  case OP_NODE_SHR: return ">>";
  case OP_NODE_BITNOT: return "~";
  case OP_NODE_EQUAL: return "==";
  case OP_NODE_LESS: return "<";
  case OP_NODE_GREATER: return ">";
  case OP_NODE_NEGATE: return "-";
  case OP_NODE_NOT: return "!";
  default: return "?";
  }
}

void printIR(IRFunc *fn, const char *title) {
  printf("== IR: %s (%s) ==\n", fn->name, title);
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    switch (in->op) {
    case IR_CONST:
      printf("  t%d = const ", in->dest);
      printValue(in->constant);
      printf("\n");
      break;
    case IR_LOAD:
      printf("  t%d = load %s\n", in->dest, in->var->chars);
      break;
    case IR_STORE:
      printf("  store %s = t%d\n", in->var->chars, in->a);
      break;
    case IR_UNARY:
      printf("  t%d = %s t%d\n", in->dest, opName(in->nodeOp), in->a);
      break;
    case IR_BINARY:
      printf("  t%d = t%d %s t%d\n", in->dest, in->a, opName(in->nodeOp), in->b);
      break;
    case IR_PRINT:
      printf("  print t%d\n", in->a);
      break;
    case IR_LABEL:
      printf("L%d:\n", in->a);
      break;
    case IR_JUMP:
      printf("  goto L%d\n", in->a);
      break;
    case IR_JUMP_IF_FALSE:
      printf("  if !t%d goto L%d\n", in->a, in->b);
      break;
    }
  }
  printf("\n");
}
