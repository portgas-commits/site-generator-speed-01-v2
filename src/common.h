/* Common utilities for the native makefun-site engine.
   Single-header style helpers: arena allocator, growable string buffer,
   UTF-8 decode/encode, JS-whitespace test, and a small open-addressing
   string map. Everything per-case lives in the arena and is dropped by
   resetting it; nothing is cached from one case to the next. */
#ifndef COMMON_H
#define COMMON_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ---------------------------------------------------------------- arena */

/* A chunked arena: memory is handed out of 1 MiB (or larger) blocks that
   are NEVER moved once handed out. This matters because the code keeps
   live pointers into the arena while allocating more (a JSON tree being
   parsed, a template node list being filled): a realloc-based arena would
   silently invalidate those pointers. */
typedef struct ArenaBlock {
  struct ArenaBlock *next;
  char *base;
  size_t len, cap;
} ArenaBlock;

typedef struct {
  ArenaBlock *head; /* block currently being filled */
  ArenaBlock *all;  /* every block, for freeing */
} Arena;

static inline void arena_init(Arena *a) {
  a->head = NULL;
  a->all = NULL;
}

static inline void *arena_alloc(Arena *a, size_t n) {
  n = (n + 15) & ~(size_t)15;
  ArenaBlock *b = a->head;
  if (!b || b->len + n > b->cap) {
    size_t cap = n > (1u << 20) ? n : (1u << 20);
    ArenaBlock *nb = (ArenaBlock *)malloc(sizeof(ArenaBlock));
    nb->base = (char *)malloc(cap);
    nb->len = 0;
    nb->cap = cap;
    nb->next = NULL;
    if (a->all) {
      ArenaBlock *t = a->all;
      while (t->next) t = t->next;
      t->next = nb;
    } else {
      a->all = nb;
    }
    a->head = nb;
    b = nb;
  }
  void *p = b->base + b->len;
  b->len += n;
  return p;
}

static inline void arena_reset(Arena *a) {
  ArenaBlock *b = a->all;
  while (b) {
    ArenaBlock *nx = b->next;
    free(b->base);
    free(b);
    b = nx;
  }
  a->head = NULL;
  a->all = NULL;
}
static inline void arena_free(Arena *a) { arena_reset(a); }

static inline char *arena_strdup_n(Arena *a, const char *s, size_t n) {
  char *p = (char *)arena_alloc(a, n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

static inline char *arena_strdup(Arena *a, const char *s) {
  return arena_strdup_n(a, s, strlen(s));
}

/* ---------------------------------------------------------------- StrBuf */

typedef struct {
  char *buf;
  size_t len, cap;
} StrBuf;

static inline void sb_init(StrBuf *b) {
  b->cap = 256;
  b->buf = (char *)malloc(b->cap);
  b->len = 0;
  b->buf[0] = 0;
}

static inline void sb_reserve(StrBuf *b, size_t extra) {
  if (b->len + extra + 1 > b->cap) {
    size_t ncap = b->cap;
    while (b->len + extra + 1 > ncap) ncap *= 2;
    b->buf = (char *)realloc(b->buf, ncap);
    b->cap = ncap;
  }
}

static inline void sb_addn(StrBuf *b, const char *s, size_t n) {
  sb_reserve(b, n);
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = 0;
}

static inline void sb_add(StrBuf *b, const char *s) { sb_addn(b, s, strlen(s)); }

static inline void sb_addc(StrBuf *b, char c) {
  sb_reserve(b, 1);
  b->buf[b->len++] = c;
  b->buf[b->len] = 0;
}

static inline void sb_free(StrBuf *b) {
  free(b->buf);
  b->buf = NULL;
  b->len = b->cap = 0;
}

/* ---------------------------------------------------------------- UTF-8 */

/* Decode one UTF-8 code point at s[0..n); returns code point, sets *adv to
   its byte length. Invalid bytes decode as U+FFFD with adv 1. */
static inline uint32_t utf8_decode(const char *s, size_t n, size_t *adv) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) {
    *adv = 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && n >= 2) {
    unsigned char c1 = (unsigned char)s[1];
    if ((c1 & 0xC0) == 0x80) {
      uint32_t cp = ((c & 0x1F) << 6) | (c1 & 0x3F);
      if (cp >= 0x80) {
        *adv = 2;
        return cp;
      }
    }
  } else if ((c & 0xF0) == 0xE0 && n >= 3) {
    unsigned char c1 = (unsigned char)s[1], c2 = (unsigned char)s[2];
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
      uint32_t cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
      if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
        *adv = 3;
        return cp;
      }
    }
  } else if ((c & 0xF8) == 0xF0 && n >= 4) {
    unsigned char c1 = (unsigned char)s[1], c2 = (unsigned char)s[2], c3 = (unsigned char)s[3];
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
      uint32_t cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
      if (cp >= 0x10000 && cp <= 0x10FFFF) {
        *adv = 4;
        return cp;
      }
    }
  }
  *adv = 1;
  return 0xFFFD;
}

/* Encode cp as UTF-8 into out (4 bytes max); returns byte count. */
static inline int utf8_encode(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* ---------------------------------------------------------------- JS whitespace */

/* JavaScript \s: \t \n \v \f \r space U+00A0 U+1680 U+2000-U+200A U+2028
   U+2029 U+202F U+205F U+3000 U+FEFF. (ASCII fast path first.) */
static inline bool is_js_space_cp(uint32_t cp) {
  if (cp < 0x80)
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20;
  return cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

/* Combining mark, Unicode general category M (approximation over the ranges
   that can appear here; the generator's alphabet produces only U+0300-U+036F). */
static inline bool is_combining_mark(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
         (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
         (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x0670 && cp <= 0x0670) ||
         (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
         (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

/* ---------------------------------------------------------------- string map */

/* Open-addressing map from NUL-terminated string keys to void* values.
   Keys are owned by the caller (arena). Iteration order is NOT used anywhere
   that affects output: lookups only. */
typedef struct {
  const char **keys;
  void **vals;
  size_t cap, n;
} StrMap;

static inline uint64_t fnv1a(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static inline void smap_init(StrMap *m) {
  m->cap = 64;
  m->n = 0;
  m->keys = (const char **)calloc(m->cap, sizeof(char *));
  m->vals = (void **)calloc(m->cap, sizeof(void *));
}

static inline void smap_grow(StrMap *m) {
  size_t ocap = m->cap;
  const char **ok = m->keys;
  void **ov = m->vals;
  m->cap *= 2;
  m->keys = (const char **)calloc(m->cap, sizeof(char *));
  m->vals = (void **)calloc(m->cap, sizeof(void *));
  for (size_t i = 0; i < ocap; i++) {
    if (ok[i]) {
      size_t j = fnv1a(ok[i], strlen(ok[i])) & (m->cap - 1);
      while (m->keys[j]) j = (j + 1) & (m->cap - 1);
      m->keys[j] = ok[i];
      m->vals[j] = ov[i];
    }
  }
  free(ok);
  free(ov);
}

static inline void smap_put(StrMap *m, const char *key, void *val) {
  if ((m->n + 1) * 10 > m->cap * 7) smap_grow(m);
  size_t j = fnv1a(key, strlen(key)) & (m->cap - 1);
  while (m->keys[j]) {
    if (strcmp(m->keys[j], key) == 0) {
      m->vals[j] = val;
      return;
    }
    j = (j + 1) & (m->cap - 1);
  }
  m->keys[j] = key;
  m->vals[j] = val;
  m->n++;
}

static inline void *smap_get(const StrMap *m, const char *key) {
  if (m->cap == 0) return NULL;
  size_t j = fnv1a(key, strlen(key)) & (m->cap - 1);
  while (m->keys[j]) {
    if (strcmp(m->keys[j], key) == 0) return m->vals[j];
    j = (j + 1) & (m->cap - 1);
  }
  return NULL;
}

static inline bool smap_has(const StrMap *m, const char *key) {
  return smap_get(m, key) != NULL;
}

static inline void smap_free(StrMap *m) {
  free(m->keys);
  free(m->vals);
  m->keys = NULL;
  m->vals = NULL;
  m->cap = m->n = 0;
}

#endif
