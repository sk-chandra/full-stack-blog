#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen_c.h"
#include "object.h"

// The native backend translates the typed first-order subset of cnano into C.
// Because types are known, every value is UNBOXED: an int is a C int64_t, a bool
// is a C bool, a str is a const char*. The system C compiler then optimises and
// targets the real machine. Anything outside the subset (closures, dynamic `any`,
// first-class functions, nil values) is rejected here with a clear message — the
// bytecode VM remains the way to run the full language.

static bool ok;     // cleared on the first unsupported construct
static FILE *out;

static void unsupported(int line, const char *what) {
  if (ok) // report only the first, like the parser's panic mode
    fprintf(stderr, "[line %d] Native compile error: %s is not supported by the "
                    "--native backend (use the VM for the full language).\n",
            line, what);
  ok = false;
}

// --- type tracking ---------------------------------------------------------
// We need each expression's type to emit correct C (printf formats, int-add vs
// string-concat, variable C types). We track variable types in a scoped table and
// function signatures separately, then infer expression types on demand.

typedef struct {
  ObjString *name;
  TypeKind type;
  int depth;
} VarSym;

typedef struct {
  ObjString *name;
  TypeKind ret;
  TypeKind params[256];
  int paramCount;
} FnSig;

static VarSym vars[1024];
static int varCount;
static int depth;
static FnSig fnSigs[256];
static int fnSigCount;

static void pushVar(ObjString *name, TypeKind type) {
  if (varCount < 1024) {
    vars[varCount].name = name;
    vars[varCount].type = type;
    vars[varCount].depth = depth;
    varCount++;
  }
}

static TypeKind lookupVar(ObjString *name) {
  for (int i = varCount - 1; i >= 0; i--)
    if (vars[i].name == name)
      return vars[i].type;
  return TY_ANY; // unknown -> treat as dynamic (will be rejected if used concretely)
}

static FnSig *lookupFn(ObjString *name) {
  for (int i = 0; i < fnSigCount; i++)
    if (fnSigs[i].name == name)
      return &fnSigs[i];
  return NULL;
}

static void beginScope(void) { depth++; }
static void endScope(void) {
  depth--;
  while (varCount > 0 && vars[varCount - 1].depth > depth)
    varCount--;
}

static TypeKind inferType(Node *node); // fwd

static TypeKind inferBinary(Node *n) {
  TypeKind l = inferType(n->as.binary.left);
  TypeKind r = inferType(n->as.binary.right);
  switch (n->as.binary.op) {
  case OP_NODE_ADD:
    return (l == TY_STR || r == TY_STR) ? TY_STR : TY_INT;
  case OP_NODE_SUB:
  case OP_NODE_MUL:
  case OP_NODE_DIV:
    return TY_INT;
  default:
    return TY_BOOL; // comparisons / equality
  }
}

static TypeKind inferType(Node *node) {
  switch (node->type) {
  case NODE_INT:
    return TY_INT;
  case NODE_BOOL:
    return TY_BOOL;
  case NODE_STRING:
    return TY_STR;
  case NODE_NIL:
    return TY_NIL;
  case NODE_VAR_GET:
    return lookupVar(node->as.name);
  case NODE_ASSIGN:
    return lookupVar(node->as.var.name);
  case NODE_UNARY:
    return node->as.unary.op == OP_NODE_NEGATE ? TY_INT : TY_BOOL;
  case NODE_BINARY:
    return inferBinary(node);
  case NODE_LOGICAL: {
    // For the bool operands we support, `and`/`or` produce a bool.
    TypeKind l = inferType(node->as.logical.left);
    return l; // both sides share a type in the subset we accept
  }
  case NODE_CALL: {
    if (node->as.call.callee->type != NODE_VAR_GET)
      return TY_ANY;
    FnSig *sig = lookupFn(node->as.call.callee->as.name);
    return sig ? sig->ret : TY_ANY;
  }
  default:
    return TY_ANY;
  }
}

// Map a TypeKind to its C type. nil maps to void (only valid as a return type).
static const char *cType(TypeKind t) {
  switch (t) {
  case TY_INT:
    return "int64_t";
  case TY_BOOL:
    return "bool";
  case TY_STR:
    return "const char *";
  case TY_NIL:
    return "void";
  default:
    return "/*any*/";
  }
}

// Reduce a structured annotation Type* to the scalar TypeKind the native backend
// understands. The PARAMETRIC types (array/map/function) need the GC'd runtime,
// so they have no place in the unboxed scalar native subset — report them
// unsupported and fall back to `any`, which the caller then rejects as "not a
// concrete native type".
static TypeKind annotationKind(Type *t, int line) {
  switch (t->kind) {
  case TY_INT:
  case TY_BOOL:
  case TY_STR:
  case TY_NIL:
  case TY_ANY:
    return t->kind;
  case TY_ARRAY:
    unsupported(line, "an array-typed value");
    return TY_ANY;
  case TY_MAP:
    unsupported(line, "a map-typed value");
    return TY_ANY;
  default:
    unsupported(line, "a function-typed value");
    return TY_ANY;
  }
}

// --- expression emission ---------------------------------------------------

static void emitExpr(Node *node);

// Emit a user identifier with a `cn_` prefix, so cnano names can never collide
// with C keywords (e.g. a cnano variable literally named `int`) or our helpers.
static void emitName(ObjString *name) {
  fprintf(out, "cn_%.*s", name->length, name->chars);
}

static void emitBinary(Node *node) {
  NodeOp op = node->as.binary.op;
  Node *l = node->as.binary.left;
  Node *r = node->as.binary.right;

  if (op == OP_NODE_ADD &&
      (inferType(l) == TY_STR || inferType(r) == TY_STR)) {
    // String concatenation goes through the runtime helper (it allocates).
    fprintf(out, "cn_concat(");
    emitExpr(l);
    fprintf(out, ", ");
    emitExpr(r);
    fprintf(out, ")");
    return;
  }

  if (op == OP_NODE_DIV) {
    // Route division through a helper that turns divide-by-zero into a runtime
    // error + exit, preserving cnano's "no host crash" guarantee.
    fprintf(out, "cn_div(");
    emitExpr(l);
    fprintf(out, ", ");
    emitExpr(r);
    fprintf(out, ", %d)", node->line);
    return;
  }

  const char *cop = "?";
  switch (op) {
  case OP_NODE_ADD: cop = "+"; break;
  case OP_NODE_SUB: cop = "-"; break;
  case OP_NODE_MUL: cop = "*"; break;
  case OP_NODE_LESS: cop = "<"; break;
  case OP_NODE_GREATER: cop = ">"; break;
  case OP_NODE_EQUAL: cop = "=="; break;
  default: break;
  }
  // Equality on strings must compare contents, not pointers.
  if (op == OP_NODE_EQUAL && inferType(l) == TY_STR && inferType(r) == TY_STR) {
    fprintf(out, "(strcmp(");
    emitExpr(l);
    fprintf(out, ", ");
    emitExpr(r);
    fprintf(out, ") == 0)");
    return;
  }
  fprintf(out, "(");
  emitExpr(l);
  fprintf(out, " %s ", cop);
  emitExpr(r);
  fprintf(out, ")");
}

static void emitExpr(Node *node) {
  switch (node->type) {
  case NODE_INT:
    fprintf(out, "INT64_C(%lld)", (long long)node->as.intValue);
    break;
  case NODE_BOOL:
    fprintf(out, "%s", node->as.boolValue ? "true" : "false");
    break;
  case NODE_STRING:
    // Emit a C string literal. We rely on the source bytes being printable; a
    // production compiler would escape them. For our tests this is sufficient.
    fprintf(out, "\"%.*s\"", node->as.stringValue->length,
            node->as.stringValue->chars);
    break;
  case NODE_NIL:
    unsupported(node->line, "a nil value in an expression");
    break;
  case NODE_VAR_GET:
    emitName(node->as.name);
    break;
  case NODE_ASSIGN:
    fprintf(out, "(");
    emitName(node->as.var.name);
    fprintf(out, " = ");
    emitExpr(node->as.var.value);
    fprintf(out, ")");
    break;
  case NODE_UNARY:
    fprintf(out, "(%s", node->as.unary.op == OP_NODE_NEGATE ? "-" : "!");
    emitExpr(node->as.unary.operand);
    fprintf(out, ")");
    break;
  case NODE_BINARY:
    emitBinary(node);
    break;
  case NODE_LOGICAL:
    fprintf(out, "(");
    emitExpr(node->as.logical.left);
    fprintf(out, " %s ", node->as.logical.isAnd ? "&&" : "||");
    emitExpr(node->as.logical.right);
    fprintf(out, ")");
    break;
  case NODE_CALL: {
    if (node->as.call.callee->type != NODE_VAR_GET) {
      unsupported(node->line, "calling a non-named (first-class) function");
      break;
    }
    emitName(node->as.call.callee->as.name);
    fprintf(out, "(");
    for (int i = 0; i < node->as.call.argCount; i++) {
      if (i > 0)
        fprintf(out, ", ");
      emitExpr(node->as.call.args[i]);
    }
    fprintf(out, ")");
    break;
  }
  case NODE_INVOKE:
    // Methods dispatch on a heap receiver through the runtime, which the scalar
    // native backend doesn't have.
    unsupported(node->line, "a method call");
    break;
  case NODE_ARRAY:
    unsupported(node->line, "an array literal");
    break;
  case NODE_MAP:
    unsupported(node->line, "a map literal");
    break;
  case NODE_INDEX_GET:
  case NODE_INDEX_SET:
    unsupported(node->line, "an index expression");
    break;
  default:
    unsupported(node->line, "this expression");
    break;
  }
}

// --- statement emission ----------------------------------------------------

static void indent(int n) {
  for (int i = 0; i < n; i++)
    fprintf(out, "  ");
}

static void emitStmt(Node *node, int ind, bool fileScope);

// Emit a `print EXPR;` — choose the printf based on the expression's type.
static void emitPrint(Node *expr, int ind) {
  TypeKind t = inferType(expr);
  indent(ind);
  switch (t) {
  case TY_INT:
    fprintf(out, "printf(\"%%lld\\n\", (long long)(");
    emitExpr(expr);
    fprintf(out, "));\n");
    break;
  case TY_BOOL:
    fprintf(out, "printf(\"%%s\\n\", (");
    emitExpr(expr);
    fprintf(out, ") ? \"true\" : \"false\");\n");
    break;
  case TY_STR:
    fprintf(out, "printf(\"%%s\\n\", ");
    emitExpr(expr);
    fprintf(out, ");\n");
    break;
  case TY_NIL:
    fprintf(out, "printf(\"nil\\n\");\n");
    break;
  default:
    unsupported(expr->line, "printing a value of unknown (dynamic) type");
    break;
  }
}

static void emitBlockBody(Program *body, int ind) {
  beginScope();
  for (int i = 0; i < body->count; i++)
    emitStmt(body->statements[i], ind, false);
  endScope();
}

static void emitStmt(Node *node, int ind, bool fileScope) {
  switch (node->type) {
  case NODE_PRINT:
    emitPrint(node->as.stmt.expr, ind);
    break;
  case NODE_EXPR_STMT:
    indent(ind);
    fprintf(out, "(void)(");
    emitExpr(node->as.stmt.expr);
    fprintf(out, ");\n");
    break;
  case NODE_VAR_DECL: {
    // Type the variable: use the annotation, else infer from the initialiser.
    TypeKind t = node->as.var.declaredType->kind != TY_ANY
                     ? annotationKind(node->as.var.declaredType, node->line)
                     : inferType(node->as.var.value);
    if (t == TY_ANY || t == TY_NIL) {
      unsupported(node->line, "a variable without a concrete type (int/bool/str)");
      pushVar(node->as.var.name, t);
      break;
    }
    if (fileScope) {
      // A top-level (global) variable was already DECLARED at file scope; here we
      // just assign its initialiser in order, inside main.
      indent(ind);
      emitName(node->as.var.name);
      fprintf(out, " = ");
      emitExpr(node->as.var.value);
      fprintf(out, ";\n");
    } else {
      indent(ind);
      fprintf(out, "%s ", cType(t));
      emitName(node->as.var.name);
      fprintf(out, " = ");
      emitExpr(node->as.var.value);
      fprintf(out, ";\n");
    }
    pushVar(node->as.var.name, t);
    break;
  }
  case NODE_BLOCK:
    indent(ind);
    fprintf(out, "{\n");
    emitBlockBody(node->as.block, ind + 1);
    indent(ind);
    fprintf(out, "}\n");
    break;
  case NODE_IF:
    indent(ind);
    fprintf(out, "if (");
    emitExpr(node->as.ifStmt.condition);
    fprintf(out, ") {\n");
    emitStmt(node->as.ifStmt.then, ind + 1, false);
    indent(ind);
    fprintf(out, "}");
    if (node->as.ifStmt.otherwise != NULL) {
      fprintf(out, " else {\n");
      emitStmt(node->as.ifStmt.otherwise, ind + 1, false);
      indent(ind);
      fprintf(out, "}");
    }
    fprintf(out, "\n");
    break;
  case NODE_WHILE:
    indent(ind);
    fprintf(out, "while (");
    emitExpr(node->as.whileStmt.condition);
    fprintf(out, ") {\n");
    emitStmt(node->as.whileStmt.body, ind + 1, false);
    indent(ind);
    fprintf(out, "}\n");
    break;
  case NODE_RETURN:
    indent(ind);
    if (node->as.ret.value != NULL) {
      fprintf(out, "return ");
      emitExpr(node->as.ret.value);
      fprintf(out, ";\n");
    } else {
      fprintf(out, "return;\n");
    }
    break;
  case NODE_FUN:
    unsupported(node->line, "a nested function declaration");
    break;
  default:
    unsupported(node->line, "this statement");
    break;
  }
}

// --- function and program emission -----------------------------------------

// Register a top-level function's signature (so calls can be typed and forward
// references resolve), and reject any feature outside the native subset.
static void registerFunction(Node *fn) {
  if (fnSigCount >= 256)
    return;
  FnSig *sig = &fnSigs[fnSigCount++];
  sig->name = fn->as.fun.name;
  sig->ret = annotationKind(fn->as.fun.returnType, fn->line);
  sig->paramCount = fn->as.fun.paramCount;
  for (int i = 0; i < fn->as.fun.paramCount; i++) {
    TypeKind pt = annotationKind(fn->as.fun.paramTypes[i], fn->line);
    if (pt == TY_ANY)
      unsupported(fn->line, "a function parameter without a type annotation");
    sig->params[i] = pt;
  }
  if (fn->as.fun.returnType->kind == TY_ANY)
    unsupported(fn->line, "a function without a return-type annotation");
}

// Emit a function's C signature: `static <ret> cn_name(<params>)`.
static void emitSignature(Node *fn) {
  fprintf(out, "static %s ", cType(annotationKind(fn->as.fun.returnType, fn->line)));
  emitName(fn->as.fun.name);
  fprintf(out, "(");
  if (fn->as.fun.paramCount == 0) {
    fprintf(out, "void");
  } else {
    for (int i = 0; i < fn->as.fun.paramCount; i++) {
      if (i > 0)
        fprintf(out, ", ");
      fprintf(out, "%s ", cType(annotationKind(fn->as.fun.paramTypes[i], fn->line)));
      emitName(fn->as.fun.params[i]);
    }
  }
  fprintf(out, ")");
}

static void emitFunction(Node *fn) {
  emitSignature(fn);
  fprintf(out, " {\n");
  beginScope();
  for (int i = 0; i < fn->as.fun.paramCount; i++)
    pushVar(fn->as.fun.params[i], annotationKind(fn->as.fun.paramTypes[i], fn->line));
  for (int i = 0; i < fn->as.fun.body->count; i++)
    emitStmt(fn->as.fun.body->statements[i], 1, false);
  endScope();
  fprintf(out, "}\n\n");
}

// The fixed C prelude: includes plus the two runtime helpers (string concat and
// checked division). Kept tiny on purpose — typed code needs almost no runtime.
static void emitPrelude(void) {
  // fputs (not fprintf): the prelude contains a literal %d inside the GENERATED
  // C, which is not a format directive for us.
  fputs("/* Generated by the cnano --native backend. */\n"
          "#include <stdio.h>\n"
          "#include <stdlib.h>\n"
          "#include <stdbool.h>\n"
          "#include <stdint.h>\n"
          "#include <string.h>\n\n"
          "static const char *cn_concat(const char *a, const char *b) {\n"
          "  size_t la = strlen(a), lb = strlen(b);\n"
          "  char *r = malloc(la + lb + 1);\n"
          "  if (!r) { fprintf(stderr, \"out of memory\\n\"); exit(70); }\n"
          "  memcpy(r, a, la); memcpy(r + la, b, lb); r[la + lb] = 0;\n"
          "  return r; /* short-lived program: intentionally not freed */\n"
          "}\n\n"
          "static int64_t cn_div(int64_t a, int64_t b, int line) {\n"
          "  if (b == 0) {\n"
          "    fprintf(stderr, \"Runtime error: division by zero\\n[line %d]\\n\", line);\n"
          "    exit(70);\n"
          "  }\n"
          "  return a / b;\n"
          "}\n\n",
        out);
}

bool emitC(Program *program, FILE *outFile) {
  out = outFile;
  ok = true;
  varCount = 0;
  depth = 0;
  fnSigCount = 0;

  emitPrelude();

  // Pass 1: register + forward-declare every top-level function, so calls (incl.
  // mutual recursion and forward references) resolve. A forward declaration in C
  // is exactly the tool for this.
  for (int i = 0; i < program->count; i++)
    if (program->statements[i]->type == NODE_FUN)
      registerFunction(program->statements[i]);
  for (int i = 0; i < program->count; i++) {
    if (program->statements[i]->type == NODE_FUN) {
      emitSignature(program->statements[i]);
      fprintf(out, ";\n");
    }
  }
  fprintf(out, "\n");

  // Pass 2: declare top-level (global) variables at file scope so functions can
  // see them. We only declare here; initialisers run in order inside main.
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type == NODE_VAR_DECL) {
      TypeKind t = s->as.var.declaredType->kind != TY_ANY
                       ? annotationKind(s->as.var.declaredType, s->line)
                       : inferType(s->as.var.value);
      if (t == TY_ANY || t == TY_NIL) {
        unsupported(s->line, "a global without a concrete type (int/bool/str)");
      } else {
        fprintf(out, "static %s ", cType(t));
        emitName(s->as.var.name);
        fprintf(out, ";\n");
      }
      pushVar(s->as.var.name, t); // visible to functions emitted next
    }
  }
  fprintf(out, "\n");

  // Pass 3: emit function bodies.
  for (int i = 0; i < program->count; i++)
    if (program->statements[i]->type == NODE_FUN)
      emitFunction(program->statements[i]);

  // Pass 4: main() runs the top-level statements in order. Globals are already
  // declared; their `let`s become assignments. Functions are skipped (emitted).
  fprintf(out, "int main(void) {\n");
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type == NODE_FUN)
      continue;
    emitStmt(s, 1, /*fileScope=*/true);
  }
  fprintf(out, "  return 0;\n}\n");

  return ok;
}
