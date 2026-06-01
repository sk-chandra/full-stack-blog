#include <string.h>

#include "suggest.h"

#define MAX_NAME 64 // names longer than this skip suggestion (kept allocation-free)

int editDistance(const char *a, const char *b) {
  int la = (int)strlen(a), lb = (int)strlen(b);
  if (la > MAX_NAME || lb > MAX_NAME)
    return MAX_NAME * 2; // "too far" — don't bother
  // Classic two-row dynamic program. row[j] = distance(a[0..i), b[0..j)).
  int prev[MAX_NAME + 1], cur[MAX_NAME + 1];
  for (int j = 0; j <= lb; j++)
    prev[j] = j;
  for (int i = 1; i <= la; i++) {
    cur[0] = i;
    for (int j = 1; j <= lb; j++) {
      int del = prev[j] + 1;          // delete a[i-1]
      int ins = cur[j - 1] + 1;       // insert b[j-1]
      int sub = prev[j - 1] + (a[i - 1] != b[j - 1] ? 1 : 0); // substitute/keep
      int best = del < ins ? del : ins;
      cur[j] = best < sub ? best : sub;
    }
    memcpy(prev, cur, sizeof(int) * (lb + 1));
  }
  return prev[lb];
}

const char *closestName(const char *target, const char *const *candidates,
                        int count) {
  int tlen = (int)strlen(target);
  // Allow more edits for longer names; a 3-letter typo shouldn't match anything
  // two edits away (that's a different word), but a 12-letter one can.
  int threshold = tlen <= 4 ? 1 : tlen <= 8 ? 2 : 3;
  const char *best = NULL;
  int bestDist = threshold + 1;
  for (int i = 0; i < count; i++) {
    if (candidates[i] == NULL)
      continue;
    int d = editDistance(target, candidates[i]);
    if (d < bestDist) {
      bestDist = d;
      best = candidates[i];
    }
  }
  return best; // NULL if nothing was within the threshold
}
