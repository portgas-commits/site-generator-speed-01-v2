/* makefun-site's front matter (lib/frontmatter.js): a strict subset of YAML
   block mappings. Error strings must match the reference exactly -- they go
   into the build report. */
#ifndef FRONTMATTER_C
#define FRONTMATTER_C

#include "common.h"

/* Split text on '\n' exactly like JS split('\n'): a trailing newline leaves
   a trailing empty element. Returns arena array of {ptr,len} lines. */
typedef struct {
  const char *ptr;
  size_t len;
} Line;

typedef struct {
  Line *lines;
  size_t n;
} Lines;

static Lines split_lines(Arena *a, const char *text, size_t len) {
  /* Single pass (the old code counted newlines first, then filled). */
  size_t cap = 16, n = 0;
  Line *lines = (Line *)arena_alloc(a, cap * sizeof(Line));
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || text[i] == '\n') {
      if (n == cap) {
        cap *= 2;
        Line *nl = (Line *)arena_alloc(a, cap * sizeof(Line));
        memcpy(nl, lines, n * sizeof(Line));
        lines = nl;
      }
      lines[n].ptr = text + start;
      lines[n].len = i - start;
      n++;
      start = i + 1;
    }
  }
  Lines out;
  out.lines = lines;
  out.n = n;
  return out;
}

static bool line_is_blank(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != ' ' && p[i] != '\t') return false;
  return true;
}

static bool line_eq(const char *p, size_t n, const char *s) {
  size_t m = strlen(s);
  return n == m && memcmp(p, s, n) == 0;
}

static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
  return 31;
}

/* A parsed key/value pair. value is the arena string (or "true"/"false" for
   draft, or the raw string for others). kind: 0 string, 1 date, 2 tags, 3 bool. */
typedef struct {
  const char *key;
  const char *val; /* string value, or for tags: see tags below */
  const char **tags;
  size_t ntags;
  bool b;
  int kind;
} FmPair;

typedef struct {
  FmPair *pairs;
  size_t n;
  const char *body; /* arena string */
  size_t body_len;
  const char *error; /* arena string when refused, else NULL */
} FrontMatter;

/* Parse one key's value; on error returns an arena error string. */
static const char *fm_value(Arena *a, const char *key, const char *raw, size_t raw_len, FmPair *out) {
  /* stringOf for title/summary */
  if (strcmp(key, "title") == 0 || strcmp(key, "summary") == 0) {
    if (raw_len > 0 && raw[0] == '"') {
      if (raw_len < 2 || raw[raw_len - 1] != '"') {
        char *e = (char *)arena_alloc(a, 64);
        snprintf(e, 64, "key \"%s\": a quoted string is not closed", key);
        return e;
      }
      StrBuf b;
      sb_init(&b);
      for (size_t i = 1; i + 1 < raw_len; i++) {
        char c = raw[i];
        if (c == '\\') {
          char nx = raw[i + 1];
          if ((nx != '"' && nx != '\\') || i + 1 == raw_len - 1) {
            sb_free(&b);
            char *e = (char *)arena_alloc(a, 96);
            snprintf(e, 96, "key \"%s\": a quoted string holds an escape other than \\\" or \\\\", key);
            return e;
          }
          sb_addc(&b, nx);
          i++;
        } else if (c == '"') {
          sb_free(&b);
          char *e = (char *)arena_alloc(a, 80);
          snprintf(e, 80, "key \"%s\": a quoted string holds a stray quote", key);
          return e;
        } else {
          sb_addc(&b, c);
        }
      }
      out->val = arena_strdup_n(a, b.buf, b.len);
      sb_free(&b);
      out->kind = 0;
      return NULL;
    }
    if (raw_len > 0 && raw[0] == '[') {
      char *e = (char *)arena_alloc(a, 80);
      snprintf(e, 80, "key \"%s\": a bare string opens on [", key);
      return e;
    }
    out->val = arena_strdup_n(a, raw, raw_len);
    out->kind = 0;
    return NULL;
  }
  if (strcmp(key, "date") == 0) {
    /* YYYY-MM-DD, real day 1970-01-01..2099-12-31 */
    bool ok = false;
    if (raw_len == 10 && raw[4] == '-' && raw[7] == '-') {
      bool digits = true;
      for (size_t i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (raw[i] < '0' || raw[i] > '9') digits = false;
      }
      if (digits) {
        int y = (raw[0] - '0') * 1000 + (raw[1] - '0') * 100 + (raw[2] - '0') * 10 + (raw[3] - '0');
        int mo = (raw[5] - '0') * 10 + (raw[6] - '0');
        int d = (raw[8] - '0') * 10 + (raw[9] - '0');
        if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= days_in(y, mo)) ok = true;
      }
    }
    if (!ok) return arena_strdup(a, "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
    out->val = arena_strdup_n(a, raw, raw_len);
    out->kind = 1;
    return NULL;
  }
  if (strcmp(key, "tags") == 0) {
    if (raw_len < 2 || raw[0] != '[' || raw[raw_len - 1] != ']')
      return arena_strdup(a, "key \"tags\": not a list [a, b]");
    /* inner, trimmed empty -> [] */
    size_t i0 = 1, i1 = raw_len - 1;
    while (i0 < i1 && (raw[i0] == ' ' || raw[i0] == '\t')) i0++;
    while (i1 > i0 && (raw[i1 - 1] == ' ' || raw[i1 - 1] == '\t')) i1--;
    if (i0 >= i1) {
      out->tags = NULL;
      out->ntags = 0;
      out->kind = 2;
      return NULL;
    }
    /* split inner on ',' and trim each */
    size_t cap = 4, n = 0;
    const char **tags = (const char **)arena_alloc(a, cap * sizeof(char *));
    size_t *tlens = (size_t *)arena_alloc(a, cap * sizeof(size_t));
    size_t start = 1;
    for (size_t i = 1;; i++) {
      if (i == raw_len - 1 || raw[i] == ',') {
        size_t s = start, e = i;
        while (s < e && (raw[s] == ' ' || raw[s] == '\t')) s++;
        while (e > s && (raw[e - 1] == ' ' || raw[e - 1] == '\t')) e--;
        if (n == cap) {
          cap *= 2;
          const char **nt = (const char **)arena_alloc(a, cap * sizeof(char *));
          size_t *nl = (size_t *)arena_alloc(a, cap * sizeof(size_t));
          memcpy(nt, tags, n * sizeof(char *));
          memcpy(nl, tlens, n * sizeof(size_t));
          tags = nt;
          tlens = nl;
        }
        tags[n] = arena_strdup_n(a, raw + s, e - s);
        tlens[n] = e - s;
        n++;
        start = i + 1;
        if (i == raw_len - 1) break;
      }
    }
    for (size_t i = 0; i < n; i++) {
      if (tlens[i] == 0) return arena_strdup(a, "key \"tags\": an empty tag, or one holding \" [ or ]");
      for (size_t k = 0; k < tlens[i]; k++) {
        char c = tags[i][k];
        if (c == '"' || c == '[' || c == ']')
          return arena_strdup(a, "key \"tags\": an empty tag, or one holding \" [ or ]");
      }
    }
    out->tags = tags;
    out->ntags = n;
    out->kind = 2;
    return NULL;
  }
  if (strcmp(key, "draft") == 0) {
    if (raw_len == 4 && memcmp(raw, "true", 4) == 0) {
      out->b = true;
      out->kind = 3;
      return NULL;
    }
    if (raw_len == 5 && memcmp(raw, "false", 5) == 0) {
      out->b = false;
      out->kind = 3;
      return NULL;
    }
    return arena_strdup(a, "key \"draft\": true or false");
  }
  if (strcmp(key, "layout") == 0) {
    bool ok = raw_len > 0;
    for (size_t i = 0; i < raw_len; i++) {
      char c = raw[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = false;
    }
    if (!ok) return arena_strdup(a, "key \"layout\": not a template name");
    out->val = arena_strdup_n(a, raw, raw_len);
    out->kind = 0;
    return NULL;
  }
  if (strcmp(key, "slug") == 0) {
    /* /^[a-z0-9]+(?:-[a-z0-9]+)*$/ and length <= 64 */
    bool ok = raw_len > 0 && raw_len <= 64;
    if (ok) {
      /* split on '-' : no empty segments, no leading/trailing '-' */
      if (raw[0] == '-' || raw[raw_len - 1] == '-') ok = false;
      for (size_t i = 0; i < raw_len && ok; i++) {
        char c = raw[i];
        if (c == '-') {
          if (i + 1 < raw_len && raw[i + 1] == '-') ok = false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
          ok = false;
        }
      }
    }
    if (!ok) return arena_strdup(a, "key \"slug\": not a slug");
    out->val = arena_strdup_n(a, raw, raw_len);
    out->kind = 0;
    return NULL;
  }
  {
    char *e = (char *)arena_alloc(a, 32 + strlen(key));
    snprintf(e, 32 + strlen(key), "unknown key \"%s\"", key);
    return e;
  }
}

static const char *KEYS[] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};

static bool is_known_key(const char *k, size_t n) {
  for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
    if (strlen(KEYS[i]) == n && memcmp(KEYS[i], k, n) == 0) return true;
  return false;
}

/* Parse front matter of a source (text already \n-normalised).
   Returns FrontMatter with .error set (arena string) when refused. */
static FrontMatter parse_front_matter(Arena *a, const char *text, size_t len) {
  FrontMatter fm;
  memset(&fm, 0, sizeof(fm));
  Lines lines = split_lines(a, text, len);
  if (lines.n == 0 || !line_eq(lines.lines[0].ptr, lines.lines[0].len, "---")) {
    fm.error = arena_strdup(a, "no front matter");
    return fm;
  }
  long close = -1;
  for (size_t i = 1; i < lines.n; i++) {
    if (line_eq(lines.lines[i].ptr, lines.lines[i].len, "---")) {
      close = (long)i;
      break;
    }
  }
  if (close == -1) {
    fm.error = arena_strdup(a, "the front matter is not closed");
    return fm;
  }
  size_t cap = 8;
  fm.pairs = (FmPair *)arena_alloc(a, cap * sizeof(FmPair));
  fm.n = 0;
  for (long i = 1; i < close; i++) {
    const char *line = lines.lines[i].ptr;
    size_t llen = lines.lines[i].len;
    if (line_is_blank(line, llen)) continue;
    if (llen > 0 && line[0] == '#') continue;
    /* LINE = /^([a-z_]+):(?: (.*))?$/ */
    size_t k = 0;
    while (k < llen && ((line[k] >= 'a' && line[k] <= 'z') || line[k] == '_')) k++;
    if (k == 0 || k >= llen || line[k] != ':') {
      char *e = (char *)arena_alloc(a, 48);
      snprintf(e, 48, "line %ld is not \"key: value\"", i + 1);
      fm.error = e;
      return fm;
    }
    const char *key = arena_strdup_n(a, line, k);
    const char *raw;
    size_t raw_len;
    if (k + 1 < llen && line[k + 1] == ' ') {
      raw = line + k + 2;
      raw_len = llen - (k + 2);
    } else if (k + 1 == llen) {
      raw = line + k + 1;
      raw_len = 0;
    } else {
      /* colon not followed by space or end: the regex fails */
      char *e = (char *)arena_alloc(a, 48);
      snprintf(e, 48, "line %ld is not \"key: value\"", i + 1);
      fm.error = e;
      return fm;
    }
    /* trailing spaces and tabs dropped */
    while (raw_len > 0 && (raw[raw_len - 1] == ' ' || raw[raw_len - 1] == '\t')) raw_len--;
    if (!is_known_key(key, k)) {
      char *e = (char *)arena_alloc(a, 32 + k);
      snprintf(e, 32 + k, "unknown key \"%s\"", key);
      fm.error = e;
      return fm;
    }
    for (size_t j = 0; j < fm.n; j++) {
      if (strcmp(fm.pairs[j].key, key) == 0) {
        char *e = (char *)arena_alloc(a, 40 + k);
        snprintf(e, 40 + k, "key \"%s\" is given twice", key);
        fm.error = e;
        return fm;
      }
    }
    if (raw_len == 0) {
      char *e = (char *)arena_alloc(a, 40 + k);
      snprintf(e, 40 + k, "key \"%s\" has no value", key);
      fm.error = e;
      return fm;
    }
    if (fm.n == cap) {
      cap *= 2;
      FmPair *np = (FmPair *)arena_alloc(a, cap * sizeof(FmPair));
      memcpy(np, fm.pairs, fm.n * sizeof(FmPair));
      fm.pairs = np;
    }
    FmPair *out = &fm.pairs[fm.n];
    memset(out, 0, sizeof(*out));
    out->key = key;
    const char *err = fm_value(a, key, raw, raw_len, out);
    if (err) {
      fm.error = err;
      return fm;
    }
    fm.n++;
  }
  bool has_title = false;
  for (size_t j = 0; j < fm.n; j++)
    if (strcmp(fm.pairs[j].key, "title") == 0) has_title = true;
  if (!has_title) {
    fm.error = arena_strdup(a, "no title");
    return fm;
  }
  /* body = lines after close, joined with '\n' */
  StrBuf b;
  sb_init(&b);
  for (long i = close + 1; i < (long)lines.n; i++) {
    if (i > close + 1) sb_addc(&b, '\n');
    sb_addn(&b, lines.lines[i].ptr, lines.lines[i].len);
  }
  fm.body = arena_strdup_n(a, b.buf, b.len);
  fm.body_len = b.len;
  sb_free(&b);
  return fm;
}

static const FmPair *fm_get(const FrontMatter *fm, const char *key) {
  for (size_t i = 0; i < fm->n; i++)
    if (strcmp(fm->pairs[i].key, key) == 0) return &fm->pairs[i];
  return NULL;
}

#endif
