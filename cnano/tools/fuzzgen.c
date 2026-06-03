// fuzzgen.c — a tiny generator of (mostly malformed) cnano programs, for fuzzing.
//
// Given a seed on the command line it prints one pseudo-random program to stdout.
// `tools/fuzz.sh` runs thousands of these through the compiler and checks it never
// CRASHES — only ever produces a clean error. The goal isn't valid programs; it's
// the weird, hostile inputs that expose missing bounds checks and unguarded
// recursion. The generator is deterministic in its seed, so any crash reproduces.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// A small xorshift PRNG — deterministic and dependency-free.
static uint32_t state;
static uint32_t rng(void) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
static int pick(int n) { return (int)(rng() % (uint32_t)n); }

// The vocabulary: keywords, operators, punctuation, identifiers, and literals.
// Drawing from real tokens (rather than random bytes) reaches far deeper into the
// lexer, parser, and type checker than noise would.
static const char *kVocab[] = {
    "let ", "const ", "fn ", "if ", "else ", "while ", "for ", "return ",
    "print ", "struct ", "enum ", "match ", "import ", "true ", "false ", "nil ",
    "and ", "or ", "is ", "yield ", "try ", "catch ", "throw ", "break ",
    "continue ", "+ ", "- ", "* ", "/ ", "% ", "( ", ") ", "{ ", "} ", "[ ",
    "] ", "< ", "> ", "== ", "!= ", "= ", "; ", ": ", ", ", ". ", "! ", "& ",
    "| ", "^ ", "<< ", ">> ", "? ", "=> ", "x ", "y ", "foo ", "n ", "a ", "b ",
    "0 ", "1 ", "42 ", "-7 ", "3.14 ", "\"hi\" ", "\"x\\n\" ", "100000 ",
    "999999999999999999 ", "0xFF ", "0b1010 ",
};
static const int kVocabCount = (int)(sizeof(kVocab) / sizeof(kVocab[0]));

// A few seed snippets to mutate — valid-ish code whose small corruptions probe
// the boundaries between accept and reject.
static const char *kSeeds[] = {
    "fn f(n) { if (n < 2) { return n; } return f(n-1) + f(n-2); } print f(10);",
    "let a = [1, 2, 3]; for (let i = 0; i < 3; i = i + 1) { print a[i]; }",
    "struct P { x: int, y: int } let p = P(1, 2); print p.x + p.y;",
    "enum C { R, G, B } fn name(c: C): str { match (c) { C.R => return \"r\"; _ => return \"?\"; } }",
    "let m = {\"k\": 1}; print m[\"k\"]; fn g<T>(v: T): T { return v; } print g(5);",
};
static const int kSeedCount = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

int main(int argc, char **argv) {
  state = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1;
  if (state == 0)
    state = 0x9e3779b9; // xorshift must not start at zero

  switch (pick(4)) {
  case 0: { // token salad
    int count = 5 + pick(160);
    for (int i = 0; i < count; i++)
      fputs(kVocab[pick(kVocabCount)], stdout);
    break;
  }
  case 1: { // deep nesting — probes recursive-descent stack depth
    const char *open[] = {"(", "[", "-", "!", "("};
    const char *close = ")";
    int depth = 50 + pick(20000);
    fputs("print ", stdout);
    for (int i = 0; i < depth; i++)
      fputs(open[pick(5)], stdout);
    fputs("1", stdout);
    for (int i = 0; i < depth; i++)
      fputs(close, stdout);
    fputs(";", stdout);
    break;
  }
  case 2: { // a long flat run of statements
    int count = 10 + pick(120);
    for (int i = 0; i < count; i++) {
      fputs(kVocab[pick(kVocabCount)], stdout);
      if (pick(6) == 0)
        fputs(";\n", stdout);
    }
    break;
  }
  default: { // mutate a seed snippet (char flips + random truncation)
    const char *base = kSeeds[pick(kSeedCount)];
    size_t len = 0;
    while (base[len])
      len++;
    if (len > 0 && pick(2) == 0)
      len = 1 + pick((int)len); // truncate sometimes
    for (size_t i = 0; i < len; i++) {
      char c = base[i];
      if (pick(20) == 0)
        c = (char)(32 + pick(95)); // flip ~5% of characters to a printable byte
      fputc(c, stdout);
    }
    break;
  }
  }
  fputc('\n', stdout);
  return 0;
}
