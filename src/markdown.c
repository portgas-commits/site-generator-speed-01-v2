/* makefun-site's Markdown (lib/markdown.js): block lexer + inline renderer,
   ported exactly, including its quirks (code-span close is a plain indexOf,
   link target is the first ')' after '(', a backslash escape skips two
   characters in the find helpers). */
#ifndef MARKDOWN_C
#define MARKDOWN_C

#include "common.h"

/* Forward: the link resolver provided by the build (env.link). */
typedef const char *(*LinkFn)(void *ctx, const char *target, size_t tlen, size_t *out_len);

typedef struct {
  LinkFn link;
  void *link_ctx;
  Arena *a;
} MdEnv;

/* ---------------------------------------------------------------- blocks */

typedef enum { B_HEADING, B_CODE, B_HR, B_QUOTE, B_LIST, B_PARA } BlockType;

typedef struct {
  BlockType type;
  int depth;          /* heading */
  const char *text;   /* heading text (trimmed), or paragraph text */
  size_t text_len;
  const char *lang;   /* code */
  Line *lines;        /* code body lines / blockquote lines (trimmed, non-empty) */
  size_t nlines;
  bool ordered;       /* list */
  long start;         /* list */
  const char **items; /* list items (joined text) */
  size_t *item_lens;
  size_t nitems;
} Block;

typedef struct {
  Block *blocks;
  size_t n, cap;
} BlockList;

static void bl_push(Arena *a, BlockList *l, Block *b) {
  if (l->n == l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 16;
    Block *nb = (Block *)arena_alloc(a, ncap * sizeof(Block));
    if (l->blocks) memcpy(nb, l->blocks, l->n * sizeof(Block));
    l->blocks = nb;
    l->cap = ncap;
  }
  l->blocks[l->n++] = *b;
}

static bool is_blank_line(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != ' ' && p[i] != '\t') return false;
  return true;
}

/* trim spaces/tabs at both ends; returns arena string */
static const char *trim_line(Arena *a, const char *p, size_t n, size_t *out_len) {
  while (n > 0 && (p[0] == ' ' || p[0] == '\t')) {
    p++;
    n--;
  }
  while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
  *out_len = n;
  return arena_strdup_n(a, p, n);
}

static bool is_fence_open(const char *p, size_t n, const char **lang, size_t *lang_len) {
  if (n < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`') return false;
  size_t i = 3;
  while (i < n) {
    char c = p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-')) break;
    i++;
  }
  size_t j = i;
  while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
  if (j != n) return false;
  *lang = p + 3;
  *lang_len = i - 3;
  return true;
}

static bool is_fence_close(const char *p, size_t n) {
  if (n < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`') return false;
  for (size_t i = 3; i < n; i++)
    if (p[i] != ' ' && p[i] != '\t') return false;
  return true;
}

/* Heading: 1-6 '#', 1+ space/tab, text (trailing spaces/tabs dropped).
   Returns depth or 0; text out (arena). */
static int heading_of(Arena *a, const char *p, size_t n, const char **text, size_t *text_len) {
  size_t h = 0;
  while (h < n && h < 6 && p[h] == '#') h++;
  if (h == 0 || h > 6) return 0;
  if (h >= n || (p[h] != ' ' && p[h] != '\t')) return 0;
  size_t i = h;
  while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
  size_t e = n;
  while (e > i && (p[e - 1] == ' ' || p[e - 1] == '\t')) e--;
  if (e == i) return 0;
  *text = arena_strdup_n(a, p + i, e - i);
  *text_len = e - i;
  return (int)h;
}

static bool is_hr(const char *p, size_t n) {
  if (n < 3) return false;
  bool dash = p[0] == '-' && p[1] == '-' && p[2] == '-';
  bool star = p[0] == '*' && p[1] == '*' && p[2] == '*';
  if (!dash && !star) return false;
  for (size_t i = 3; i < n; i++)
    if (p[i] != ' ' && p[i] != '\t') return false;
  return true;
}

static bool ends_paragraph(Arena *a, const char *p, size_t n) {
  if (is_blank_line(p, n)) return true;
  const char *l;
  size_t ll;
  if (is_fence_open(p, n, &l, &ll)) return true;
  const char *t;
  size_t tl;
  if (heading_of(a, p, n, &t, &tl) != 0) return true;
  if (is_hr(p, n)) return true;
  return false;
}

/* Lex src into blocks. src is the body text (already \n-normalised). */
static BlockList lex_markdown(Arena *a, const char *src, size_t len) {
  Lines lines = split_lines(a, src, len);
  BlockList out;
  memset(&out, 0, sizeof(out));
  size_t i = 0;
  while (i < lines.n) {
    const char *line = lines.lines[i].ptr;
    size_t llen = lines.lines[i].len;
    if (is_blank_line(line, llen)) {
      i++;
      continue;
    }
    const char *lang;
    size_t lang_len;
    if (is_fence_open(line, llen, &lang, &lang_len)) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.type = B_CODE;
      b.lang = arena_strdup_n(a, lang, lang_len);
      i++;
      size_t cap = 8, n = 0;
      Line *body = (Line *)arena_alloc(a, cap * sizeof(Line));
      while (i < lines.n && !is_fence_close(lines.lines[i].ptr, lines.lines[i].len)) {
        if (n == cap) {
          cap *= 2;
          Line *nb = (Line *)arena_alloc(a, cap * sizeof(Line));
          memcpy(nb, body, n * sizeof(Line));
          body = nb;
        }
        body[n++] = lines.lines[i++];
      }
      i++; /* skip the closing fence, or run past the end */
      b.lines = body;
      b.nlines = n;
      bl_push(a, &out, &b);
      continue;
    }
    const char *htext;
    size_t hlen;
    int depth = heading_of(a, line, llen, &htext, &hlen);
    if (depth) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.type = B_HEADING;
      b.depth = depth;
      b.text = htext;
      b.text_len = hlen;
      bl_push(a, &out, &b);
      i++;
      continue;
    }
    if (is_hr(line, llen)) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.type = B_HR;
      bl_push(a, &out, &b);
      i++;
      continue;
    }
    if (llen > 0 && line[0] == '>') {
      size_t cap = 8, n = 0;
      Line *body = (Line *)arena_alloc(a, cap * sizeof(Line));
      while (i < lines.n && lines.lines[i].len > 0 && lines.lines[i].ptr[0] == '>') {
        const char *l = lines.lines[i].ptr;
        size_t ln = lines.lines[i].len;
        l++;
        ln--;
        if (ln > 0 && l[0] == ' ') {
          l++;
          ln--;
        }
        if (n == cap) {
          cap *= 2;
          Line *nb = (Line *)arena_alloc(a, cap * sizeof(Line));
          memcpy(nb, body, n * sizeof(Line));
          body = nb;
        }
        body[n].ptr = l;
        body[n].len = ln;
        n++;
        i++;
      }
      /* trim each, drop empties */
      size_t kept = 0;
      for (size_t k = 0; k < n; k++) {
        size_t tl;
        const char *t = trim_line(a, body[k].ptr, body[k].len, &tl);
        if (tl == 0) continue;
        body[kept].ptr = t;
        body[kept].len = tl;
        kept++;
      }
      Block b;
      memset(&b, 0, sizeof(b));
      b.type = B_QUOTE;
      b.lines = body;
      b.nlines = kept;
      bl_push(a, &out, &b);
      continue;
    }
    /* list? bullet: /^[-*] (.*)$/ ; ordered: /^(\d{1,9})\. (.*)$/ */
    bool is_bullet = llen >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ';
    bool is_ordered = false;
    long ord_start = 0;
    size_t ord_digits = 0;
    if (!is_bullet && llen >= 3) {
      size_t d = 0;
      while (d < llen && d < 9 && line[d] >= '0' && line[d] <= '9') d++;
      if (d >= 1 && d < llen && line[d] == '.' && d + 1 < llen && line[d + 1] == ' ') {
        is_ordered = true;
        ord_digits = d;
        ord_start = 0;
        for (size_t k = 0; k < d; k++) ord_start = ord_start * 10 + (line[k] - '0');
      }
    }
    if (is_bullet || is_ordered) {
      size_t cap = 8, n = 0;
      const char **items = (const char **)arena_alloc(a, cap * sizeof(char *));
      size_t *ilens = (size_t *)arena_alloc(a, cap * sizeof(size_t));
      while (i < lines.n) {
        const char *l = lines.lines[i].ptr;
        size_t ln = lines.lines[i].len;
        bool match = false;
        const char *item = NULL;
        size_t item_len = 0;
        if (is_ordered) {
          size_t d = 0;
          while (d < ln && d < 9 && l[d] >= '0' && l[d] <= '9') d++;
          if (d >= 1 && d < ln && l[d] == '.' && d + 1 < ln && l[d + 1] == ' ') {
            match = true;
            item = l + d + 2;
            item_len = ln - (d + 2);
          }
        } else {
          /* BULLET is /^[-*] (.*)$/ : either marker continues the list. */
          if (ln >= 2 && (l[0] == '-' || l[0] == '*') && l[1] == ' ') {
            match = true;
            item = l + 2;
            item_len = ln - 2;
          }
        }
        if (match) {
          if (n == cap) {
            cap *= 2;
            const char **ni = (const char **)arena_alloc(a, cap * sizeof(char *));
            size_t *nl = (size_t *)arena_alloc(a, cap * sizeof(size_t));
            memcpy(ni, items, n * sizeof(char *));
            memcpy(nl, ilens, n * sizeof(size_t));
            items = ni;
            ilens = nl;
          }
          /* items hold parts; a continuation appends "\n" + trimmed line */
          size_t tl;
          const char *t = trim_line(a, item, item_len, &tl);
          items[n] = t;
          ilens[n] = tl;
          n++;
          i++;
        } else {
          /* CONTINUED = /^ {2,}\S/ : two or more spaces, then a non-space. */
          size_t sp = 0;
          while (sp < ln && l[sp] == ' ') sp++;
          bool cont = false;
          if (sp >= 2 && sp < ln) {
            size_t adv = 0;
            uint32_t cp = utf8_decode(l + sp, ln - sp, &adv);
            cont = !is_js_space_cp(cp);
          }
          if (!cont) break;
          {
            size_t tl;
            const char *t = trim_line(a, l, ln, &tl);
            /* append to the last item with a "\n" between */
            StrBuf sb;
            sb_init(&sb);
            sb_addn(&sb, items[n - 1], ilens[n - 1]);
            sb_addc(&sb, '\n');
            sb_addn(&sb, t, tl);
            items[n - 1] = arena_strdup_n(a, sb.buf, sb.len);
            ilens[n - 1] = sb.len;
            sb_free(&sb);
            i++;
          }
        }
      }
      Block b;
      memset(&b, 0, sizeof(b));
      b.type = B_LIST;
      b.ordered = is_ordered;
      b.start = is_ordered ? ord_start : 1;
      b.items = items;
      b.item_lens = ilens;
      b.nitems = n;
      (void)ord_digits;
      bl_push(a, &out, &b);
      continue;
    }
    /* paragraph */
    {
      StrBuf sb;
      sb_init(&sb);
      size_t tl;
      const char *t = trim_line(a, line, llen, &tl);
      sb_addn(&sb, t, tl);
      i++;
      while (i < lines.n && !ends_paragraph(a, lines.lines[i].ptr, lines.lines[i].len)) {
        sb_addc(&sb, '\n');
        const char *t2 = trim_line(a, lines.lines[i].ptr, lines.lines[i].len, &tl);
        sb_addn(&sb, t2, tl);
        i++;
      }
      Block b;
      memset(&b, 0, sizeof(b));
      b.type = B_PARA;
      b.text = arena_strdup_n(a, sb.buf, sb.len);
      b.text_len = sb.len;
      sb_free(&sb);
      bl_push(a, &out, &b);
      continue;
    }
  }
  return out;
}

/* ---------------------------------------------------------------- inline */

static const char ESCAPABLE_CHARS[] = "\\`*_[]()#+-.!>";

static bool is_escapable(char c) {
  return strchr(ESCAPABLE_CHARS, c) != NULL;
}

/* First index >= from where s holds `what` outside a backslash escape, or -1. */
static long find_unescaped(const char *s, size_t n, const char *what, size_t wlen, size_t from) {
  for (size_t j = from; j + wlen <= n; j++) {
    if (s[j] == '\\' && j + 1 < n && is_escapable(s[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s + j, what, wlen) == 0) return (long)j;
  }
  return -1;
}

/* First single * from `from` (one whose run is not **), outside an escape, or -1. */
static long find_single_star(const char *s, size_t n, size_t from) {
  for (size_t j = from; j < n; j++) {
    char c = s[j];
    if (c == '\\' && j + 1 < n && is_escapable(s[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < n && s[j + 1] == '*') {
      j++;
      continue;
    }
    return (long)j;
  }
  return -1;
}

static void md_inline_into(StrBuf *out, const char *s, size_t n, MdEnv *env);

/* Render inline markup of s[0..n) into out. */
static void md_inline_into(StrBuf *out, const char *s, size_t n, MdEnv *env) {
  size_t i = 0;
  while (i < n) {
    char c = s[i];
    if (c == '\\' && i + 1 < n && is_escapable(s[i + 1])) {
      escape_html_into(out, s + i + 1, 1);
      i += 2;
      continue;
    }
    if (c == '`') {
      /* plain indexOf for the closer (an escaped backtick still closes) */
      long j = -1;
      for (size_t k = i + 1; k < n; k++)
        if (s[k] == '`') {
          j = (long)k;
          break;
        }
      if (j > (long)i + 1) {
        sb_addn(out, "<code>", 6);
        escape_html_into(out, s + i + 1, (size_t)(j - (long)i - 1));
        sb_addn(out, "</code>", 7);
        i = (size_t)j + 1;
      } else {
        sb_addc(out, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < n && s[i + 1] == '*') {
        long j = find_unescaped(s, n, "**", 2, i + 2);
        if (j > (long)i + 2) {
          sb_addn(out, "<strong>", 8);
          md_inline_into(out, s + i + 2, (size_t)(j - (long)i - 2), env);
          sb_addn(out, "</strong>", 9);
          i = (size_t)j + 2;
        } else {
          sb_addn(out, "**", 2);
          i += 2;
        }
        continue;
      }
      long j = find_single_star(s, n, i + 1);
      if (j > (long)i + 1) {
        sb_addn(out, "<em>", 4);
        md_inline_into(out, s + i + 1, (size_t)(j - (long)i - 1), env);
        sb_addn(out, "</em>", 5);
        i = (size_t)j + 1;
      } else {
        sb_addc(out, '*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, n, "]", 1, i + 1);
      long m = -1;
      const char *target = NULL;
      size_t target_len = 0;
      if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
        for (size_t q = (size_t)k + 2; q < n; q++)
          if (s[q] == ')') {
            m = (long)q;
            break;
          }
        if (m != -1) {
          target = s + k + 2;
          target_len = (size_t)(m - k - 2);
        }
      }
      bool ok = false;
      if (target != NULL && target_len > 0) {
        ok = true;
        /* target must hold no whitespace (JS \s) */
        size_t q = 0;
        while (q < target_len) {
          size_t adv = 0;
          uint32_t cp = utf8_decode(target + q, target_len - q, &adv);
          if (is_js_space_cp(cp)) {
            ok = false;
            break;
          }
          q += adv;
        }
      }
      if (ok) {
        size_t href_len = 0;
        const char *href = env->link(env->link_ctx, target, target_len, &href_len);
        sb_addn(out, "<a href=\"", 9);
        escape_html_into(out, href, href_len);
        sb_addn(out, "\">", 2);
        md_inline_into(out, s + i + 1, (size_t)(k - (long)i - 1), env);
        sb_addn(out, "</a>", 4);
        i = (size_t)m + 1;
        continue;
      }
      sb_addc(out, '[');
      i++;
      continue;
    }
    /* Plain run: everything up to the next markdown-special character,
       html-escaped in a single call (the old code called escape_html_into
       one byte at a time here). */
    size_t j = i;
    while (j < n) {
      char d = s[j];
      if (d == '\\' || d == '`' || d == '*' || d == '[') break;
      j++;
    }
    if (j == i) {
      /* a lone backslash (the next char is not escapable): literal */
      sb_addc(out, '\\');
      i++;
    } else {
      escape_html_into(out, s + i, j - i);
      i = j;
    }
  }
}

#endif
