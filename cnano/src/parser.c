#include <stdio.h>
#include <stdlib.h>
#include <string.h> // memcmp — for recognising type-name tokens

#include "ast.h"
#include "lexer.h"
#include "object.h" // copyString — interns identifier and string-literal text
#include "parser.h"

// The grammar we implement, written in EBNF. Each rule below becomes a function.
//
// A program is now a SEQUENCE OF STATEMENTS, not a single expression. The top of
// the grammar is therefore about statements; expressions sit underneath:
//
//   program     -> declaration* EOF ;
//   declaration -> funDecl | varDecl | statement ;
//   funDecl     -> "fn" IDENTIFIER "(" parameters? ")" block ;
//   parameters  -> IDENTIFIER ( "," IDENTIFIER )* ;
//   varDecl     -> "let" IDENTIFIER "=" expression ";" ;
//   statement   -> printStmt | ifStmt | whileStmt | forStmt | returnStmt
//                | block | exprStmt ;
//   returnStmt  -> "return" expression? ";" ;
//   ifStmt      -> "if" "(" expression ")" statement ( "else" statement )? ;
//   whileStmt   -> "while" "(" expression ")" statement ;
//   forStmt     -> "for" "(" ( varDecl | exprStmt | ";" )
//                            expression? ";" expression? ")" statement ;
//   block       -> "{" declaration* "}" ;
//   printStmt   -> "print" expression ";" ;
//   exprStmt    -> expression ";" ;
//
// Lower expression rules bind tighter (higher precedence):
//
//   expression -> assignment ;
//   assignment -> IDENTIFIER "=" assignment | logic_or ;   // right-associative
//   logic_or   -> logic_and ( "or" logic_and )* ;          // short-circuits
//   logic_and  -> equality  ( "and" equality )* ;          // short-circuits
//
// ASSIGNMENT and the l-value problem: `a = 1` reads left-to-right, but the left
// side is a *target* (where to store), not a value to compute. The clean
// recursive-descent trick: parse the left side as an ordinary expression, then
// if a "=" follows, check that what we parsed is actually assignable (a bare
// variable) and rebuild it as an assignment. Assignment is right-associative
// (a = b = 1 means a = (b = 1)) and lowest precedence, so it sits at the very
// top of the expression grammar.
//   equality   -> comparison ( ( "==" | "!=" ) comparison )* ;
//   comparison -> term ( ( "<" | "<=" | ">" | ">=" ) term )* ;
//   term       -> factor ( ( "+" | "-" ) factor )* ;       // left-associative
//   factor     -> unary  ( ( "*" | "/" ) unary  )* ;       // left-associative
//   unary      -> ( "-" | "!" ) unary | call ;
//   call       -> primary ( "(" arguments? ")" )* ;   // postfix call(s)
//   arguments  -> expression ( "," expression )* ;
//   primary    -> INT | STRING | IDENTIFIER | "true" | "false" | "nil"
//               | "(" expression ")" ;
//
// Precedence falls out of the call chain: equality calls comparison calls term
// calls factor calls unary calls primary. Each new level we add sits ABOVE the
// arithmetic it should bind looser than — so `1 + 2 == 3` parses as
// `(1 + 2) == 3`, which is what people expect. Associativity falls out of the
// loops (`while`): we fold left-to-right, so 1-2-3 parses as (1-2)-3.
//
// DESUGARING: the grammar lists `!=`, `<=`, `>=`, but the AST has no operators
// for them. The parser rewrites them into the three primitives the rest of the
// pipeline supports:
//     a != b   becomes   !(a == b)
//     a <= b   becomes   !(a > b)
//     a >= b   becomes   !(a < b)
// This is "syntactic sugar": surface syntax that expands into a simpler core.
// Doing it once, here, means the compiler and VM never need to know these forms
// exist — a recurring strategy for keeping a language's CORE small while its
// SURFACE stays convenient.

typedef struct {
  Token current;  // the next token to consume (one-token lookahead)
  Token previous; // the most recently consumed token
  bool hadError;  // did ANY error occur during the whole parse?
  bool panicMode; // are we currently recovering from an error?
} Parser;

static Parser parser;

// --- error reporting -------------------------------------------------------

static void errorAt(Token *token, const char *message) {
  // panicMode suppresses the cascade of bogus errors that follows a real one,
  // until we resynchronise at a statement boundary (see synchronize()). Without
  // this, one mistake produces a wall of confusing messages. hadError stays set
  // for the whole parse so the caller knows compilation must be aborted.
  if (parser.panicMode)
    return;
  parser.panicMode = true;
  parser.hadError = true;
  fprintf(stderr, "[line %d] Error", token->line);
  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // The lexer already put a message in the token's text.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }
  fprintf(stderr, ": %s\n", message);
}

// --- token cursor ----------------------------------------------------------

// Pull the next real token, surfacing any lexer error tokens as parse errors.
static void advance(void) {
  parser.previous = parser.current;
  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR)
      break;
    errorAt(&parser.current, parser.current.start);
  }
}

// Consume the current token if it matches; otherwise report `message`.
static void consume(TokenType type, const char *message) {
  if (parser.current.type == type) {
    advance();
    return;
  }
  errorAt(&parser.current, message);
}

static bool check(TokenType type) { return parser.current.type == type; }

// If the current token is `type`, consume it and return true.
static bool match(TokenType type) {
  if (!check(type))
    return false;
  advance();
  return true;
}

// --- grammar rules (forward declarations, since they call each other) -------

static Node *expression(void);
static Node *assignment(void);
static Node *logicOr(void);
static Node *logicAnd(void);
static Node *equality(void);
static Node *comparison(void);
static Node *term(void);
static Node *factor(void);
static Node *unary(void);
static Node *primary(void);

static Node *expression(void) { return assignment(); }

// Map a just-matched compound-assignment token to its arithmetic NodeOp.
static bool matchCompoundAssign(NodeOp *op) {
  if (match(TOKEN_PLUS_EQUAL)) { *op = OP_NODE_ADD; return true; }
  if (match(TOKEN_MINUS_EQUAL)) { *op = OP_NODE_SUB; return true; }
  if (match(TOKEN_STAR_EQUAL)) { *op = OP_NODE_MUL; return true; }
  if (match(TOKEN_SLASH_EQUAL)) { *op = OP_NODE_DIV; return true; }
  if (match(TOKEN_PERCENT_EQUAL)) { *op = OP_NODE_MOD; return true; }
  return false;
}

static Node *assignment(void) {
  // Parse the left-hand side as a normal expression first. It goes through the
  // logical operators, so `a or b` and `a and b` are valid l-value *bases* even
  // though they are never valid assignment targets.
  Node *node = logicOr();

  // Compound assignment `target OP= rhs` desugars to `target = target OP rhs`.
  // We build it here so it works for both variable and index targets, reusing
  // the plain-assignment nodes — there is no dedicated opcode.
  NodeOp cop;
  if (matchCompoundAssign(&cop)) {
    int line = parser.previous.line;
    Node *rhs = assignment(); // right-associative, like '='

    if (node->type == NODE_VAR_GET) {
      // x OP= rhs  ->  x = (x OP rhs). A variable read has no side effect, so a
      // fresh read for the binary is safe; salvage the name and free the target.
      ObjString *name = node->as.name;
      Node *read = newVarGet(name, line);
      Node *combined = newBinary(cop, read, rhs, line);
      freeNode(node);
      return newAssign(name, combined, line);
    }

    if (node->type == NODE_INDEX_GET) {
      // a[i] OP= rhs  ->  a[i] = (a[i] OP rhs). Clone the (pure) object/index for
      // the STORE; reuse the original index-get node as the READ in the binary,
      // so neither side double-frees and side-effect-free targets evaluate alike.
      Node *objClone = cloneExpr(node->as.index.object);
      Node *idxClone = cloneExpr(node->as.index.index);
      if (objClone == NULL || idxClone == NULL) {
        errorAt(&parser.previous,
                "compound-assignment target is too complex; write it out as "
                "`a[i] = a[i] + x`.");
        freeNode(objClone);
        freeNode(idxClone);
        freeNode(rhs);
        return node;
      }
      Node *combined = newBinary(cop, node, rhs, line); // `node` is the read
      return newIndexSet(objClone, idxClone, combined, line);
    }

    errorAt(&parser.previous, "Invalid assignment target.");
    freeNode(rhs);
    return node;
  }

  // If a '=' follows, this was actually an assignment target.
  if (match(TOKEN_EQUAL)) {
    int line = parser.previous.line;
    Node *value = assignment(); // recurse right -> right-associative

    if (node->type == NODE_VAR_GET) {
      // Valid target: turn the "read x" we built into "assign to x". We salvage
      // the interned name, then free the throwaway VAR_GET node.
      ObjString *name = node->as.name;
      freeNode(node);
      return newAssign(name, value, line);
    }

    if (node->type == NODE_INDEX_GET) {
      // `obj[i] = value`: salvage the object and index out of the get node (NULL
      // them so freeing the wrapper leaves them intact) and rebuild as a set.
      Node *object = node->as.index.object;
      Node *index = node->as.index.index;
      node->as.index.object = NULL;
      node->as.index.index = NULL;
      freeNode(node);
      return newIndexSet(object, index, value, line);
    }

    // Invalid l-value, e.g. `1 + 2 = 3` or `(a) = 3`. Report but don't abort the
    // whole parse. We still free what we built to avoid a leak.
    errorAt(&parser.current, "Invalid assignment target.");
    freeNode(value);
    return node;
  }

  return node;
}

// `a or b or c` — left-associative chain. The short-circuit semantics live in
// the COMPILER (emit a jump that skips the right side when the left already
// decides the result); here we just build the tree.
static Node *logicOr(void) {
  Node *node = logicAnd();
  while (check(TOKEN_OR)) {
    int line = parser.current.line;
    advance();
    Node *right = logicAnd();
    node = newLogical(/*isAnd=*/false, node, right, line);
  }
  return node;
}

static Node *logicAnd(void) {
  Node *node = equality();
  while (check(TOKEN_AND)) {
    int line = parser.current.line;
    advance();
    Node *right = equality();
    node = newLogical(/*isAnd=*/true, node, right, line);
  }
  return node;
}

static Node *equality(void) {
  Node *node = comparison();
  while (check(TOKEN_EQUAL_EQUAL) || check(TOKEN_BANG_EQUAL)) {
    int line = parser.current.line;
    bool negate = check(TOKEN_BANG_EQUAL); // remember before consuming
    advance();
    Node *right = comparison();
    node = newBinary(OP_NODE_EQUAL, node, right, line);
    // Desugar `a != b` into `!(a == b)`.
    if (negate)
      node = newUnary(OP_NODE_NOT, node, line);
  }
  return node;
}

static Node *comparison(void) {
  Node *node = term();
  while (check(TOKEN_LESS) || check(TOKEN_LESS_EQUAL) ||
         check(TOKEN_GREATER) || check(TOKEN_GREATER_EQUAL)) {
    int line = parser.current.line;
    TokenType op = parser.current.type;
    advance();
    Node *right = term();
    // Build each form out of the two primitives `<` and `>` plus `!`:
    switch (op) {
    case TOKEN_LESS:
      node = newBinary(OP_NODE_LESS, node, right, line);
      break;
    case TOKEN_GREATER:
      node = newBinary(OP_NODE_GREATER, node, right, line);
      break;
    case TOKEN_LESS_EQUAL: // a <= b  ==>  !(a > b)
      node = newUnary(OP_NODE_NOT,
                      newBinary(OP_NODE_GREATER, node, right, line), line);
      break;
    case TOKEN_GREATER_EQUAL: // a >= b  ==>  !(a < b)
      node = newUnary(OP_NODE_NOT,
                      newBinary(OP_NODE_LESS, node, right, line), line);
      break;
    default:
      break; // unreachable
    }
  }
  return node;
}

static Node *term(void) {
  Node *node = factor();
  while (check(TOKEN_PLUS) || check(TOKEN_MINUS)) {
    int line = parser.current.line;
    NodeOp op = match(TOKEN_PLUS) ? OP_NODE_ADD : (advance(), OP_NODE_SUB);
    Node *right = factor();
    node = newBinary(op, node, right, line);
  }
  return node;
}

static Node *factor(void) {
  Node *node = unary();
  while (check(TOKEN_STAR) || check(TOKEN_SLASH) || check(TOKEN_PERCENT)) {
    int line = parser.current.line;
    NodeOp op;
    if (match(TOKEN_STAR))
      op = OP_NODE_MUL;
    else if (match(TOKEN_SLASH))
      op = OP_NODE_DIV;
    else {
      advance(); // consume '%'
      op = OP_NODE_MOD;
    }
    Node *right = unary();
    node = newBinary(op, node, right, line);
  }
  return node;
}

static Node *call(void);

static Node *unary(void) {
  // Both prefix operators recurse into unary() (not primary) so stacked prefixes
  // like `--5` or `!!true` parse right-to-left as -(-5) / !(!true).
  if (match(TOKEN_MINUS)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_NEGATE, unary(), line);
  }
  if (match(TOKEN_BANG)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_NOT, unary(), line);
  }
  return call();
}

// Parse an argument list after a just-consumed '(' up to and including ')'.
// Arguments are expressions separated by commas; the count is capped at 255
// because the call/invoke operands are one byte. Returns the heap array (NULL if
// empty) and writes the count through `argCountOut`. Shared by calls and method
// invocations so the two stay identical.
static Node **parseArgList(int *argCountOut) {
  Node **args = NULL;
  int argCount = 0;
  int capacity = 0;
  if (!check(TOKEN_RPAREN)) {
    do {
      if (argCount == 255) {
        errorAt(&parser.current, "Cannot have more than 255 arguments.");
        break;
      }
      if (argCount + 1 > capacity) {
        capacity = capacity < 4 ? 4 : capacity * 2;
        args = realloc(args, sizeof(Node *) * capacity);
        if (args == NULL) {
          fprintf(stderr, "cnano: out of memory parsing arguments\n");
          exit(70);
        }
      }
      args[argCount++] = expression();
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RPAREN, "Expect ')' after arguments.");
  *argCountOut = argCount;
  return args;
}

// `callee(args)` — a plain call. The '(' has just been consumed.
static Node *finishCall(Node *callee) {
  int line = parser.previous.line; // the '('
  int argCount = 0;
  Node **args = parseArgList(&argCount);
  return newCall(callee, args, argCount, line);
}

// `receiver.method(args)` — a method invocation. The '.' has just been consumed.
static Node *finishInvoke(Node *receiver) {
  int line = parser.previous.line; // the '.'
  consume(TOKEN_IDENTIFIER, "Expect a method name after '.'.");
  ObjString *method = copyString(parser.previous.start, parser.previous.length);
  consume(TOKEN_LPAREN, "Expect '(' after method name.");
  int argCount = 0;
  Node **args = parseArgList(&argCount);
  return newInvoke(receiver, method, args, argCount, line);
}

// `call -> primary ( "(" arguments? ")" | "." NAME "(" arguments? ")" )*` — a
// primary followed by zero or more call / method-invoke suffixes. Looping lets
// `f()()` and `a.b().c()` chain, which is why these are parsed as postfix here
// rather than baked into primary.
static Node *call(void) {
  Node *node = primary();
  for (;;) {
    if (match(TOKEN_LPAREN))
      node = finishCall(node);
    else if (match(TOKEN_DOT))
      node = finishInvoke(node);
    else if (match(TOKEN_LBRACKET)) {
      int line = parser.previous.line; // the '['
      Node *index = expression();
      consume(TOKEN_RBRACKET, "Expect ']' after index.");
      node = newIndexGet(node, index, line); // assignment() may rewrite to a set
    } else
      break;
  }
  return node;
}

static Node *primary(void) {
  if (match(TOKEN_NUMBER)) {
    // strtoll parses the slice of source text the token points at. The token is
    // not NUL-terminated on its own, but it is followed by more source (or the
    // final '\0'), and strtoll stops at the first non-digit, so this is safe.
    int64_t value = strtoll(parser.previous.start, NULL, 10);
    return newInt(value, parser.previous.line);
  }
  if (match(TOKEN_TRUE))
    return newBool(true, parser.previous.line);
  if (match(TOKEN_FALSE))
    return newBool(false, parser.previous.line);
  if (match(TOKEN_NIL))
    return newNil(parser.previous.line);
  if (match(TOKEN_STRING)) {
    // Strip the surrounding quotes: start+1, length-2. copyString interns it.
    ObjString *s = copyString(parser.previous.start + 1,
                              parser.previous.length - 2);
    return newString(s, parser.previous.line);
  }
  if (match(TOKEN_IDENTIFIER)) {
    // Intern the variable's name so it can be used as a hash-table key. We build
    // a "read" node; assignment() rewrites it into an assign if a '=' follows.
    ObjString *name = copyString(parser.previous.start, parser.previous.length);
    return newVarGet(name, parser.previous.line);
  }
  if (match(TOKEN_LPAREN)) {
    Node *node = expression();
    consume(TOKEN_RPAREN, "Expect ')' after expression.");
    return node;
  }
  if (match(TOKEN_LBRACKET)) {
    // Array literal: `[e0, e1, ...]`. Same growable-list parse as an argument
    // list, but delimited by ']'.
    int line = parser.previous.line;
    Node **elements = NULL;
    int count = 0, capacity = 0;
    if (!check(TOKEN_RBRACKET)) {
      do {
        if (count == 255) {
          errorAt(&parser.current, "Cannot have more than 255 array elements.");
          break;
        }
        if (count + 1 > capacity) {
          capacity = capacity < 4 ? 4 : capacity * 2;
          elements = realloc(elements, sizeof(Node *) * capacity);
          if (elements == NULL) {
            fprintf(stderr, "cnano: out of memory parsing array literal\n");
            exit(70);
          }
        }
        elements[count++] = expression();
      } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RBRACKET, "Expect ']' after array elements.");
    return newArray(elements, count, line);
  }
  if (match(TOKEN_LBRACE)) {
    // Map literal: `{k0: v0, k1: v1, ...}`. Reached only in EXPRESSION position;
    // a `{` that begins a statement is parsed as a block (see statement()), so
    // there is no ambiguity — but a map-literal statement must be parenthesised
    // or used as a value (e.g. `let m = {..};`).
    int line = parser.previous.line;
    Node **keys = NULL;
    Node **values = NULL;
    int count = 0, capacity = 0;
    if (!check(TOKEN_RBRACE)) {
      do {
        if (count == 255) {
          errorAt(&parser.current, "Cannot have more than 255 map entries.");
          break;
        }
        if (count + 1 > capacity) {
          capacity = capacity < 4 ? 4 : capacity * 2;
          keys = realloc(keys, sizeof(Node *) * capacity);
          values = realloc(values, sizeof(Node *) * capacity);
          if (keys == NULL || values == NULL) {
            fprintf(stderr, "cnano: out of memory parsing map literal\n");
            exit(70);
          }
        }
        keys[count] = expression();
        consume(TOKEN_COLON, "Expect ':' between a map key and its value.");
        values[count] = expression();
        count++;
      } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RBRACE, "Expect '}' after map entries.");
    return newMap(keys, values, count, line);
  }
  errorAt(&parser.current, "Expect a value or '('.");
  // CRITICAL for error recovery: consume the offending token so the parser
  // always makes forward progress. Without this, a token that cannot start an
  // expression (e.g. a stray '}') is never advanced past, and the recovery loop
  // — which may early-return from synchronize() on a stale ';' — spins forever.
  // We only compile when parsing fully succeeds, so returning NULL here is safe.
  if (!check(TOKEN_EOF))
    advance();
  return NULL;
}

// --- statements ------------------------------------------------------------

static Node *statement(void);
static Node *declaration(void);

// After a syntax error we "panic": skip tokens until we reach a likely
// statement boundary, so parsing can resume and report further independent
// errors instead of one cascade. We stop just after a ';' (the end of the bad
// statement) or just before a token that clearly begins a new statement. This
// is classic panic-mode error recovery.
static void synchronize(void) {
  parser.panicMode = false;
  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON)
      return; // just finished a statement; the next one can parse cleanly
    switch (parser.current.type) {
    case TOKEN_FN:
    case TOKEN_LET:
    case TOKEN_PRINT:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_FOR:
    case TOKEN_RETURN:
      return; // a keyword that starts a declaration/statement — resume here
    default:
      break;
    }
    advance();
  }
}

static Node *printStatement(void) {
  int line = parser.previous.line; // the 'print' keyword's line
  Node *value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  return newPrint(value, line);
}

static Node *expressionStatement(void) {
  int line = parser.current.line;
  Node *expr = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  // The wrapper records that this expression's value is to be DISCARDED — the
  // compiler will emit an OP_POP after it. Distinguishing this from `print`
  // here, at parse time, keeps the compiler simple.
  return newExprStmt(expr, line);
}

// `{ declaration* }` — parse statements into a fresh Program until the closing
// brace. The block OWNS that Program; the scope itself (which names are local) is
// purely a COMPILE-TIME concern handled later by the compiler, so the parser just
// records the nesting structure here.
static Node *block(void) {
  int line = parser.previous.line; // the '{'
  Program *body = malloc(sizeof(Program));
  if (body == NULL) {
    fprintf(stderr, "cnano: out of memory allocating block\n");
    exit(70);
  }
  initProgram(body);

  while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
    writeProgram(body, declaration());
    if (parser.panicMode)
      synchronize();
  }
  consume(TOKEN_RBRACE, "Expect '}' after block.");
  return newBlock(body, line);
}

static Node *varDeclaration(void); // used by forStatement's initialiser clause
static Type *parseType(void);      // used by forStatement's let-init clause

// `if (cond) thenStmt [else elseStmt]`. The parens are required (cnano follows
// the C family here). The branches are ordinary statements, so `if (c) { ... }`
// works because a block is a statement.
static Node *ifStatement(void) {
  int line = parser.previous.line; // the 'if'
  consume(TOKEN_LPAREN, "Expect '(' after 'if'.");
  Node *condition = expression();
  consume(TOKEN_RPAREN, "Expect ')' after condition.");
  Node *then = statement();
  Node *otherwise = NULL;
  if (match(TOKEN_ELSE))
    otherwise = statement(); // a bare `else` binds to the nearest `if`
  return newIf(condition, then, otherwise, line);
}

static Node *whileStatement(void) {
  int line = parser.previous.line; // the 'while'
  consume(TOKEN_LPAREN, "Expect '(' after 'while'.");
  Node *condition = expression();
  consume(TOKEN_RPAREN, "Expect ')' after condition.");
  Node *body = statement();
  return newWhile(condition, body, line);
}

// `for (init; cond; update) body` — implemented entirely as SYNTACTIC SUGAR over
// constructs we already have. There is no for-loop node and no for-loop opcode;
// the parser rewrites
//
//     for (init; cond; update) body
//
// into the equivalent
//
//     { init; while (cond) { body; update; } }
//
// The outer block scopes the loop variable; the while drives the iteration; the
// update is appended to the body. A missing condition becomes `true` (infinite
// loop). This is the same trick step 1 used for `!=`/`<=`: keep the core tiny,
// express conveniences by lowering them to it. Desugaring a whole statement form
// (not just an operator) is a powerful demonstration of the idea.
// Allocate a Program (a growable node list) on the heap — the container a block
// node owns. Small helper so the desugars below read cleanly.
static Program *makeProgram(void) {
  Program *p = malloc(sizeof(Program));
  if (p == NULL) {
    fprintf(stderr, "cnano: out of memory desugaring a loop\n");
    exit(70);
  }
  initProgram(p);
  return p;
}

// Desugar `for (let VAR in COLL) BODY` into an index loop over a hidden sequence:
//
//   { let $seq = $for_iter(COLL); let $i = 0;
//     while ($i < $seq.len()) { let VAR = $seq[$i]; BODY; $i = $i + 1; } }
//
// $for_iter (a hidden builtin) yields the array to walk — the array itself, or a
// map's keys — so this one shape covers both, even when COLL's type is dynamic.
// The synthesised names use a leading '$' (unlexable), so they cannot collide
// with user variables; block scoping makes nested for-in loops independent.
static Node *desugarForIn(ObjString *var, Node *coll, Node *body, int line) {
  ObjString *seq = copyString("$seq", 4);
  ObjString *idx = copyString("$i", 2);

  // let $seq = $for_iter(COLL);
  Node **iterArgs = malloc(sizeof(Node *));
  if (iterArgs == NULL) {
    fprintf(stderr, "cnano: out of memory desugaring for-in\n");
    exit(70);
  }
  iterArgs[0] = coll;
  Node *iterCall =
      newCall(newVarGet(copyString("$for_iter", 9), line), iterArgs, 1, line);
  Node *declSeq = newVarDecl(seq, iterCall, typeAny(), line);
  Node *declIdx = newVarDecl(idx, newInt(0, line), typeAny(), line);

  // while ($i < $seq.len()) { ... }
  Node *lenCall = newInvoke(newVarGet(seq, line), copyString("len", 3), NULL, 0, line);
  Node *cond = newBinary(OP_NODE_LESS, newVarGet(idx, line), lenCall, line);

  Node *element = newIndexGet(newVarGet(seq, line), newVarGet(idx, line), line);
  Node *declVar = newVarDecl(var, element, typeAny(), line);
  Node *incr = newAssign(
      idx, newBinary(OP_NODE_ADD, newVarGet(idx, line), newInt(1, line), line),
      line);
  Program *loopBody = makeProgram();
  writeProgram(loopBody, declVar);
  writeProgram(loopBody, body);
  writeProgram(loopBody, newExprStmt(incr, line));
  Node *whileNode = newWhile(cond, newBlock(loopBody, line), line);

  Program *outer = makeProgram();
  writeProgram(outer, declSeq);
  writeProgram(outer, declIdx);
  writeProgram(outer, whileNode);
  return newBlock(outer, line);
}

static Node *forStatement(void) {
  int line = parser.previous.line; // the 'for'
  consume(TOKEN_LPAREN, "Expect '(' after 'for'.");

  // --- initialiser clause (also the for-in fork) ---
  Node *initializer = NULL;
  if (match(TOKEN_SEMICOLON)) {
    initializer = NULL; // no initialiser
  } else if (match(TOKEN_LET)) {
    // After `let NAME`, one token of lookahead distinguishes the two loops:
    //   `in`  -> for-in;   `:` or `=` -> a C-style let-initialiser.
    consume(TOKEN_IDENTIFIER, "Expect a variable name after 'let'.");
    ObjString *name = copyString(parser.previous.start, parser.previous.length);
    if (match(TOKEN_IN)) {
      Node *coll = expression();
      consume(TOKEN_RPAREN, "Expect ')' after the for-in collection.");
      Node *body = statement();
      return desugarForIn(name, coll, body, line);
    }
    // C-style let-initialiser: finish parsing `[: TYPE] = EXPR ;` by hand (we have
    // already consumed `let NAME`, so we can't call varDeclaration here).
    Type *declaredType = match(TOKEN_COLON) ? parseType() : typeAny();
    consume(TOKEN_EQUAL, "Expect '=' after the loop variable name.");
    Node *init = expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after the loop initialiser.");
    initializer = newVarDecl(name, init, declaredType, line);
  } else {
    initializer = expressionStatement(); // consumes its own trailing ';'
  }

  // --- condition clause (optional) ---
  Node *condition = NULL;
  if (!check(TOKEN_SEMICOLON))
    condition = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

  // --- update clause (optional) ---
  Node *update = NULL;
  if (!check(TOKEN_RPAREN))
    update = expression();
  consume(TOKEN_RPAREN, "Expect ')' after for clauses.");

  Node *body = statement();

  // --- desugar into blocks + while ---
  // If there is an update, splice it after the body inside a fresh block:
  //   { body; update; }
  if (update != NULL) {
    Program *bodyBlock = malloc(sizeof(Program));
    if (bodyBlock == NULL) {
      fprintf(stderr, "cnano: out of memory desugaring for-loop\n");
      exit(70);
    }
    initProgram(bodyBlock);
    writeProgram(bodyBlock, body);
    writeProgram(bodyBlock, newExprStmt(update, line));
    body = newBlock(bodyBlock, line);
  }

  // A missing condition means "loop forever": substitute the literal `true`.
  if (condition == NULL)
    condition = newBool(true, line);
  body = newWhile(condition, body, line);

  // If there is an initialiser, wrap the whole thing in a block so the loop
  // variable's scope is the loop:  { init; while (...) {...} }
  if (initializer != NULL) {
    Program *outer = malloc(sizeof(Program));
    if (outer == NULL) {
      fprintf(stderr, "cnano: out of memory desugaring for-loop\n");
      exit(70);
    }
    initProgram(outer);
    writeProgram(outer, initializer);
    writeProgram(outer, body);
    body = newBlock(outer, line);
  }

  return body;
}

// `return EXPR? ;` — return from the enclosing function. A bare `return;`
// returns nil (handled by the compiler). Returning from the top-level script is
// rejected at COMPILE time, not here, since the parser doesn't track that.
static Node *returnStatement(void) {
  int line = parser.previous.line; // the 'return'
  Node *value = NULL;
  if (!check(TOKEN_SEMICOLON))
    value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
  return newReturn(value, line);
}

static Node *statement(void) {
  if (match(TOKEN_PRINT))
    return printStatement();
  if (match(TOKEN_IF))
    return ifStatement();
  if (match(TOKEN_WHILE))
    return whileStatement();
  if (match(TOKEN_FOR))
    return forStatement();
  if (match(TOKEN_RETURN))
    return returnStatement();
  if (match(TOKEN_LBRACE))
    return block();
  return expressionStatement();
}

// Parse a TYPE expression and return its structured Type*. A type is one of:
//   * a primitive name: int / bool / str / any (identifiers) or the `nil` keyword
//   * an array type:  [ TYPE ]
//   * a map type:     { TYPE : TYPE }
// Types nest, so this recurses: `[[int]]` and `{str: [int]}` both parse. Type
// names are NOT reserved keywords — they are recognised only here, in annotation
// position, so `int` etc. remain usable as ordinary variable names elsewhere.
static Type *parseType(void) {
  if (match(TOKEN_LBRACKET)) { // [ ELEMENT ]
    Type *element = parseType();
    consume(TOKEN_RBRACKET, "Expect ']' to close an array type.");
    return typeArray(element);
  }
  if (match(TOKEN_LBRACE)) { // { KEY : VALUE }
    Type *key = parseType();
    consume(TOKEN_COLON, "Expect ':' between a map's key and value types.");
    Type *value = parseType();
    consume(TOKEN_RBRACE, "Expect '}' to close a map type.");
    return typeMap(key, value);
  }
  if (match(TOKEN_NIL)) // `nil` is a keyword (the literal) but also a type name
    return typeNil();
  if (match(TOKEN_IDENTIFIER)) {
    int len = parser.previous.length;
    const char *s = parser.previous.start;
    if (len == 3 && memcmp(s, "int", 3) == 0)
      return typeInt();
    if (len == 4 && memcmp(s, "bool", 4) == 0)
      return typeBool();
    if (len == 3 && memcmp(s, "str", 3) == 0)
      return typeStr();
    if (len == 3 && memcmp(s, "any", 3) == 0)
      return typeAny();
    errorAt(&parser.previous,
            "Unknown type name (expected int, bool, str, nil, any, [T], or {K: V}).");
    return typeAny();
  }
  errorAt(&parser.current, "Expect a type after ':'.");
  return typeAny();
}

// `let NAME [: TYPE] = EXPR ;` — declare and initialise a variable. The type
// annotation is optional; omitted means TY_ANY (stay dynamic). We require an
// initialiser for simplicity, sidestepping the "uninitialised variable" question.
static Node *varDeclaration(void) {
  int line = parser.previous.line; // the 'let' keyword's line
  consume(TOKEN_IDENTIFIER, "Expect variable name after 'let'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  Type *declaredType = match(TOKEN_COLON) ? parseType() : typeAny();
  consume(TOKEN_EQUAL, "Expect '=' after variable name.");
  Node *initializer = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  return newVarDecl(name, initializer, declaredType, line);
}

// `fn NAME ( params ) { body }`. We parse the parameter names into a heap array,
// then the body as a block. The compiler turns this into an ObjFunction. Like
// `let`, a function declaration BINDS a name (so functions can be called, and
// can recurse / be mutually recursive among globals).
static Node *funDeclaration(void) {
  int line = parser.previous.line; // the 'fn'
  consume(TOKEN_IDENTIFIER, "Expect function name after 'fn'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);

  consume(TOKEN_LPAREN, "Expect '(' after function name.");
  ObjString **params = NULL;
  Type **paramTypes = NULL;
  int paramCount = 0;
  int capacity = 0;
  if (!check(TOKEN_RPAREN)) {
    do {
      if (paramCount == 255) {
        errorAt(&parser.current, "Cannot have more than 255 parameters.");
        break;
      }
      consume(TOKEN_IDENTIFIER, "Expect parameter name.");
      if (paramCount + 1 > capacity) {
        capacity = capacity < 4 ? 4 : capacity * 2;
        params = realloc(params, sizeof(ObjString *) * capacity);
        paramTypes = realloc(paramTypes, sizeof(Type *) * capacity);
        if (params == NULL || paramTypes == NULL) {
          fprintf(stderr, "cnano: out of memory parsing parameters\n");
          exit(70);
        }
      }
      params[paramCount] =
          copyString(parser.previous.start, parser.previous.length);
      // Optional `: TYPE` per parameter; default any.
      paramTypes[paramCount] = match(TOKEN_COLON) ? parseType() : typeAny();
      paramCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RPAREN, "Expect ')' after parameters.");

  // Optional return-type annotation: `fn f(...) : TYPE { ... }`. Default any.
  Type *returnType = match(TOKEN_COLON) ? parseType() : typeAny();

  consume(TOKEN_LBRACE, "Expect '{' before function body.");
  Node *bodyBlock = block(); // parses up to and including the closing '}'
  // block() returns a NODE_BLOCK owning a Program; unwrap it for the fun node.
  Program *body = bodyBlock->as.block;
  bodyBlock->as.block = NULL; // detach so freeing the wrapper won't free body
  freeNode(bodyBlock);

  return newFun(name, params, paramTypes, paramCount, returnType, body, line);
}

// One level above statement(): a declaration is a `fn`, a `let`, or any
// statement. This is the natural synchronisation point for errors.
static Node *declaration(void) {
  if (match(TOKEN_FN))
    return funDeclaration();
  if (match(TOKEN_LET))
    return varDeclaration();
  return statement();
}

bool parse(const char *source, Program *out) {
  initLexer(source);
  initProgram(out);
  parser.hadError = false;
  parser.panicMode = false;
  advance(); // prime `current` with the first token

  // The program loop: parse statements until end of file. On a syntax error we
  // synchronize and keep going, collecting as many independent errors as we can
  // in a single run — much friendlier than stopping at the first.
  while (!check(TOKEN_EOF)) {
    Node *decl = declaration();
    writeProgram(out, decl);
    if (parser.panicMode)
      synchronize();
  }

  return !parser.hadError;
}
