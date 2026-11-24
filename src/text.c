/* makefun-site's text rules (lib/text.js): HTML escaping, the slug rule
   (NFKD, combining marks dropped, A-Z lowered, runs of other characters to
   one "-", at most 64 chars, "untitled" when empty), unique slugs, long dates.
   The NFKD table (nfkd_table.h) holds every code point whose NFKD is ASCII
   letters/digits and/or combining marks -- the only decompositions that can
   change a slug. Anything else with no table entry either has no NFKD
   decomposition or decomposes to nothing ASCII, so slugify drops it either
   way. */
#ifndef TEXT_C
#define TEXT_C

#include "common.h"
#include "nfkd_table.h"

/* Binary search the NFKD table; NULL when the code point has no entry. */
static const char *nfkd_lookup(uint32_t cp) {
  size_t lo = 0, hi = NFKD_TABLE_LEN;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (NFKD_TABLE[mid].cp < cp)
      lo = mid + 1;
    else if (NFKD_TABLE[mid].cp > cp)
      hi = mid;
    else
      return NFKD_TABLE[mid].rep;
  }
  return NULL;
}

/* The slug of `text` (UTF-8, NUL-terminated), written into the arena.
   Fast path: pure ASCII input never touches the table. */
static char *slugify(Arena *a, const char *text) {
  /* Fast path: all ASCII? */
  bool ascii = true;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    if (*p >= 0x80) {
      ascii = false;
      break;
    }
  StrBuf b;
  sb_init(&b);
  bool gap = false;
  if (ascii) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
      unsigned char c = *p;
      char keep = 0;
      if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = (char)c;
      if (!keep) {
        if (b.len > 0) gap = true;
        continue;
      }
      if (gap) sb_addc(&b, '-');
      gap = false;
      sb_addc(&b, keep);
    }
  } else {
    size_t n = strlen(text);
    size_t i = 0;
    while (i < n) {
      size_t adv = 0;
      uint32_t cp = utf8_decode(text + i, n - i, &adv);
      const char *rep = nfkd_lookup(cp);
      if (rep) {
        /* replacement is ASCII letters/digits and combining marks */
        for (const unsigned char *r = (const unsigned char *)rep; *r; r++) {
          unsigned char c = *r;
          if (c >= 0x80) continue; /* a combining mark: dropped */
          char keep = 0;
          if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
          else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = (char)c;
          if (!keep) {
            if (b.len > 0) gap = true;
            continue;
          }
          if (gap) sb_addc(&b, '-');
          gap = false;
          sb_addc(&b, keep);
        }
      } else if (is_combining_mark(cp)) {
        /* dropped */
      } else if (cp >= 'A' && cp <= 'Z') {
        if (gap) sb_addc(&b, '-');
        gap = false;
        sb_addc(&b, (char)(cp + 32));
      } else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
        if (gap) sb_addc(&b, '-');
        gap = false;
        sb_addc(&b, (char)cp);
      } else {
        if (b.len > 0) gap = true;
      }
      i += adv;
    }
  }
  /* cut to 64 chars, a "-" left at the cut dropped */
  if (b.len > 64) {
    b.len = 64;
    while (b.len > 0 && b.buf[b.len - 1] == '-') b.len--;
    b.buf[b.len] = 0;
  }
  char *out;
  if (b.len == 0) {
    out = arena_strdup(a, "untitled");
  } else {
    out = arena_strdup_n(a, b.buf, b.len);
  }
  sb_free(&b);
  return out;
}

/* `slug` made unique against the taken set (a StrMap used as a set, values
   are (void*)1) and added to it. */
static char *unique_slug(Arena *a, const char *slug, StrMap *taken) {
  if (!smap_has(taken, slug)) {
    smap_put(taken, arena_strdup(a, slug), (void *)1);
    return arena_strdup(a, slug);
  }
  StrBuf b;
  sb_init(&b);
  for (int n = 2;; n++) {
    b.len = 0;
    sb_add(&b, slug);
    char tmp[16];
    int m = snprintf(tmp, sizeof(tmp), "-%d", n);
    sb_addn(&b, tmp, (size_t)m);
    if (!smap_has(taken, b.buf)) {
      smap_put(taken, arena_strdup(a, b.buf), (void *)1);
      char *out = arena_strdup_n(a, b.buf, b.len);
      sb_free(&b);
      return out;
    }
  }
}

/* HTML escaping: & < > " ' only. Appends to b. Fast path copies runs of
   safe bytes with memchr-free scanning. */
static void escape_html_into(StrBuf *b, const char *s, size_t n) {
  static const char *rep[256] = {0};
  /* one-time table init via a static flag */
  static bool init = false;
  if (!init) {
    init = true;
    /* filled lazily below through a switch instead; table unused */
    (void)rep;
  }
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    const char *r = NULL;
    switch (c) {
      case '&': r = "&amp;"; break;
      case '<': r = "&lt;"; break;
      case '>': r = "&gt;"; break;
      case '"': r = "&quot;"; break;
      case '\'': r = "&#39;"; break;
      default: break;
    }
    if (r) {
      sb_add(b, r);
      i++;
      continue;
    }
    size_t j = i;
    while (j < n) {
      unsigned char d = (unsigned char)s[j];
      if (d == '&' || d == '<' || d == '>' || d == '"' || d == '\'') break;
      j++;
    }
    sb_addn(b, s + i, j - i);
    i = j;
  }
}

/* A YYYY-MM-DD date in words, from the string alone. `date` is "YYYY-MM-DD". */
static void long_date_into(StrBuf *b, const char *date) {
  static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December"};
  int y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
  int m = (date[5] - '0') * 10 + (date[6] - '0');
  int d = (date[8] - '0') * 10 + (date[9] - '0');
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%d", d);
  sb_addn(b, tmp, (size_t)n);
  sb_addc(b, ' ');
  sb_add(b, MONTHS[m - 1]);
  sb_addc(b, ' ');
  n = snprintf(tmp, sizeof(tmp), "%d", y);
  sb_addn(b, tmp, (size_t)n);
}

#endif
