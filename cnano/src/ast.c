#include <stdio.h>
#include <stdlib.h>

#include "ast.h"

// A tiny allocation helper so every constructor reports OOM the same way.
static Node *allocNode(NodeType type, int line) {
  Node *node = malloc(sizeof(Node));
  if (node == NULL) {
    fprintf(stderr, "cnano: out of memory allocating AST node\n");
    exit(70);
  }
  node->type = type;
  node->line = line;
  return node;
}

Node *newInt(int64_t value, int line) {
  Node *node = allocNode(NODE_INT, line);
  node->as.intValue = value;
  return node;
}

Node *newBool(bool value, int line) {
  Node *node = allocNode(NODE_BOOL, line);
  node->as.boolValue = value;
  return node;
}

Node *newNil(int line) { return allocNode(NODE_NIL, line); }

Node *newString(ObjString *value, int line) {
  Node *node = allocNode(NODE_STRING, line);
  node->as.stringValue = value;
  return node;
}

Node *newVarGet(ObjString *name, int line) {
  Node *node = allocNode(NODE_VAR_GET, line);
  node->as.name = name;
  return node;
}

Node *newAssign(ObjString *name, Node *value, int line) {
  Node *node = allocNode(NODE_ASSIGN, line);
  node->as.var.name = name;
  node->as.var.value = value;
  return node;
}

Node *newUnary(NodeOp op, Node *operand, int line) {
  Node *node = allocNode(NODE_UNARY, line);
  node->as.unary.op = op;
  node->as.unary.operand = operand;
  return node;
}

Node *newBinary(NodeOp op, Node *left, Node *right, int line) {
  Node *node = allocNode(NODE_BINARY, line);
  node->as.binary.op = op;
  node->as.binary.left = left;
  node->as.binary.right = right;
  return node;
}

Node *newPrint(Node *expr, int line) {
  Node *node = allocNode(NODE_PRINT, line);
  node->as.stmt.expr = expr;
  return node;
}

Node *newExprStmt(Node *expr, int line) {
  Node *node = allocNode(NODE_EXPR_STMT, line);
  node->as.stmt.expr = expr;
  return node;
}

Node *newVarDecl(ObjString *name, Node *value, Type *declaredType, int line) {
  Node *node = allocNode(NODE_VAR_DECL, line);
  node->as.var.name = name;
  node->as.var.value = value;
  node->as.var.declaredType = declaredType;
  return node;
}

Node *newBlock(Program *block, int line) {
  Node *node = allocNode(NODE_BLOCK, line);
  node->as.block = block; // node now owns this heap-allocated Program
  return node;
}

Node *newLogical(bool isAnd, Node *left, Node *right, int line) {
  Node *node = allocNode(NODE_LOGICAL, line);
  node->as.logical.isAnd = isAnd;
  node->as.logical.left = left;
  node->as.logical.right = right;
  return node;
}

Node *newIf(Node *condition, Node *then, Node *otherwise, int line) {
  Node *node = allocNode(NODE_IF, line);
  node->as.ifStmt.condition = condition;
  node->as.ifStmt.then = then;
  node->as.ifStmt.otherwise = otherwise;
  return node;
}

Node *newWhile(Node *condition, Node *body, int line) {
  Node *node = allocNode(NODE_WHILE, line);
  node->as.whileStmt.condition = condition;
  node->as.whileStmt.body = body;
  return node;
}

Node *newCall(Node *callee, Node **args, int argCount, int line) {
  Node *node = allocNode(NODE_CALL, line);
  node->as.call.callee = callee;
  node->as.call.args = args;
  node->as.call.argCount = argCount;
  return node;
}

Node *newInvoke(Node *receiver, ObjString *method, Node **args, int argCount,
                int line) {
  Node *node = allocNode(NODE_INVOKE, line);
  node->as.invoke.receiver = receiver;
  node->as.invoke.method = method;
  node->as.invoke.args = args;
  node->as.invoke.argCount = argCount;
  return node;
}

Node *newArray(Node **elements, int count, int line) {
  Node *node = allocNode(NODE_ARRAY, line);
  node->as.array.elements = elements;
  node->as.array.count = count;
  return node;
}

Node *newMap(Node **keys, Node **values, int count, int line) {
  Node *node = allocNode(NODE_MAP, line);
  node->as.map.keys = keys;
  node->as.map.values = values;
  node->as.map.count = count;
  return node;
}

Node *newIndexGet(Node *object, Node *index, int line) {
  Node *node = allocNode(NODE_INDEX_GET, line);
  node->as.index.object = object;
  node->as.index.index = index;
  node->as.index.value = NULL;
  return node;
}

Node *newIndexSet(Node *object, Node *index, Node *value, int line) {
  Node *node = allocNode(NODE_INDEX_SET, line);
  node->as.index.object = object;
  node->as.index.index = index;
  node->as.index.value = value;
  return node;
}

Node *newFieldGet(Node *object, ObjString *field, int line) {
  Node *node = allocNode(NODE_FIELD_GET, line);
  node->as.field.object = object;
  node->as.field.field = field;
  node->as.field.value = NULL;
  return node;
}

Node *newFieldSet(Node *object, ObjString *field, Node *value, int line) {
  Node *node = allocNode(NODE_FIELD_SET, line);
  node->as.field.object = object;
  node->as.field.field = field;
  node->as.field.value = value;
  return node;
}

Node *newStructDecl(ObjString *name, ObjString **fieldNames, Type **fieldTypes,
                    int fieldCount, int line) {
  Node *node = allocNode(NODE_STRUCT, line);
  node->as.structDecl.name = name;
  node->as.structDecl.fieldNames = fieldNames;
  node->as.structDecl.fieldTypes = fieldTypes;
  node->as.structDecl.fieldCount = fieldCount;
  node->as.structDecl.methods = NULL; // filled in by the parser
  node->as.structDecl.methodCount = 0;
  return node;
}

Node *cloneExpr(Node *node) {
  switch (node->type) {
  case NODE_INT:
    return newInt(node->as.intValue, node->line);
  case NODE_BOOL:
    return newBool(node->as.boolValue, node->line);
  case NODE_NIL:
    return newNil(node->line);
  case NODE_STRING:
    return newString(node->as.stringValue, node->line); // interned; shareable
  case NODE_VAR_GET:
    return newVarGet(node->as.name, node->line); // name is interned; shareable
  case NODE_INDEX_GET: {
    Node *object = cloneExpr(node->as.index.object);
    Node *index = cloneExpr(node->as.index.index);
    if (object == NULL || index == NULL) {
      freeNode(object); // tolerates NULL
      freeNode(index);
      return NULL;
    }
    return newIndexGet(object, index, node->line);
  }
  case NODE_FIELD_GET: {
    Node *object = cloneExpr(node->as.field.object);
    if (object == NULL)
      return NULL;
    return newFieldGet(object, node->as.field.field, node->line);
  }
  default:
    return NULL; // not a pure, safely-duplicable target sub-expression
  }
}

Node *newFun(ObjString *name, ObjString **params, Type **paramTypes,
             int paramCount, Type *returnType, Program *body, int line) {
  Node *node = allocNode(NODE_FUN, line);
  node->as.fun.name = name;
  node->as.fun.params = params;
  node->as.fun.paramTypes = paramTypes;
  node->as.fun.paramCount = paramCount;
  node->as.fun.returnType = returnType;
  node->as.fun.body = body;
  return node;
}

Node *newReturn(Node *value, int line) {
  Node *node = allocNode(NODE_RETURN, line);
  node->as.ret.value = value;
  return node;
}

// Post-order traversal: free children before the parent so we never follow a
// dangling pointer. Recursion mirrors the tree's own shape — the natural way to
// walk a tree in any compiler stage.
void freeNode(Node *node) {
  if (node == NULL)
    return;
  switch (node->type) {
  case NODE_INT:
  case NODE_BOOL:
  case NODE_NIL:
  case NODE_STRING:  // the ObjString is owned/freed by the VM, not the AST
  case NODE_VAR_GET: // ditto for the variable name
    break;           // leaf nodes, no child Nodes
  case NODE_UNARY:
    freeNode(node->as.unary.operand);
    break;
  case NODE_BINARY:
    freeNode(node->as.binary.left);
    freeNode(node->as.binary.right);
    break;
  case NODE_PRINT:
  case NODE_EXPR_STMT:
    freeNode(node->as.stmt.expr);
    break;
  case NODE_ASSIGN:
  case NODE_VAR_DECL:
    freeNode(node->as.var.value); // free the value expr; name is VM-owned
    break;
  case NODE_BLOCK:
    // A block owns a heap-allocated Program: free its statements, the Program
    // struct itself, then fall through to free the node. (May be NULL if the
    // Program was detached, e.g. when a function declaration adopts the body.)
    if (node->as.block != NULL) {
      freeProgram(node->as.block);
      free(node->as.block);
    }
    break;
  case NODE_LOGICAL:
    freeNode(node->as.logical.left);
    freeNode(node->as.logical.right);
    break;
  case NODE_IF:
    freeNode(node->as.ifStmt.condition);
    freeNode(node->as.ifStmt.then);
    freeNode(node->as.ifStmt.otherwise); // freeNode tolerates NULL (no else)
    break;
  case NODE_WHILE:
    freeNode(node->as.whileStmt.condition);
    freeNode(node->as.whileStmt.body);
    break;
  case NODE_CALL:
    freeNode(node->as.call.callee);
    for (int i = 0; i < node->as.call.argCount; i++)
      freeNode(node->as.call.args[i]);
    free(node->as.call.args); // free the heap array of arg pointers
    break;
  case NODE_INVOKE:
    // method name is VM-owned (interned); free the receiver, args, and the array.
    freeNode(node->as.invoke.receiver);
    for (int i = 0; i < node->as.invoke.argCount; i++)
      freeNode(node->as.invoke.args[i]);
    free(node->as.invoke.args);
    break;
  case NODE_ARRAY:
    for (int i = 0; i < node->as.array.count; i++)
      freeNode(node->as.array.elements[i]);
    free(node->as.array.elements);
    break;
  case NODE_MAP:
    for (int i = 0; i < node->as.map.count; i++) {
      freeNode(node->as.map.keys[i]);
      freeNode(node->as.map.values[i]);
    }
    free(node->as.map.keys);
    free(node->as.map.values);
    break;
  case NODE_INDEX_GET:
  case NODE_INDEX_SET:
    freeNode(node->as.index.object);
    freeNode(node->as.index.index);
    freeNode(node->as.index.value); // tolerates NULL (the get form)
    break;
  case NODE_FIELD_GET:
  case NODE_FIELD_SET:
    // field name is VM-owned (interned); free the object and (set form) value.
    freeNode(node->as.field.object);
    freeNode(node->as.field.value); // tolerates NULL (the get form)
    break;
  case NODE_STRUCT:
    // name and field names are interned (VM-owned); free only the arrays and the
    // method declaration nodes.
    free(node->as.structDecl.fieldNames);
    free(node->as.structDecl.fieldTypes);
    for (int i = 0; i < node->as.structDecl.methodCount; i++)
      freeNode(node->as.structDecl.methods[i]);
    free(node->as.structDecl.methods);
    break;
  case NODE_FUN:
    // name and the param ObjStrings are VM-owned (interned); free only the
    // params/paramTypes arrays, the body program, and its container.
    free(node->as.fun.params);
    free(node->as.fun.paramTypes);
    freeProgram(node->as.fun.body);
    free(node->as.fun.body);
    break;
  case NODE_RETURN:
    freeNode(node->as.ret.value); // tolerates NULL (bare `return;`)
    break;
  }
  free(node);
}

// --- Program: a growable list of statements --------------------------------

void initProgram(Program *program) {
  program->count = 0;
  program->capacity = 0;
  program->statements = NULL;
}

void writeProgram(Program *program, Node *statement) {
  if (program->capacity < program->count + 1) {
    int oldCapacity = program->capacity;
    program->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
    program->statements =
        realloc(program->statements, sizeof(Node *) * program->capacity);
    if (program->statements == NULL) {
      fprintf(stderr, "cnano: out of memory growing program\n");
      exit(70);
    }
  }
  program->statements[program->count++] = statement;
}

void freeProgram(Program *program) {
  // Own every statement: free each tree, then the array holding the pointers.
  for (int i = 0; i < program->count; i++)
    freeNode(program->statements[i]);
  free(program->statements);
  initProgram(program);
}
