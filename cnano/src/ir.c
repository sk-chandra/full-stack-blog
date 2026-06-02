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
    emit(fn, (IRInstr){op, -1, condTemp, label, NIL_VAL, NULL, 0, false, NULL, 0});
  else
    emit(fn, (IRInstr){op, -1, label, -1, NIL_VAL, NULL, 0, false, NULL, 0});
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
    emit(fn, (IRInstr){IR_CONST, t, -1, -1, v, NULL, 0, false, NULL, 0});
    return t;
  }
  case NODE_VAR_GET: {
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_LOAD, t, -1, -1, NIL_VAL, node->as.name, 0, false, NULL, 0});
    return t;
  }
  case NODE_ASSIGN: {
    int v = lowerExpr(fn, node->as.var.value);
    if (v < 0)
      return -1;
    emit(fn, (IRInstr){IR_STORE, -1, v, -1, NIL_VAL, node->as.var.name, 0, false, NULL, 0});
    return v; // assignment yields the stored value
  }
  case NODE_UNARY: {
    int o = lowerExpr(fn, node->as.unary.operand);
    if (o < 0)
      return -1;
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_UNARY, t, o, -1, NIL_VAL, NULL, node->as.unary.op, false, NULL, 0});
    return t;
  }
  case NODE_BINARY: {
    int a = lowerExpr(fn, node->as.binary.left);
    int b = lowerExpr(fn, node->as.binary.right);
    if (a < 0 || b < 0)
      return -1;
    int t = freshTemp(fn);
    emit(fn, (IRInstr){IR_BINARY, t, a, b, NIL_VAL, NULL, node->as.binary.op, false, NULL, 0});
    return t;
  }
  case NODE_CALL: {
    // A direct call to a NAMED function (no first-class/indirect calls here).
    if (node->as.call.callee->type != NODE_VAR_GET)
      return -1;
    int argc = node->as.call.argCount;
    int *args = argc > 0 ? malloc(sizeof(int) * argc) : NULL;
    for (int i = 0; i < argc; i++) {
      args[i] = lowerExpr(fn, node->as.call.args[i]);
      if (args[i] < 0) { free(args); return -1; }
    }
    int t = freshTemp(fn);
    IRInstr in = {IR_CALL, t, -1, -1, NIL_VAL,
                  node->as.call.callee->as.name, 0, false, args, argc};
    emit(fn, in);
    return t;
  }
  default:
    return -1; // indexing, logicals, lambdas, etc. — out of the subset
  }
}

// Lower a single statement; return false if it isn't straight-line scalar code.
static bool lowerStmt(IRFunc *fn, Node *node) {
  switch (node->type) {
  case NODE_VAR_DECL: {
    int v = lowerExpr(fn, node->as.var.value);
    if (v < 0)
      return false;
    emit(fn, (IRInstr){IR_STORE, -1, v, -1, NIL_VAL, node->as.var.name, 0, false, NULL, 0});
    return true;
  }
  case NODE_EXPR_STMT:
    return lowerExpr(fn, node->as.stmt.expr) >= 0;
  case NODE_PRINT: {
    int v = lowerExpr(fn, node->as.stmt.expr);
    if (v < 0)
      return false;
    emit(fn, (IRInstr){IR_PRINT, -1, v, -1, NIL_VAL, NULL, 0, false, NULL, 0});
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
  case NODE_RETURN: {
    if (node->as.ret.value != NULL) {
      int v = lowerExpr(fn, node->as.ret.value);
      if (v < 0)
        return false;
      emit(fn, (IRInstr){IR_RETURN, -1, v, -1, NIL_VAL, NULL, 0, false, NULL, 0});
    } else {
      emit(fn, (IRInstr){IR_RETURN, -1, -1, -1, NIL_VAL, NULL, 0, false, NULL, 0});
    }
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

static IRFunc *newIRFunc(const char *name, ObjString **params, int paramCount) {
  IRFunc *fn = malloc(sizeof(IRFunc));
  if (fn == NULL)
    return NULL;
  fn->code = NULL;
  fn->count = fn->capacity = 0;
  fn->nextTemp = 0;
  fn->nextLabel = 0;
  fn->name = name;
  fn->params = params;
  fn->paramCount = paramCount;
  return fn;
}

IRFunc *lowerToIR(Program *body, const char *name) {
  IRFunc *fn = newIRFunc(name, NULL, 0);
  if (fn == NULL)
    return NULL;
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
  for (int i = 0; i < fn->count; i++)
    free(fn->code[i].callArgs);
  free(fn->code);
  free(fn);
}

// Lower one function body (params already on `fn`). A NESTED function definition
// is unsupported (it would need closures), so it bails.
static bool lowerBody(IRFunc *fn, Program *body) {
  for (int i = 0; i < body->count; i++)
    if (!lowerStmt(fn, body->statements[i]))
      return false;
  return true;
}

IRModule *lowerModule(Program *program) {
  IRModule *m = malloc(sizeof(IRModule));
  if (m == NULL)
    return NULL;
  m->funcs = NULL;
  m->count = 0;
  int cap = 0;

  // One IRFunc per top-level function definition.
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type != NODE_FUN)
      continue;
    IRFunc *f = newIRFunc(s->as.fun.name ? s->as.fun.name->chars : "fn",
                          s->as.fun.params, s->as.fun.paramCount);
    if (f == NULL || !lowerBody(f, s->as.fun.body)) {
      freeIR(f);
      freeModule(m);
      return NULL;
    }
    if (m->count + 1 > cap) { cap = cap < 4 ? 4 : cap * 2; m->funcs = realloc(m->funcs, sizeof(IRFunc *) * cap); }
    m->funcs[m->count++] = f;
  }

  // The top-level code becomes "main" (skipping the function declarations).
  IRFunc *main = newIRFunc("main", NULL, 0);
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type == NODE_FUN)
      continue;
    if (!lowerStmt(main, s)) {
      freeIR(main);
      freeModule(m);
      return NULL;
    }
  }
  if (m->count + 1 > cap) { cap = cap < 4 ? 4 : cap * 2; m->funcs = realloc(m->funcs, sizeof(IRFunc *) * cap); }
  m->funcs[m->count++] = main; // main is emitted last
  return m;
}

void freeModule(IRModule *m) {
  if (m == NULL)
    return;
  for (int i = 0; i < m->count; i++)
    freeIR(m->funcs[i]);
  free(m->funcs);
  free(m);
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
    case IR_CALL:
      printf("  t%d = %s(", in->dest, in->var->chars);
      for (int k = 0; k < in->callArgCount; k++)
        printf("%st%d", k ? ", " : "", in->callArgs[k]);
      printf(")\n");
      break;
    case IR_RETURN:
      if (in->a >= 0)
        printf("  return t%d\n", in->a);
      else
        printf("  return\n");
      break;
    }
  }
  printf("\n");
}
