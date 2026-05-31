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
    break; // leaf nodes, no children
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
