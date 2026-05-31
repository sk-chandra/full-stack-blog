// ast.h — the Abstract Syntax Tree (AST).
//
// The parser does not emit bytecode directly. Instead it builds a tree that
// mirrors the *structure* of the expression. For `1 + 2 * 3` the tree is:
//
//         (+)
//        /   .
//       1    (*)
//           /   .
//          2     3
//
// (dots stand for the right branches: a backslash ending a // line would be
//  read as a line-continuation by the C preprocessor and warn under -Wall.)
//
// The tree captures precedence as SHAPE: multiplication sits lower, so it is
// evaluated first. Separating "understand the structure" (parser → AST) from
// "produce instructions" (compiler walks AST → bytecode) is the single biggest
// readability win in a compiler, and it is how almost every real compiler is
// organised. A one-pass compiler can skip the tree, but you lose the clean
// separation and the ability to analyse/optimise the program as data.
#ifndef CNANO_AST_H
#define CNANO_AST_H

#include "common.h"

typedef enum {
  NODE_INT,     // a literal integer
  NODE_BOOL,    // a literal `true` or `false`
  NODE_NIL,     // the literal `nil`
  NODE_UNARY,   // a prefix operator applied to one child (e.g. -x, !x)
  NODE_BINARY,  // an operator with a left and right child (e.g. a + b, a < b)
} NodeType;

// The operator carried by unary/binary nodes. Keeping this separate from the
// lexer's TokenType means the compiler switches over a small, meaningful set
// rather than over raw token kinds.
//
// Note what is and isn't here: we have NEGATE, NOT, ADD..DIV, plus the three
// PRIMITIVE comparisons EQUAL, LESS, GREATER. There is deliberately no
// NOT_EQUAL/LESS_EQUAL/GREATER_EQUAL operator — the parser *desugars* those into
// combinations of the primitives (see parser.c). Fewer operators here means
// fewer cases in the compiler and fewer opcodes in the VM.
typedef enum {
  OP_NODE_NEGATE,  // -x  (arithmetic)
  OP_NODE_NOT,     // !x  (logical)
  OP_NODE_ADD,
  OP_NODE_SUB,
  OP_NODE_MUL,
  OP_NODE_DIV,
  OP_NODE_EQUAL,   // ==
  OP_NODE_LESS,    // <
  OP_NODE_GREATER, // >
} NodeOp;

// A "tagged union": the `type` field tells you which arm of the union `as` is
// valid. This is the idiomatic C way to represent "one of several shapes" and
// is exactly how dynamically typed values and AST nodes are stored in real VMs.
typedef struct Node {
  NodeType type;
  int line; // source line, threaded through for runtime/compile error messages
  union {
    // NODE_INT carries an integer; NODE_BOOL carries a bool; NODE_NIL carries
    // nothing. We store the raw C payloads here (not a full tagged Value) and
    // let the compiler wrap them into Values — keeping the AST independent of the
    // runtime value representation.
    int64_t intValue;
    bool boolValue;
    struct {
      NodeOp op;
      struct Node *operand;
    } unary;
    struct {
      NodeOp op;
      struct Node *left;
      struct Node *right;
    } binary;
  } as;
} Node;

// Constructors. Each allocates a node on the heap and fills it in. The parser
// owns these; freeNode walks the tree and releases the whole thing.
Node *newInt(int64_t value, int line);
Node *newBool(bool value, int line);
Node *newNil(int line);
Node *newUnary(NodeOp op, Node *operand, int line);
Node *newBinary(NodeOp op, Node *left, Node *right, int line);
void freeNode(Node *node);

#endif // CNANO_AST_H
