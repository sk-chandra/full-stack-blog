// realpath() and PATH_MAX are POSIX, not ISO C, so under -std=c11 they are not
// declared by default. Request glibc's default (BSD+POSIX) surface BEFORE any
// include so the prototype is visible — otherwise realpath is implicitly `int`
// and its returned pointer is truncated on 64-bit systems (a crash waiting to
// happen). _DEFAULT_SOURCE is glibc's catch-all for exactly these functions.
#define _DEFAULT_SOURCE

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "object.h" // ObjString fields
#include "parser.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char *readFileOrExit(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "cnano: could not open file \"%s\".\n", path);
    exit(74); // 74 = EX_IOERR
  }
  // Chunked read (no fseek/ftell, which fail on pipes like /dev/stdin).
  size_t capacity = 1024, length = 0;
  char *buffer = malloc(capacity);
  if (buffer == NULL) {
    fprintf(stderr, "cnano: not enough memory to read \"%s\".\n", path);
    exit(74);
  }
  for (;;) {
    if (length + 1 >= capacity) {
      capacity *= 2;
      buffer = realloc(buffer, capacity);
      if (buffer == NULL) {
        fprintf(stderr, "cnano: not enough memory to read \"%s\".\n", path);
        exit(74);
      }
    }
    size_t got = fread(buffer + length, 1, capacity - length - 1, file);
    length += got;
    if (got == 0)
      break;
  }
  buffer[length] = '\0';
  fclose(file);
  return buffer;
}

// --- the once-only "already loaded" set ------------------------------------
// Canonical (realpath) paths of files already spliced in, so a file imported
// from several places — or via a cycle — is included exactly once.
#define MAX_MODULES 256
static char *loadedPaths[MAX_MODULES];
static int loadedCount;

static void resetLoaded(void) {
  for (int i = 0; i < loadedCount; i++)
    free(loadedPaths[i]);
  loadedCount = 0;
}

static bool alreadyLoaded(const char *real) {
  for (int i = 0; i < loadedCount; i++)
    if (strcmp(loadedPaths[i], real) == 0)
      return true;
  return false;
}

static void markLoaded(const char *real) {
  if (loadedCount < MAX_MODULES) {
    char *copy = malloc(strlen(real) + 1);
    if (copy != NULL) {
      strcpy(copy, real);
      loadedPaths[loadedCount++] = copy;
    }
  }
}

// Write the directory portion of `path` into `out` (capacity `cap`). For an
// absolute canonical path this is everything up to the last '/'; with no '/'
// it is ".". The result never has a trailing slash (except the filesystem root).
static void dirOf(const char *path, char *out, size_t cap) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    snprintf(out, cap, ".");
    return;
  }
  size_t len = (size_t)(slash - path);
  if (len == 0)
    len = 1; // the root "/" itself
  if (len >= cap)
    len = cap - 1;
  memcpy(out, path, len);
  out[len] = '\0';
}

static bool resolveInto(Program *src, const char *baseDir, Program *out);

// Parse the file at canonical path `real` (read via `openPath`) and splice its
// resolved statements into `out`. The caller has already deduped/marked `real`.
static bool loadAndSplice(const char *openPath, const char *real, Program *out) {
  char *source = readFileOrExit(openPath);
  Program child;
  if (!parse(source, &child)) {
    free(source);
    freeProgram(&child); // may hold partially-built statements
    return false;
  }
  free(source);
  char childDir[PATH_MAX];
  dirOf(real, childDir, sizeof(childDir));
  bool ok = resolveInto(&child, childDir, out);
  free(child.statements); // nodes were transferred to `out` or freed; free array
  return ok;
}

// Move every statement of `src` into `out`, replacing each top-level NODE_IMPORT
// with the (recursively resolved) statements of the file it names. `src` is fully
// consumed: every node ends up either transferred to `out` or freed, so the
// caller need only free `src`'s backing array. Returns false if any import could
// not be resolved (but still consumes `src` cleanly).
static bool resolveInto(Program *src, const char *baseDir, Program *out) {
  bool ok = true;
  for (int i = 0; i < src->count; i++) {
    Node *stmt = src->statements[i];
    if (stmt->type != NODE_IMPORT) {
      writeProgram(out, stmt); // transfer ownership to the merged program
      continue;
    }
    if (!ok) { // a previous import failed — just drain the rest
      freeNode(stmt);
      continue;
    }
    const char *rel = stmt->as.stringValue->chars;
    char candidate[PATH_MAX];
    if (rel[0] == '/')
      snprintf(candidate, sizeof(candidate), "%s", rel);
    else
      snprintf(candidate, sizeof(candidate), "%s/%s", baseDir, rel);

    char real[PATH_MAX];
    if (realpath(candidate, real) == NULL) {
      fprintf(stderr, "cnano: cannot open imported file \"%s\" (line %d).\n", rel,
              stmt->line);
      ok = false;
      freeNode(stmt);
      continue;
    }
    freeNode(stmt); // the path is captured in `real`; the node is done

    if (alreadyLoaded(real))
      continue; // diamond/cycle: include this file only once
    markLoaded(real);
    if (!loadAndSplice(candidate, real, out))
      ok = false;
  }
  return ok;
}

// Shared finish: splice imports of an already-parsed root `parsed` into `out`.
// `rootReal` (or NULL) seeds the loaded set so a self/cyclic import of the root
// is deduped. Always consumes `parsed` (frees its backing array). On failure,
// frees the partial `out` too.
static bool finishResolve(Program *parsed, const char *baseDir,
                          const char *rootReal, Program *out) {
  initProgram(out);
  resetLoaded();
  if (rootReal != NULL)
    markLoaded(rootReal);
  bool ok = resolveInto(parsed, baseDir, out);
  free(parsed->statements); // nodes consumed by resolveInto
  resetLoaded();
  if (!ok) {
    freeProgram(out);
    return false;
  }
  return true;
}

bool loadModuleFile(const char *path, Program *out) {
  char *source = readFileOrExit(path);
  Program parsed;
  if (!parse(source, &parsed)) {
    free(source);
    freeProgram(&parsed);
    return false;
  }
  free(source);

  char real[PATH_MAX];
  char *rootReal = realpath(path, real); // may be NULL (shouldn't be: we read it)
  char base[PATH_MAX];
  dirOf(rootReal ? real : path, base, sizeof(base));
  return finishResolve(&parsed, base, rootReal, out);
}

bool loadModuleSource(const char *source, Program *out) {
  Program parsed;
  if (!parse(source, &parsed)) {
    freeProgram(&parsed);
    return false;
  }
  return finishResolve(&parsed, ".", NULL, out); // imports relative to CWD
}
