# The cnano language specification

This is the reference for cnano **as implemented** — every production below is
checked against the parser in `src/parser.c`, and the test suite holds the
implementation to it. Writing the grammar down is itself a classic step in a
language's life: until now the grammar lived *implicitly* in recursive-descent
code; this document makes it an artifact you can read, argue with, and test
against.

Notation: EBNF. `*` = zero or more, `?` = optional, `|` = alternatives,
`"…"` = literal text, UPPERCASE = a token from the lexer.

---

## 1. Lexical structure

### 1.1 Comments and whitespace

```
comment     -> "//" <anything up to end of line>
```

Whitespace (spaces, tabs, newlines) separates tokens and is otherwise
insignificant. There are no block comments.

### 1.2 Integer and float literals

```
INT         -> DIGIT+
             | "0x" HEXDIGIT+          // hexadecimal
             | "0b" BINDIGIT+          // binary
FLOAT       -> DIGIT+ "." DIGIT+       // the dot needs digits on BOTH sides
```

Integers are 64-bit signed (`int64_t`); floats are IEEE-754 doubles. A literal
like `1.` or `.5` is not a float — the dot would parse as a member access.

### 1.3 String literals

```
STRING      -> '"' ( CHAR | ESCAPE | INTERP )* '"'
ESCAPE      -> "\n" | "\t" | "\\" | "\"" | "\$"
INTERP      -> "${" expression "}"     // string interpolation
```

Interpolation splices any expression's printed form into the string:
`"x is ${x + 1}"`. `\$` escapes a literal dollar sign.

### 1.4 Keywords

```
and   break  catch  const  continue  do     else   enum   false  fn
for   if     import in     is        let    match  nil    or     print
return struct throw  true  try      while  yield
```

`_` is not a keyword, but has special meaning as a `match` arm pattern.

### 1.5 Operators and punctuation

```
+  -  *  /  %  &  |  ^  <<  >>  ~  !  <  <=  >  >=  ==  !=
=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
( ) { } [ ]  ,  ;  :  .  ..  ?  =>
```

---

## 2. Grammar

### 2.1 Program and declarations

```
program     -> declaration* EOF

declaration -> importDecl | structDecl | enumDecl | funDecl
             | varDecl | statement

importDecl  -> "import" STRING ";"           // top level only
structDecl  -> "struct" IDENT "{" field ( "," field )* method* "}"
field       -> IDENT ":" type
method      -> "fn" IDENT "(" params? ")" ( ":" type )? funBody
enumDecl    -> "enum" IDENT "{" IDENT ( "," IDENT )* "}"

funDecl     -> "fn" IDENT typeParams? "(" params? ")" ( ":" type )? funBody
typeParams  -> "<" IDENT ( "," IDENT )* ">"  // generic type parameters
params      -> param ( "," param )*
param       -> IDENT ( ":" type )?
funBody     -> block
             | "=>" expression            // shorthand for { return expression; }

varDecl     -> ( "let" | "const" ) IDENT ( ":" type )? "=" expression ";"
```

Notes:
- `import` paths resolve **relative to the importing file** (absolute paths are
  also accepted); each file is spliced in at most once, so diamonds and cycles
  are safe.
- A function whose body contains `yield` is a **generator**: calling it returns
  a suspended Generator object rather than running the body.
- Inside a struct, methods may use `self` to reach the receiver.

### 2.2 Statements

```
statement   -> printStmt | ifStmt | whileStmt | doWhileStmt | forStmt
             | matchStmt | returnStmt | throwStmt | yieldStmt | tryStmt
             | breakStmt | continueStmt | block | exprStmt

printStmt   -> "print" expression ";"
ifStmt      -> "if" "(" expression ")" statement ( "else" statement )?
whileStmt   -> "while" "(" expression ")" statement
doWhileStmt -> "do" statement "while" "(" expression ")" ";"
forStmt     -> "for" "(" ( varDecl | exprStmt | ";" )    // C-style
                         expression? ";" expression? ")" statement
             | "for" "(" "let" IDENT "in" expression ".." expression ")"
                         statement                       // counting (end-exclusive)
             | "for" "(" "let" IDENT "in" expression ")" statement
                                                          // over an array, or a map's keys
matchStmt   -> "match" "(" expression ")" "{" arm+ "}"
arm         -> pattern "=>" statement
pattern     -> expression                  // a value to compare with ==
             | "is" type                   // runtime type test (narrows)
             | "_"                         // wildcard (default)
returnStmt  -> "return" expression? ";"
throwStmt   -> "throw" expression ";"
yieldStmt   -> "yield" expression ";"      // only inside a function
tryStmt     -> "try" statement "catch" "(" IDENT ")" statement
breakStmt   -> "break" ";"
continueStmt-> "continue" ";"
block       -> "{" declaration* "}"
exprStmt    -> expression ";"
```

Notes:
- `for`-loops are **desugared** by the parser into `while` loops; `continue`
  still runs the increment clause.
- A `match` over a **closed domain** (an enum, a bool, or a union) with no `_`
  must cover every case — non-exhaustiveness is a compile error.

### 2.3 Expressions (the precedence ladder)

From loosest to tightest binding; each level is built from the next:

```
expression  -> assignment
assignment  -> lvalue ( "=" | "+=" | "-=" | "*=" | "/=" | "%="
                       | "&=" | "|=" | "^=" | "<<=" | ">>=" ) assignment
             | ternary                       // right-associative
lvalue      -> IDENT | postfix "[" expression "]" | postfix "." IDENT
ternary     -> logicOr ( "?" expression ":" ternary )?   // right-associative
logicOr     -> logicAnd ( "or" logicAnd )*               // short-circuits
logicAnd    -> bitOr ( "and" bitOr )*                    // short-circuits
bitOr       -> bitXor ( "|" bitXor )*
bitXor      -> bitAnd ( "^" bitAnd )*
bitAnd      -> equality ( "&" equality )*
equality    -> comparison ( ( "==" | "!=" ) comparison
                          | "is" type )*     // runtime type test, yields bool
comparison  -> shift ( ( "<" | "<=" | ">" | ">=" ) shift )*
shift       -> term ( ( "<<" | ">>" ) term )*
term        -> factor ( ( "+" | "-" ) factor )*
factor      -> unary ( ( "*" | "/" | "%" ) unary )*
unary       -> ( "-" | "!" | "~" ) unary | postfix
postfix     -> primary ( "(" args? ")"                   // call
                       | "[" expression "]"              // index
                       | "." IDENT ( "(" args? ")" )?    // field / method
                       )*
args        -> expression ( "," expression )*
primary     -> INT | FLOAT | STRING | "true" | "false" | "nil"
             | IDENT
             | "(" expression ")"
             | "[" ( expression ( "," expression )* )? "]"      // array literal
             | "{" ( expression ":" expression
                     ( "," expression ":" expression )* )? "}"  // map literal
             | "fn" "(" params? ")" ( ":" type )? funBody        // lambda
```

All binary operators are **left-associative**; assignment and `?:` are
right-associative. Compound assignment `x OP= e` desugars to `x = x OP e`
(the target is evaluated once where that matters).

### 2.4 Types

```
type        -> nullableType ( "|" nullableType )*   // union: int | str
nullableType-> baseType "?"?                        // T? means T | nil
baseType    -> "int" | "float" | "bool" | "str" | "nil" | "any"
             | "[" type "]"                         // array of T
             | "{" type ":" type "}"                // map of K to V
             | IDENT                                // struct / enum name,
                                                    // or a generic type variable
```

Annotations are optional everywhere (parameters, returns, `let`). Unannotated
values are `any` — the gradual escape hatch — **except** a function's return
type, which is *inferred* from its body when unannotated.

---

## 3. Semantics highlights

- **Truthiness:** only `nil` and `false` are falsey. Every number — including
  `0` — is truthy.
- **Numbers:** `int` and `float` are distinct. Mixed arithmetic promotes to
  float; `int OP int` stays an int (`/` is integer division; `÷0` and `%0` are
  runtime errors; float division by zero is IEEE `inf`/`nan`).
- **`+`** adds numbers or concatenates two strings; anything else is an error.
- **Equality** (`==`) is defined for all values: different types are never
  equal; strings compare by content (they are interned); arrays/maps/structs
  compare by reference.
- **Scoping** is lexical. Globals are late-bound by name; locals live on the
  stack; closures capture variables (not values) via upvalues.
- **Type checking** runs before execution. `any` is compatible with everything;
  unions/nullables must be **narrowed** (`if (x is int)`, `if (x != nil)`)
  before use as a specific type. Struct and enum types are **nominal**;
  array/map types are **structural**.
- **Generics** (`fn id<T>(x: T): T`) are solved per call site by unification and
  then erased — the VM executes one function for every instantiation.
- **Errors:** `throw` any value; `try`/`catch` unwinds to the nearest handler;
  an uncaught throw prints a stack trace naming each frame's file.
- **Evaluation order** is left to right everywhere (operands, arguments,
  array/map literal elements).

## 4. Execution targets

| Target | Command | Subset |
|---|---|---|
| Bytecode VM | `cnano file.cn` | the full language |
| C backend (AOT) | `cnano --native file.cn -o bin` | typed first-order scalars (int/float/bool/str), control flow, functions |
| x86-64 backend | `cnano --asm file.cn` | integer scalars, control flow, functions (≤ 6 params) |

The narrower backends **reject** out-of-subset programs with a diagnostic
rather than miscompiling them; the VM always runs the whole language.
