/* Minimal JSON: parse a whole document into an arena value tree, and
   serialise a value tree back out. Only what the site builder needs:
   objects, arrays, strings, numbers, true/false/null. Strings are stored
   as UTF-8 (decoded from \u escapes, surrogate pairs combined; an
   unpaired surrogate becomes U+FFFD -- the case generator never emits one).
   Numbers are kept as double; the builder only uses integral ones. */
#ifndef JSON_H
#define JSON_H

#include "common.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
  JType t;
  bool b;
  double num;
  const char *str; /* J_STR: UTF-8 bytes, NUL-terminated */
  size_t len;      /* J_STR: byte length */
  JVal **items;    /* J_ARR / J_OBJ values */
  const char **keys; /* J_OBJ keys */
  size_t n, cap;
};

typedef struct {
  const char *p, *end;
  Arena *a;
  bool err;
} JParser;

static JVal *jnew(Arena *a, JType t) {
  JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
  memset(v, 0, sizeof(*v));
  v->t = t;
  return v;
}

static void jarr_push(Arena *a, JVal *arr, JVal *item) {
  if (arr->n == arr->cap) {
    size_t ncap = arr->cap ? arr->cap * 2 : 8;
    JVal **ni = (JVal **)arena_alloc(a, ncap * sizeof(JVal *));
    if (arr->items) memcpy(ni, arr->items, arr->n * sizeof(JVal *));
    arr->items = ni;
    arr->cap = ncap;
  }
  arr->items[arr->n++] = item;
}

static void jobj_put(Arena *a, JVal *obj, const char *key, JVal *val) {
  if (obj->n == obj->cap) {
    size_t ncap = obj->cap ? obj->cap * 2 : 8;
    JVal **ni = (JVal **)arena_alloc(a, ncap * sizeof(JVal *));
    const char **nk = (const char **)arena_alloc(a, ncap * sizeof(char *));
    if (obj->items) {
      memcpy(ni, obj->items, obj->n * sizeof(JVal *));
      memcpy(nk, obj->keys, obj->n * sizeof(char *));
    }
    obj->items = ni;
    obj->keys = nk;
    obj->cap = ncap;
  }
  obj->keys[obj->n] = key;
  obj->items[obj->n] = val;
  obj->n++;
}

static JVal *jobj_get(const JVal *obj, const char *key) {
  if (!obj || obj->t != J_OBJ) return NULL;
  for (size_t i = 0; i < obj->n; i++)
    if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
  return NULL;
}

static const char *jstr(const JVal *v) {
  return (v && v->t == J_STR) ? v->str : NULL;
}

static void jp_ws(JParser *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int jp_hex4(JParser *j) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    if (j->p >= j->end) return -1;
    char c = *j->p++;
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

/* Parse a JSON string starting at the opening quote. Returns arena string. */
static const char *jp_string_raw(JParser *j, size_t *out_len) {
  StrBuf b;
  sb_init(&b);
  j->p++; /* opening quote */
  while (j->p < j->end) {
    unsigned char c = (unsigned char)*j->p;
    if (c == '"') {
      j->p++;
      char *s = arena_strdup_n(j->a, b.buf, b.len);
      *out_len = b.len;
      sb_free(&b);
      return s;
    }
    if (c == '\\') {
      j->p++;
      if (j->p >= j->end) break;
      char e = *j->p++;
      switch (e) {
        case '"': sb_addc(&b, '"'); break;
        case '\\': sb_addc(&b, '\\'); break;
        case '/': sb_addc(&b, '/'); break;
        case 'b': sb_addc(&b, '\b'); break;
        case 'f': sb_addc(&b, '\f'); break;
        case 'n': sb_addc(&b, '\n'); break;
        case 'r': sb_addc(&b, '\r'); break;
        case 't': sb_addc(&b, '\t'); break;
        case 'u': {
          int cp = jp_hex4(j);
          if (cp < 0) {
            j->err = true;
            break;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* high surrogate: try a following low one */
            if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
              const char *save = j->p;
              j->p += 2;
              int lo = jp_hex4(j);
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                j->p = save;
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          char enc[4];
          int n = utf8_encode((uint32_t)cp, enc);
          sb_addn(&b, enc, (size_t)n);
          break;
        }
        default:
          j->err = true;
          break;
      }
      if (j->err) break;
      continue;
    }
    if (c < 0x20) {
      j->err = true;
      break;
    }
    /* bulk-copy a run of ordinary bytes (the old code added one byte at
       a time through the string buffer) */
    const char *run = j->p;
    while (j->p < j->end) {
      unsigned char d = (unsigned char)*j->p;
      if (d == '"' || d == '\\' || d < 0x20) break;
      j->p++;
    }
    sb_addn(&b, run, (size_t)(j->p - run));
  }
  sb_free(&b);
  j->err = true;
  return NULL;
}

static JVal *jp_value(JParser *j);

static JVal *jp_value(JParser *j) {
  jp_ws(j);
  if (j->p >= j->end) {
    j->err = true;
    return NULL;
  }
  char c = *j->p;
  if (c == '{') {
    j->p++;
    JVal *o = jnew(j->a, J_OBJ);
    jp_ws(j);
    if (j->p < j->end && *j->p == '}') {
      j->p++;
      return o;
    }
    for (;;) {
      jp_ws(j);
      if (j->p >= j->end || *j->p != '"') {
        j->err = true;
        return NULL;
      }
      size_t klen = 0;
      const char *key = jp_string_raw(j, &klen);
      if (j->err) return NULL;
      jp_ws(j);
      if (j->p >= j->end || *j->p != ':') {
        j->err = true;
        return NULL;
      }
      j->p++;
      JVal *v = jp_value(j);
      if (j->err) return NULL;
      jobj_put(j->a, o, key, v);
      jp_ws(j);
      if (j->p < j->end && *j->p == ',') {
        j->p++;
        continue;
      }
      if (j->p < j->end && *j->p == '}') {
        j->p++;
        return o;
      }
      j->err = true;
      return NULL;
    }
  }
  if (c == '[') {
    j->p++;
    JVal *arr = jnew(j->a, J_ARR);
    jp_ws(j);
    if (j->p < j->end && *j->p == ']') {
      j->p++;
      return arr;
    }
    for (;;) {
      JVal *v = jp_value(j);
      if (j->err) return NULL;
      jarr_push(j->a, arr, v);
      jp_ws(j);
      if (j->p < j->end && *j->p == ',') {
        j->p++;
        continue;
      }
      if (j->p < j->end && *j->p == ']') {
        j->p++;
        return arr;
      }
      j->err = true;
      return NULL;
    }
  }
  if (c == '"') {
    size_t len = 0;
    const char *s = jp_string_raw(j, &len);
    if (j->err) return NULL;
    JVal *v = jnew(j->a, J_STR);
    v->str = s;
    v->len = len;
    return v;
  }
  if (c == 't') {
    if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) {
      j->p += 4;
      JVal *v = jnew(j->a, J_BOOL);
      v->b = true;
      return v;
    }
  } else if (c == 'f') {
    if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) {
      j->p += 5;
      JVal *v = jnew(j->a, J_BOOL);
      v->b = false;
      return v;
    }
  } else if (c == 'n') {
    if (j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) {
      j->p += 4;
      return jnew(j->a, J_NULL);
    }
  } else {
    /* number */
    const char *start = j->p;
    if (j->p < j->end && *j->p == '-') j->p++;
    while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) j->p++;
    if (j->p == start) {
      j->err = true;
      return NULL;
    }
    char tmp[64];
    size_t n = (size_t)(j->p - start);
    if (n >= sizeof(tmp)) {
      j->err = true;
      return NULL;
    }
    memcpy(tmp, start, n);
    tmp[n] = 0;
    JVal *v = jnew(j->a, J_NUM);
    v->num = strtod(tmp, NULL);
    return v;
  }
  j->err = true;
  return NULL;
}

/* Parse a whole JSON document; NULL on error. */
static JVal *json_parse(Arena *a, const char *text, size_t len) {
  JParser j = {text, text + len, a, false};
  JVal *v = jp_value(&j);
  if (j.err) return NULL;
  jp_ws(&j);
  if (j.p != j.end) return NULL;
  return v;
}

/* ---------------------------------------------------------------- serialise */

static void json_escape_into(StrBuf *b, const char *s, size_t n) {
  sb_addc(b, '"');
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"') {
      sb_addn(b, "\\\"", 2);
      i++;
    } else if (c == '\\') {
      sb_addn(b, "\\\\", 2);
      i++;
    } else if (c < 0x20) {
      char tmp[8];
      int m = snprintf(tmp, sizeof(tmp), "\\u%04x", c);
      sb_addn(b, tmp, (size_t)m);
      i++;
    } else {
      /* copy a run of safe bytes (ASCII printable, or UTF-8 multibyte) */
      size_t j = i;
      while (j < n) {
        unsigned char d = (unsigned char)s[j];
        if (d == '"' || d == '\\' || d < 0x20) break;
        j++;
      }
      sb_addn(b, s + i, j - i);
      i = j;
    }
  }
  sb_addc(b, '"');
}

static int jkey_cmp(const void *a, const void *b) {
  const char * const *x = (const char * const *)a;
  const char * const *y = (const char * const *)b;
  return strcmp(*x, *y);
}

static void json_write(StrBuf *b, const JVal *v) {
  switch (v->t) {
    case J_NULL:
      sb_addn(b, "null", 4);
      break;
    case J_BOOL:
      sb_addn(b, v->b ? "true" : "false", v->b ? 4 : 5);
      break;
    case J_NUM: {
      double d = v->num;
      if (d == (double)(long long)d && d >= -9007199254740992.0 && d <= 9007199254740992.0) {
        char tmp[32];
        int m = snprintf(tmp, sizeof(tmp), "%lld", (long long)d);
        sb_addn(b, tmp, (size_t)m);
      } else {
        char tmp[40];
        int m = snprintf(tmp, sizeof(tmp), "%.17g", d);
        sb_addn(b, tmp, (size_t)m);
      }
      break;
    }
    case J_STR:
      json_escape_into(b, v->str, v->len);
      break;
    case J_ARR: {
      sb_addc(b, '[');
      for (size_t i = 0; i < v->n; i++) {
        if (i) sb_addc(b, ',');
        json_write(b, v->items[i]);
      }
      sb_addc(b, ']');
      break;
    }
    case J_OBJ: {
      /* keys in code-unit (byte) order, for deterministic output */
      const char **keys = (const char **)malloc(v->n * sizeof(char *));
      size_t *idx = (size_t *)malloc(v->n * sizeof(size_t));
      for (size_t i = 0; i < v->n; i++) {
        keys[i] = v->keys[i];
        idx[i] = i;
      }
      /* insertion sort by key (n is small) */
      for (size_t i = 1; i < v->n; i++) {
        const char *k = keys[i];
        size_t id = idx[i];
        size_t k2 = i;
        while (k2 > 0 && strcmp(keys[k2 - 1], k) > 0) {
          keys[k2] = keys[k2 - 1];
          idx[k2] = idx[k2 - 1];
          k2--;
        }
        keys[k2] = k;
        idx[k2] = id;
      }
      (void)jkey_cmp;
      sb_addc(b, '{');
      for (size_t i = 0; i < v->n; i++) {
        if (i) sb_addc(b, ',');
        json_escape_into(b, keys[i], strlen(keys[i]));
        sb_addc(b, ':');
        json_write(b, v->items[idx[i]]);
      }
      sb_addc(b, '}');
      free(keys);
      free(idx);
      break;
    }
  }
}

#endif
