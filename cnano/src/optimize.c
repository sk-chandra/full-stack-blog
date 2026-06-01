#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "optimize.h"

static int foldCount;

// Is this node a literal the folder can compute with?
static bool isIntLit(Node *n) { return n != NULL && n->type == NODE_INT; }
static bool isFloatLit(Node *n) { return n != NULL && n->type == NODE_FLOAT; }
static bool isBoolLit(Node *n) { return n != NULL && n->type == NODE_BOOL; }
static bool isStrLit(Node *n) { return n != NULL && n->type == NODE_STRING; }
// A numeric literal (int or float), and its value as a double.
static bool isNumLit(Node *n) { return isIntLit(n) || isFloatLit(n); }
static double numLit(Node *n) {
  return isIntLit(n) ? (double)n->as.intValue : n->as.floatValue;
}

static Node *foldExpr(Node *node);
static void foldProgram(Program *program); // fold a whole statement list

// A folded node that is a constant bool literal: report its value via *out and
// return true. Used to collapse conditionals whose test is statically known.
static bool constBool(Node *n, bool *out) {
  if (isBoolLit(n)) { *out = n->as.boolValue; return true; }
  return false;
}

// Try to fold a binary node whose children have already been folded. Returns a
// new literal node if it could, or NULL to leave the binary node as-is.
static Node *tryFoldBinary(Node *node) {
  Node *l = node->as.binary.left;
  Node *r = node->as.binary.right;
  int line = node->line;

  // Numeric arithmetic where at least one side is a FLOAT literal (and both are
  // numeric): fold to a float, mirroring the VM's promotion.
  if (isNumLit(l) && isNumLit(r) && (isFloatLit(l) || isFloatLit(r))) {
    double a = numLit(l), b = numLit(r);
    switch (node->as.binary.op) {
    case OP_NODE_ADD: return newFloat(a + b, line);
    case OP_NODE_SUB: return newFloat(a - b, line);
    case OP_NODE_MUL: return newFloat(a * b, line);
    case OP_NODE_DIV: return newFloat(a / b, line); // float / 0 -> inf (defined)
    case OP_NODE_LESS: return newBool(a < b, line);
    case OP_NODE_GREATER: return newBool(a > b, line);
    case OP_NODE_EQUAL: return newBool(a == b, line);
    default: return NULL; // mod/bitwise on floats are type errors, not folded
    }
  }

  // Integer arithmetic and comparisons.
  if (isIntLit(l) && isIntLit(r)) {
    int64_t a = l->as.intValue, b = r->as.intValue;
    switch (node->as.binary.op) {
    case OP_NODE_ADD: return newInt(a + b, line);
    case OP_NODE_SUB: return newInt(a - b, line);
    case OP_NODE_MUL: return newInt(a * b, line);
    case OP_NODE_DIV:
      // Preserve runtime semantics: division by zero must still error at
      // RUNTIME, so we refuse to fold it and leave the node for the VM to catch.
      if (b == 0) return NULL;
      return newInt(a / b, line);
    case OP_NODE_MOD:
      // Same reasoning as DIV: modulo by zero must error at runtime, not fold.
      if (b == 0) return NULL;
      return newInt(a % b, line);
    case OP_NODE_BITAND: return newInt(a & b, line);
    case OP_NODE_BITOR:  return newInt(a | b, line);
    case OP_NODE_BITXOR: return newInt(a ^ b, line);
    case OP_NODE_SHL:
    case OP_NODE_SHR:
      // Out-of-range shifts must error at runtime (UB in C), so don't fold them.
      if (b < 0 || b > 63) return NULL;
      return newInt(node->as.binary.op == OP_NODE_SHL
                        ? (int64_t)((uint64_t)a << b)
                        : a >> b,
                    line);
    case OP_NODE_LESS: return newBool(a < b, line);
    case OP_NODE_GREATER: return newBool(a > b, line);
    case OP_NODE_EQUAL: return newBool(a == b, line);
    default: return NULL;
    }
  }

  // String concatenation: "a" + "b" -> "ab". Uses the interning copyString.
  if (isStrLit(l) && isStrLit(r) && node->as.binary.op == OP_NODE_ADD) {
    ObjString *as = l->as.stringValue, *bs = r->as.stringValue;
    int len = as->length + bs->length;
    char *buf = malloc(len + 1);
    if (buf == NULL) return NULL; // give up folding; not fatal
    memcpy(buf, as->chars, as->length);
    memcpy(buf + as->length, bs->chars, bs->length);
    buf[len] = '\0';
    ObjString *s = copyString(buf, len);
    free(buf);
    return newString(s, line);
  }

  // Equality across like literal kinds we can decide statically.
  if (node->as.binary.op == OP_NODE_EQUAL) {
    if (isBoolLit(l) && isBoolLit(r))
      return newBool(l->as.boolValue == r->as.boolValue, line);
    if (isStrLit(l) && isStrLit(r)) // interned: pointer identity is equality
      return newBool(l->as.stringValue == r->as.stringValue, line);
  }
  return NULL;
}

// Fold a unary node whose operand is already folded.
static Node *tryFoldUnary(Node *node) {
  Node *o = node->as.unary.operand;
  int line = node->line;
  if (node->as.unary.op == OP_NODE_NEGATE && isIntLit(o))
    return newInt(-o->as.intValue, line);
  if (node->as.unary.op == OP_NODE_NEGATE && isFloatLit(o))
    return newFloat(-o->as.floatValue, line);
  if (node->as.unary.op == OP_NODE_BITNOT && isIntLit(o))
    return newInt(~o->as.intValue, line);
  if (node->as.unary.op == OP_NODE_NOT) {
    // cnano truthiness: only nil and false are falsey. Mirror it exactly so the
    // folded result matches what the VM would have produced.
    if (isBoolLit(o)) return newBool(!o->as.boolValue, line);
    if (o != NULL && o->type == NODE_NIL) return newBool(true, line);
    if (isIntLit(o) || isStrLit(o)) return newBool(false, line); // truthy -> !x is false
  }
  return NULL;
}

// Recursively fold an expression, returning the (possibly new) node. The pattern
// is bottom-up: fold children first by recursing into their slots, then attempt
// to collapse this node now that its children may be literals.
static Node *foldExpr(Node *node) {
  if (node == NULL) return NULL;
  switch (node->type) {
  case NODE_UNARY: {
    node->as.unary.operand = foldExpr(node->as.unary.operand);
    Node *folded = tryFoldUnary(node);
    if (folded != NULL) { freeNode(node); foldCount++; return folded; }
    return node;
  }
  case NODE_BINARY: {
    node->as.binary.left = foldExpr(node->as.binary.left);
    node->as.binary.right = foldExpr(node->as.binary.right);
    Node *folded = tryFoldBinary(node);
    if (folded != NULL) { freeNode(node); foldCount++; return folded; }
    return node;
  }
  case NODE_LOGICAL: {
    node->as.logical.left = foldExpr(node->as.logical.left);
    node->as.logical.right = foldExpr(node->as.logical.right);
    // With a constant-bool LEFT we can collapse, and this MATCHES short-circuit
    // semantics exactly (so it never changes which side effects run): `a and b`
    // is `b` when a is true and `a` (=false) when a is false; `a or b` is `a`
    // (=true) when a is true and `b` when a is false.
    bool lv;
    if (constBool(node->as.logical.left, &lv)) {
      bool takeRight = node->as.logical.isAnd ? lv : !lv;
      Node *keep = takeRight ? node->as.logical.right : node->as.logical.left;
      // Detach the kept child so freeing the logical node doesn't free it.
      if (takeRight) node->as.logical.right = NULL;
      else node->as.logical.left = NULL;
      freeNode(node);
      foldCount++;
      return keep;
    }
    return node;
  }
  case NODE_COND: {
    // `c ? a : b` — fold the parts, then collapse if the test is a known bool.
    // Only the taken branch would ever have run, so dropping the other is safe.
    node->as.ifStmt.condition = foldExpr(node->as.ifStmt.condition);
    node->as.ifStmt.then = foldExpr(node->as.ifStmt.then);
    node->as.ifStmt.otherwise = foldExpr(node->as.ifStmt.otherwise);
    bool cv;
    if (constBool(node->as.ifStmt.condition, &cv)) {
      Node *keep = cv ? node->as.ifStmt.then : node->as.ifStmt.otherwise;
      if (cv) node->as.ifStmt.then = NULL;
      else node->as.ifStmt.otherwise = NULL;
      freeNode(node);
      foldCount++;
      return keep;
    }
    return node;
  }
  case NODE_FUN:
    // A lambda expression: fold constants inside its body (named functions get
    // this via foldStatement; lambdas reach here as expressions).
    foldProgram(node->as.fun.body);
    return node;
  case NODE_ASSIGN:
    node->as.var.value = foldExpr(node->as.var.value);
    return node;
  case NODE_CALL:
    node->as.call.callee = foldExpr(node->as.call.callee);
    for (int i = 0; i < node->as.call.argCount; i++)
      node->as.call.args[i] = foldExpr(node->as.call.args[i]);
    return node;
  case NODE_INVOKE:
    node->as.invoke.receiver = foldExpr(node->as.invoke.receiver);
    for (int i = 0; i < node->as.invoke.argCount; i++)
      node->as.invoke.args[i] = foldExpr(node->as.invoke.args[i]);
    return node;
  case NODE_IS:
    node->as.isTest.expr = foldExpr(node->as.isTest.expr);
    return node;
  case NODE_ARRAY:
    for (int i = 0; i < node->as.array.count; i++)
      node->as.array.elements[i] = foldExpr(node->as.array.elements[i]);
    return node;
  case NODE_MAP:
    for (int i = 0; i < node->as.map.count; i++) {
      node->as.map.keys[i] = foldExpr(node->as.map.keys[i]);
      node->as.map.values[i] = foldExpr(node->as.map.values[i]);
    }
    return node;
  case NODE_INDEX_GET:
    node->as.index.object = foldExpr(node->as.index.object);
    node->as.index.index = foldExpr(node->as.index.index);
    return node;
  case NODE_INDEX_SET:
    node->as.index.object = foldExpr(node->as.index.object);
    node->as.index.index = foldExpr(node->as.index.index);
    node->as.index.value = foldExpr(node->as.index.value);
    return node;
  case NODE_FIELD_GET:
    node->as.field.object = foldExpr(node->as.field.object);
    return node;
  case NODE_FIELD_SET:
    node->as.field.object = foldExpr(node->as.field.object);
    node->as.field.value = foldExpr(node->as.field.value);
    return node;
  default:
    return node; // literals, var reads: nothing to fold
  }
}

static void foldProgram(Program *program); // forward decl

// Fold inside a statement (recursing into nested blocks/functions).
static void foldStatement(Node *node) {
  switch (node->type) {
  case NODE_PRINT:
  case NODE_EXPR_STMT:
    node->as.stmt.expr = foldExpr(node->as.stmt.expr);
    break;
  case NODE_VAR_DECL:
    node->as.var.value = foldExpr(node->as.var.value);
    break;
  case NODE_BLOCK:
    foldProgram(node->as.block);
    break;
  case NODE_IF:
    node->as.ifStmt.condition = foldExpr(node->as.ifStmt.condition);
    foldStatement(node->as.ifStmt.then);
    if (node->as.ifStmt.otherwise != NULL)
      foldStatement(node->as.ifStmt.otherwise);
    break;
  case NODE_WHILE:
    node->as.whileStmt.condition = foldExpr(node->as.whileStmt.condition);
    foldStatement(node->as.whileStmt.body);
    if (node->as.whileStmt.increment != NULL)
      node->as.whileStmt.increment = foldExpr(node->as.whileStmt.increment);
    break;
  case NODE_FUN:
    foldProgram(node->as.fun.body);
    break;
  case NODE_RETURN:
    if (node->as.ret.value != NULL)
      node->as.ret.value = foldExpr(node->as.ret.value);
    break;
  case NODE_STRUCT:
    for (int i = 0; i < node->as.structDecl.methodCount; i++)
      foldProgram(node->as.structDecl.methods[i]->as.fun.body);
    break;
  case NODE_THROW:
    node->as.stmt.expr = foldExpr(node->as.stmt.expr);
    break;
  case NODE_TRY:
    foldStatement(node->as.tryStmt.body);
    foldStatement(node->as.tryStmt.handler);
    break;
  case NODE_MATCH:
    foldStatement(node->as.matchStmt.body); // fold the lowered chain
    break;
  default:
    break;
  }
}

static void foldProgram(Program *program) {
  for (int i = 0; i < program->count; i++)
    foldStatement(program->statements[i]);
}

int foldConstants(Program *program) {
  foldCount = 0;
  foldProgram(program);
  return foldCount;
}
