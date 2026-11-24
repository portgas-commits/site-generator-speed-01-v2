/* The build pipeline (vendor/makefun-site/index.js), ported exactly, plus
   the N-API binding. One build() call = one case, fully independent: fresh
   arena, fresh maps, nothing carried to the next case. */
#ifndef BUILD_C
#define BUILD_C

#include "common.h"
#include "json.h"
#include "text.c"
#include "frontmatter.c"
#include "markdown.c"
#include "template.c"

/* ---------------------------------------------------------------- model */

typedef struct Source Source;
typedef struct Tag Tag;

typedef struct {
  int level;
  const char *id;
  const char *html; /* arena */
} TocEntry;

struct Source {
  const char *path;
  const char *text; /* \n-normalised */
  FrontMatter fm;
  bool post;
  const char *layout;
  const char *slug;
  const char *out_dir;
  const char *url;
  const char *html;
  TocEntry *toc;
  size_t ntoc;
  const char *excerpt;
  Tag **tag_refs;
  size_t ntag_refs;
  Source *newer, *older;
  JVal *card_cache; /* memoised card: a post is listed on its index page,
                       its tag pages and the recent list, so the same card
                       is built many times per case; it is read-only once
                       built, so sharing it is safe */
};

struct Tag {
  const char *name;
  const char *slug;
  const char *url;
  Source **posts;
  size_t nposts, capposts;
};

typedef struct {
  const char *path;
  const char *content;
} StaticFile;

typedef struct {
  Arena *a;
  const JVal *input;
  const char *base;
  StrMap templates; /* name -> Template* */
  const char **report;
  size_t nreport, capreport;
  Source **sources;
  size_t nsources, capsources;
  StaticFile *statics;
  size_t nstatics, capstatics;
  StrMap by_source; /* path -> Source* */
  StrMap broken;    /* "broken-link ..." -> (void*)1 */
  Source **posts;
  size_t nposts;
  Tag **tags;
  size_t ntags;
  /* output: parallel arrays + map for the shadowed check */
  const char **out_paths;
  const char **out_texts;
  size_t nout, capout;
  StrMap out_map; /* path -> (void*)1 */
} Build;

static void report_push(Build *b, const char *line) {
  if (b->nreport == b->capreport) {
    size_t ncap = b->capreport ? b->capreport * 2 : 32;
    const char **nr = (const char **)malloc(ncap * sizeof(char *));
    if (b->report) memcpy(nr, b->report, b->nreport * sizeof(char *));
    free(b->report);
    b->report = nr;
    b->capreport = ncap;
  }
  b->report[b->nreport++] = line;
}

static void out_put(Build *b, const char *path, const char *text) {
  if (b->nout == b->capout) {
    size_t ncap = b->capout ? b->capout * 2 : 64;
    b->out_paths = (const char **)realloc(b->out_paths, ncap * sizeof(char *));
    b->out_texts = (const char **)realloc(b->out_texts, ncap * sizeof(char *));
    b->capout = ncap;
  }
  b->out_paths[b->nout] = path;
  b->out_texts[b->nout] = text;
  b->nout++;
  smap_put(&b->out_map, path, (void *)1);
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static const char *dir_of(Arena *a, const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) return arena_strdup(a, "");
  return arena_strdup_n(a, path, (size_t)(slash - path));
}

/* resolvePath(dir, rel): "" and "." steps skipped, ".." pops, null when it
   climbs out of the root. Returns arena string or NULL. */
static const char *resolve_path(Arena *a, const char *dir, const char *rel) {
  /* split dir into segments */
  const char **parts = NULL;
  size_t n = 0, cap = 0;
  if (dir[0]) {
    const char *p = dir;
    for (;;) {
      const char *s = strchr(p, '/');
      size_t len = s ? (size_t)(s - p) : strlen(p);
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        parts = (const char **)realloc(parts, cap * sizeof(char *));
      }
      parts[n++] = arena_strdup_n(a, p, len);
      if (!s) break;
      p = s + 1;
    }
  }
  const char *p = rel;
  for (;;) {
    const char *s = strchr(p, '/');
    size_t len = s ? (size_t)(s - p) : strlen(p);
    if (len == 0 || (len == 1 && p[0] == '.')) {
      /* skip */
    } else if (len == 2 && p[0] == '.' && p[1] == '.') {
      if (n == 0) {
        free(parts);
        return NULL;
      }
      n--;
    } else {
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        parts = (const char **)realloc(parts, cap * sizeof(char *));
      }
      parts[n++] = arena_strdup_n(a, p, len);
    }
    if (!s) break;
    p = s + 1;
  }
  StrBuf b;
  sb_init(&b);
  for (size_t i = 0; i < n; i++) {
    if (i) sb_addc(&b, '/');
    sb_add(&b, parts[i]);
  }
  free(parts);
  char *out = arena_strdup_n(a, b.buf, b.len);
  sb_free(&b);
  return out;
}

/* ---------------------------------------------------------------- markdown render */

typedef struct {
  Build *b;
  Source *src;
} LinkCtx;

/* env.link: resolve a link target to its href, recording broken links. */
static const char *link_resolve(void *ctx, const char *target, size_t tlen, size_t *out_len) {
  LinkCtx *lc = (LinkCtx *)ctx;
  Build *b = lc->b;
  Arena *a = b->a;
  char *tgt = arena_strdup_n(a, target, tlen);
  /* scheme? /^[A-Za-z][A-Za-z0-9+.-]*:/ or starts with '/' or '#' */
  bool external = false;
  if (tgt[0] == '/' || tgt[0] == '#') {
    external = true;
  } else if ((tgt[0] >= 'A' && tgt[0] <= 'Z') || (tgt[0] >= 'a' && tgt[0] <= 'z')) {
    size_t i = 1;
    while ((tgt[i] >= 'A' && tgt[i] <= 'Z') || (tgt[i] >= 'a' && tgt[i] <= 'z') ||
           (tgt[i] >= '0' && tgt[i] <= '9') || tgt[i] == '+' || tgt[i] == '-' || tgt[i] == '.')
      i++;
    if (tgt[i] == ':') external = true;
  }
  if (external) {
    *out_len = tlen;
    return tgt;
  }
  const char *hash = strchr(tgt, '#');
  const char *rel = tgt;
  size_t rel_len = hash ? (size_t)(hash - tgt) : tlen;
  const char *frag = hash ? hash : "";
  char *rel_s = arena_strdup_n(a, rel, rel_len);
  const char *dir = dir_of(a, lc->src->path);
  const char *path = rel_len == 0 ? NULL : resolve_path(a, dir, rel_s);
  if (path) {
    Source *hit = (Source *)smap_get(&b->by_source, path);
    if (hit) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, hit->url);
      sb_add(&sb, frag);
      char *out = arena_strdup_n(a, sb.buf, sb.len);
      *out_len = sb.len;
      sb_free(&sb);
      return out;
    }
    /* static? */
    for (size_t i = 0; i < b->nstatics; i++) {
      if (strcmp(b->statics[i].path, path) == 0) {
        StrBuf sb;
        sb_init(&sb);
        sb_add(&sb, b->base);
        sb_addc(&sb, '/');
        sb_add(&sb, path);
        sb_add(&sb, frag);
        char *out = arena_strdup_n(a, sb.buf, sb.len);
        *out_len = sb.len;
        sb_free(&sb);
        return out;
      }
    }
  }
  StrBuf sb;
  sb_init(&sb);
  sb_add(&sb, "broken-link ");
  sb_add(&sb, lc->src->path);
  sb_add(&sb, " -> ");
  sb_addn(&sb, target, tlen);
  char *line = arena_strdup_n(a, sb.buf, sb.len);
  sb_free(&sb);
  smap_put(&b->broken, line, (void *)1);
  *out_len = tlen;
  return tgt;
}

/* Render one source's body to HTML + toc + excerpt. */
static void render_markdown(Build *b, Source *s) {
  Arena *a = b->a;
  BlockList blocks = lex_markdown(a, s->fm.body, s->fm.body_len);
  StrMap ids;
  smap_init(&ids);
  StrBuf html;
  sb_init(&html);
  TocEntry *toc = NULL;
  size_t ntoc = 0, captoc = 0;
  const char *excerpt = NULL;
  LinkCtx lc = {b, s};
  MdEnv env = {link_resolve, &lc, a};
  for (size_t bi = 0; bi < blocks.n; bi++) {
    Block *t = &blocks.blocks[bi];
    switch (t->type) {
      case B_HEADING: {
        char *id = unique_slug(a, slugify(a, t->text), &ids);
        StrBuf hb;
        sb_init(&hb);
        md_inline_into(&hb, t->text, t->text_len, &env);
        char tmp[16];
        int m = snprintf(tmp, sizeof(tmp), "<h%d id=\"", t->depth);
        sb_addn(&html, tmp, (size_t)m);
        sb_add(&html, id);
        sb_addn(&html, "\">", 2);
        sb_addn(&html, hb.buf, hb.len);
        m = snprintf(tmp, sizeof(tmp), "</h%d>", t->depth);
        sb_addn(&html, tmp, (size_t)m);
        sb_addc(&html, '\n');
        if (t->depth == 2 || t->depth == 3) {
          if (ntoc == captoc) {
            size_t ncap = captoc ? captoc * 2 : 8;
            TocEntry *nt = (TocEntry *)arena_alloc(a, ncap * sizeof(TocEntry));
            if (toc) memcpy(nt, toc, ntoc * sizeof(TocEntry));
            toc = nt;
            captoc = ncap;
          }
          toc[ntoc].level = t->depth;
          toc[ntoc].id = id;
          toc[ntoc].html = arena_strdup_n(a, hb.buf, hb.len);
          ntoc++;
        }
        sb_free(&hb);
        break;
      }
      case B_CODE: {
        sb_addn(&html, "<pre><code", 10);
        if (t->lang && t->lang[0]) {
          sb_addn(&html, " class=\"language-", 17);
          escape_html_into(&html, t->lang, strlen(t->lang));
          sb_addc(&html, '"');
        }
        sb_addc(&html, '>');
        for (size_t k = 0; k < t->nlines; k++) {
          escape_html_into(&html, t->lines[k].ptr, t->lines[k].len);
          sb_addc(&html, '\n');
        }
        sb_addn(&html, "</code></pre>\n", 14);
        break;
      }
      case B_HR:
        sb_addn(&html, "<hr>\n", 5);
        break;
      case B_QUOTE: {
        if (t->nlines == 0) {
          sb_addn(&html, "<blockquote>\n</blockquote>\n", 27);
        } else {
          StrBuf qb;
          sb_init(&qb);
          for (size_t k = 0; k < t->nlines; k++) {
            if (k) sb_addc(&qb, '\n');
            sb_addn(&qb, t->lines[k].ptr, t->lines[k].len);
          }
          sb_addn(&html, "<blockquote>\n<p>", 16);
          md_inline_into(&html, qb.buf, qb.len, &env);
          sb_addn(&html, "</p>\n</blockquote>\n", 19);
          sb_free(&qb);
        }
        break;
      }
      case B_LIST: {
        if (t->ordered) {
          if (t->start != 1) {
            char tmp[32];
            int m = snprintf(tmp, sizeof(tmp), "<ol start=\"%ld\">", t->start);
            sb_addn(&html, tmp, (size_t)m);
          } else {
            sb_addn(&html, "<ol>", 4);
          }
        } else {
          sb_addn(&html, "<ul>", 4);
        }
        sb_addc(&html, '\n');
        for (size_t k = 0; k < t->nitems; k++) {
          sb_addn(&html, "<li>", 4);
          md_inline_into(&html, t->items[k], t->item_lens[k], &env);
          sb_addn(&html, "</li>\n", 6);
        }
        sb_addn(&html, t->ordered ? "</ol>\n" : "</ul>\n", t->ordered ? 6 : 6);
        break;
      }
      case B_PARA: {
        StrBuf pb;
        sb_init(&pb);
        md_inline_into(&pb, t->text, t->text_len, &env);
        if (!excerpt) excerpt = arena_strdup_n(a, pb.buf, pb.len);
        sb_addn(&html, "<p>", 3);
        sb_addn(&html, pb.buf, pb.len);
        sb_addn(&html, "</p>\n", 5);
        sb_free(&pb);
        break;
      }
    }
  }
  smap_free(&ids);
  s->html = arena_strdup_n(a, html.buf, html.len);
  sb_free(&html);
  s->toc = toc;
  s->ntoc = ntoc;
  s->excerpt = excerpt ? excerpt : arena_strdup(a, "");
}

/* ---------------------------------------------------------------- cards */

/* wordsOf: runs of non-whitespace in text */
static long words_of(const char *s, size_t n) {
  long count = 0;
  bool in_word = false;
  size_t i = 0;
  while (i < n) {
    size_t adv = 0;
    uint32_t cp = utf8_decode(s + i, n - i, &adv);
    if (is_js_space_cp(cp)) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      count++;
    }
    i += adv;
  }
  return count;
}

static JVal *jstr_val(Arena *a, const char *s) {
  JVal *v = jnew(a, J_STR);
  v->str = arena_strdup(a, s ? s : "");
  v->len = strlen(v->str);
  return v;
}

static JVal *jnum_val(Arena *a, double d) {
  JVal *v = jnew(a, J_NUM);
  v->num = d;
  return v;
}

static JVal *jbool_val(Arena *a, bool b) {
  JVal *v = jnew(a, J_BOOL);
  v->b = b;
  return v;
}

/* A post's card, as lists show it. Memoised per source: the same card is
   rendered on the source's index page, its tag pages and the recent list. */
static JVal *card_of(Build *b, Source *s) {
  if (s->card_cache) return s->card_cache;
  Arena *a = b->a;
  JVal *card = jnew(a, J_OBJ);
  const char *title = "";
  const FmPair *tp = fm_get(&s->fm, "title");
  if (tp) title = tp->val;
  jobj_put(a, card, "title", jstr_val(a, title));
  jobj_put(a, card, "url", jstr_val(a, s->url));
  const FmPair *dp = fm_get(&s->fm, "date");
  jobj_put(a, card, "date", jstr_val(a, dp ? dp->val : ""));
  if (dp) {
    StrBuf lb;
    sb_init(&lb);
    long_date_into(&lb, dp->val);
    JVal *v = jstr_val(a, lb.buf);
    sb_free(&lb);
    jobj_put(a, card, "date_long", v);
  } else {
    jobj_put(a, card, "date_long", jstr_val(a, ""));
  }
  const FmPair *sp = fm_get(&s->fm, "summary");
  jobj_put(a, card, "summary", jstr_val(a, sp ? sp->val : ""));
  jobj_put(a, card, "excerpt", jstr_val(a, s->excerpt));
  long w = words_of(s->fm.body, s->fm.body_len);
  long rm = (w + 199) / 200;
  if (rm < 1) rm = 1;
  jobj_put(a, card, "reading_minutes", jnum_val(a, (double)rm));
  JVal *tags = jnew(a, J_ARR);
  for (size_t i = 0; i < s->ntag_refs; i++) {
    Tag *t = s->tag_refs[i];
    JVal *o = jnew(a, J_OBJ);
    jobj_put(a, o, "name", jstr_val(a, t->name));
    jobj_put(a, o, "slug", jstr_val(a, t->slug));
    jobj_put(a, o, "url", jstr_val(a, t->url));
    jarr_push(a, tags, o);
  }
  jobj_put(a, card, "tags", tags);
  s->card_cache = card;
  return card;
}

static JVal *link_of(Build *b, Source *s) {
  Arena *a = b->a;
  if (!s) return jnew(a, J_NULL);
  JVal *o = jnew(a, J_OBJ);
  const FmPair *tp = fm_get(&s->fm, "title");
  jobj_put(a, o, "title", jstr_val(a, tp ? tp->val : ""));
  jobj_put(a, o, "url", jstr_val(a, s->url));
  const FmPair *dp = fm_get(&s->fm, "date");
  jobj_put(a, o, "date", jstr_val(a, dp ? dp->val : ""));
  if (dp) {
    StrBuf lb;
    sb_init(&lb);
    long_date_into(&lb, dp->val);
    JVal *v = jstr_val(a, lb.buf);
    sb_free(&lb);
    jobj_put(a, o, "date_long", v);
  } else {
    jobj_put(a, o, "date_long", jstr_val(a, ""));
  }
  return o;
}

/* A page as its layout reads it. */
static JVal *page_of(Build *b, Source *s) {
  Arena *a = b->a;
  JVal *page = card_of(b, s);
  jobj_put(a, page, "slug", jstr_val(a, s->slug));
  jobj_put(a, page, "content", jstr_val(a, s->html));
  JVal *toc = jnew(a, J_ARR);
  for (size_t i = 0; i < s->ntoc; i++) {
    JVal *o = jnew(a, J_OBJ);
    jobj_put(a, o, "level", jnum_val(a, (double)s->toc[i].level));
    jobj_put(a, o, "id", jstr_val(a, s->toc[i].id));
    jobj_put(a, o, "html", jstr_val(a, s->toc[i].html));
    jarr_push(a, toc, o);
  }
  jobj_put(a, page, "toc", toc);
  jobj_put(a, page, "newer", link_of(b, s->newer));
  jobj_put(a, page, "older", link_of(b, s->older));
  jobj_put(a, page, "is_post", jbool_val(a, s->post));
  return page;
}

static JVal *tag_of(Build *b, Tag *t) {
  Arena *a = b->a;
  JVal *o = jnew(a, J_OBJ);
  jobj_put(a, o, "name", jstr_val(a, t->name));
  jobj_put(a, o, "slug", jstr_val(a, t->slug));
  jobj_put(a, o, "url", jstr_val(a, t->url));
  jobj_put(a, o, "count", jnum_val(a, (double)t->nposts));
  JVal *posts = jnew(a, J_ARR);
  for (size_t i = 0; i < t->nposts; i++) jarr_push(a, posts, card_of(b, t->posts[i]));
  jobj_put(a, o, "posts", posts);
  return o;
}

/* ---------------------------------------------------------------- pipeline */

static int cmp_source_path(const void *a, const void *b) {
  const JVal *x = *(const JVal *const *)a;
  const JVal *y = *(const JVal *const *)b;
  return strcmp(jstr(jobj_get(x, "path")), jstr(jobj_get(y, "path")));
}

static int cmp_post_order(const void *a, const void *b) {
  Source *x = *(Source *const *)a;
  Source *y = *(Source *const *)b;
  const FmPair *dx = fm_get(&x->fm, "date");
  const FmPair *dy = fm_get(&y->fm, "date");
  int c = strcmp(dy->val, dx->val); /* newest first */
  if (c) return c;
  return strcmp(x->slug, y->slug);
}

static int cmp_tag_order(const void *a, const void *b) {
  Tag *x = *(Tag *const *)a;
  Tag *y = *(Tag *const *)b;
  if (x->nposts != y->nposts) return x->nposts < y->nposts ? 1 : -1;
  return strcmp(x->slug, y->slug);
}

static int cmp_report_line(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ownTags: a source's tags, each slug once, in its own order. Returns
   arena array of tag names (first spelling kept). */
static const char **own_tags(Build *b, Source *s, size_t *out_n) {
  Arena *a = b->a;
  const FmPair *tp = fm_get(&s->fm, "tags");
  if (!tp || tp->ntags == 0) {
    *out_n = 0;
    return NULL;
  }
  StrMap seen;
  smap_init(&seen);
  const char **names = (const char **)arena_alloc(a, tp->ntags * sizeof(char *));
  size_t n = 0;
  for (size_t i = 0; i < tp->ntags; i++) {
    char *slug = slugify(a, tp->tags[i]);
    if (smap_has(&seen, slug)) continue;
    smap_put(&seen, slug, (void *)1);
    names[n++] = tp->tags[i];
  }
  smap_free(&seen);
  *out_n = n;
  return names;
}

static void render_into(Build *b, const char *out_path, const char *tmpl_name, JVal *extra_vars,
                         JVal *site, JVal *all_tags, JVal *recent) {
  Template *t = (Template *)smap_get(&b->templates, tmpl_name);
  if (!t) return;
  Scope base_scope = {NULL, NULL};
  /* root vars object: {site, tags, recent} */
  JVal *base_vars = jnew(b->a, J_OBJ);
  jobj_put(b->a, base_vars, "site", site);
  jobj_put(b->a, base_vars, "tags", all_tags);
  jobj_put(b->a, base_vars, "recent", recent);
  Scope base = {base_vars, NULL};
  Scope inner = {extra_vars, &base};
  (void)base_scope;
  RenderCtx ctx = {&b->templates, b->a};
  StrBuf out;
  sb_init(&out);
  render_nodes(&out, t->nodes, t->n, &inner, &ctx, 0);
  out_put(b, out_path, arena_strdup_n(b->a, out.buf, out.len));
  sb_free(&out);
}

/* Build one case's input into its output tree (a JVal object). */
static JVal *build_case(Arena *a, const JVal *input) {
  Build b;
  memset(&b, 0, sizeof(b));
  b.a = a;
  b.input = input;
  smap_init(&b.templates);
  smap_init(&b.by_source);
  smap_init(&b.broken);
  smap_init(&b.out_map);

  const JVal *site_in = jobj_get(input, "site");
  const char *base_url = jstr(jobj_get(site_in, "base_url"));
  if (!base_url) base_url = "";
  /* base = base_url with trailing slashes dropped */
  size_t bl = strlen(base_url);
  while (bl > 0 && base_url[bl - 1] == '/') bl--;
  b.base = arena_strdup_n(a, base_url, bl);

  /* 2. Templates, parsed in code-unit order of name. */
  const JVal *templates_in = jobj_get(input, "templates");
  if (templates_in && templates_in->t == J_OBJ) {
    size_t nt = templates_in->n;
    const char **names = (const char **)malloc(nt * sizeof(char *));
    for (size_t i = 0; i < nt; i++) names[i] = templates_in->keys[i];
    qsort(names, nt, sizeof(char *), cmp_str);
    for (size_t i = 0; i < nt; i++) {
      const JVal *src = jobj_get(templates_in, names[i]);
      const char *text = jstr(src);
      if (!text) text = "";
      TokList toks;
      TplResult tr = tokenize(a, names[i], text, strlen(text), &toks);
      if (tr.failed) {
        report_push(&b, tr.msg);
        continue;
      }
      Template *tpl = (Template *)arena_alloc(a, sizeof(Template));
      TplResult pr = parse_template(a, names[i], &toks, tpl);
      if (pr.failed) {
        report_push(&b, pr.msg);
        continue;
      }
      smap_put(&b.templates, arena_strdup(a, names[i]), tpl);
    }
    free(names);
  }

  /* 1. Sources, sorted by path. */
  const JVal *files_in = jobj_get(input, "files");
  size_t nf = files_in && files_in->t == J_ARR ? files_in->n : 0;
  const JVal **sorted = (const JVal **)malloc((nf ? nf : 1) * sizeof(JVal *));
  for (size_t i = 0; i < nf; i++) sorted[i] = files_in->items[i];
  qsort(sorted, nf, sizeof(JVal *), cmp_source_path);
  for (size_t i = 0; i < nf; i++) {
    const char *path = jstr(jobj_get(sorted[i], "path"));
    const char *content = jstr(jobj_get(sorted[i], "content"));
    if (!path) path = "";
    if (!content) content = "";
    size_t plen = strlen(path);
    if (plen >= 3 && strcmp(path + plen - 3, ".md") == 0) {
      /* normalise \r\n and lone \r to \n */
      StrBuf sb;
      sb_init(&sb);
      for (size_t k = 0; content[k]; k++) {
        if (content[k] == '\r') {
          sb_addc(&sb, '\n');
          if (content[k + 1] == '\n') k++;
        } else {
          sb_addc(&sb, content[k]);
        }
      }
      Source *s = (Source *)arena_alloc(a, sizeof(Source));
      memset(s, 0, sizeof(*s));
      s->path = arena_strdup(a, path);
      s->text = arena_strdup_n(a, sb.buf, sb.len);
      sb_free(&sb);
      if (b.nsources == b.capsources) {
        size_t ncap = b.capsources ? b.capsources * 2 : 64;
        Source **ns = (Source **)malloc(ncap * sizeof(Source *));
        if (b.sources) memcpy(ns, b.sources, b.nsources * sizeof(Source *));
        free(b.sources);
        b.sources = ns;
        b.capsources = ncap;
      }
      b.sources[b.nsources++] = s;
    } else {
      if (b.nstatics == b.capstatics) {
        size_t ncap = b.capstatics ? b.capstatics * 2 : 16;
        StaticFile *ns = (StaticFile *)malloc(ncap * sizeof(StaticFile));
        if (b.statics) memcpy(ns, b.statics, b.nstatics * sizeof(StaticFile));
        free(b.statics);
        b.statics = ns;
        b.capstatics = ncap;
      }
      b.statics[b.nstatics].path = arena_strdup(a, path);
      b.statics[b.nstatics].content = arena_strdup(a, content);
      b.nstatics++;
    }
  }
  free(sorted);

  /* front matter + checks, in order */
  Source **kept = (Source **)malloc((b.nsources ? b.nsources : 1) * sizeof(Source *));
  size_t nkept = 0;
  static const char *RESERVED[] = {"posts", "tags", "page"};
  for (size_t i = 0; i < b.nsources; i++) {
    Source *s = b.sources[i];
    FrontMatter fm = parse_front_matter(a, s->text, strlen(s->text));
    if (fm.error) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, "refused ");
      sb_add(&sb, s->path);
      sb_add(&sb, ": ");
      sb_add(&sb, fm.error);
      report_push(&b, arena_strdup_n(a, sb.buf, sb.len));
      sb_free(&sb);
      continue;
    }
    s->fm = fm;
    s->post = strncmp(s->path, "posts/", 6) == 0;
    const char *own = NULL;
    for (size_t d = 0; d < 3; d++) {
      if (strcmp(RESERVED[d], "posts") == 0) continue;
      size_t dl = strlen(RESERVED[d]);
      if (strncmp(s->path, RESERVED[d], dl) == 0 && s->path[dl] == '/') {
        own = RESERVED[d];
        break;
      }
    }
    if (own) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, "refused ");
      sb_add(&sb, s->path);
      sb_add(&sb, ": ");
      sb_add(&sb, own);
      sb_add(&sb, "/ is the generator's own");
      report_push(&b, arena_strdup_n(a, sb.buf, sb.len));
      sb_free(&sb);
      continue;
    }
    if (s->post && !fm_get(&fm, "date")) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, "refused ");
      sb_add(&sb, s->path);
      sb_add(&sb, ": a post needs a date");
      report_push(&b, arena_strdup_n(a, sb.buf, sb.len));
      sb_free(&sb);
      continue;
    }
    const FmPair *lp = fm_get(&fm, "layout");
    const char *layout = lp ? lp->val : (s->post ? "post" : "page");
    if (!smap_has(&b.templates, layout)) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, "refused ");
      sb_add(&sb, s->path);
      sb_add(&sb, ": layout \"");
      sb_add(&sb, layout);
      sb_add(&sb, "\" is no template that parses");
      report_push(&b, arena_strdup_n(a, sb.buf, sb.len));
      sb_free(&sb);
      continue;
    }
    s->layout = arena_strdup(a, layout);
    const FmPair *dr = fm_get(&fm, "draft");
    if (dr && dr->kind == 3 && dr->b) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, "draft ");
      sb_add(&sb, s->path);
      report_push(&b, arena_strdup_n(a, sb.buf, sb.len));
      sb_free(&sb);
      continue;
    }
    kept[nkept++] = s;
  }
  b.nsources = nkept;
  /* sources array now holds only kept ones (kept is a copy; reuse it) */
  free(b.sources);
  b.sources = kept;
  b.capsources = nkept;

  /* permalinks */
  StrMap taken_by_dir;
  smap_init(&taken_by_dir);
  for (size_t i = 0; i < b.nsources; i++) {
    Source *s = b.sources[i];
    const char *dir = s->post ? "posts" : dir_of(a, s->path);
    StrMap *taken = (StrMap *)smap_get(&taken_by_dir, dir);
    if (!taken) {
      taken = (StrMap *)malloc(sizeof(StrMap));
      smap_init(taken);
      if (dir[0] == 0) {
        for (size_t d = 0; d < 3; d++) smap_put(taken, RESERVED[d], (void *)1);
      }
      smap_put(&taken_by_dir, arena_strdup(a, dir), taken);
    }
    const FmPair *sp = fm_get(&s->fm, "slug");
    const char *base_slug = sp ? sp->val : slugify(a, fm_get(&s->fm, "title")->val);
    s->slug = unique_slug(a, base_slug, taken);
    StrBuf sb;
    sb_init(&sb);
    if (dir[0]) {
      sb_add(&sb, dir);
      sb_addc(&sb, '/');
    }
    sb_add(&sb, s->slug);
    s->out_dir = arena_strdup_n(a, sb.buf, sb.len);
    sb_free(&sb);
    StrBuf ub;
    sb_init(&ub);
    sb_add(&ub, b.base);
    sb_addc(&ub, '/');
    sb_add(&ub, s->out_dir);
    sb_addc(&ub, '/');
    s->url = arena_strdup_n(a, ub.buf, ub.len);
    sb_free(&ub);
    smap_put(&b.by_source, s->path, s);
  }

  /* markdown */
  for (size_t i = 0; i < b.nsources; i++) render_markdown(&b, b.sources[i]);

  /* collections */
  b.posts = (Source **)malloc((b.nsources ? b.nsources : 1) * sizeof(Source *));
  b.nposts = 0;
  for (size_t i = 0; i < b.nsources; i++)
    if (b.sources[i]->post) b.posts[b.nposts++] = b.sources[i];
  qsort(b.posts, b.nposts, sizeof(Source *), cmp_post_order);
  for (size_t k = 0; k < b.nposts; k++) {
    b.posts[k]->newer = k == 0 ? NULL : b.posts[k - 1];
    b.posts[k]->older = k == b.nposts - 1 ? NULL : b.posts[k + 1];
  }

  /* tags */
  StrMap by_slug;
  smap_init(&by_slug);
  for (size_t k = 0; k < b.nposts; k++) {
    Source *p = b.posts[k];
    size_t nn = 0;
    const char **names = own_tags(&b, p, &nn);
    p->tag_refs = (Tag **)arena_alloc(a, (nn ? nn : 1) * sizeof(Tag *));
    p->ntag_refs = 0;
    for (size_t t = 0; t < nn; t++) {
      char *slug = slugify(a, names[t]);
      Tag *tag = (Tag *)smap_get(&by_slug, slug);
      if (!tag) {
        tag = (Tag *)arena_alloc(a, sizeof(Tag));
        memset(tag, 0, sizeof(*tag));
        tag->name = arena_strdup(a, names[t]);
        tag->slug = slug;
        StrBuf ub;
        sb_init(&ub);
        sb_add(&ub, b.base);
        sb_add(&ub, "/tags/");
        sb_add(&ub, slug);
        sb_addc(&ub, '/');
        tag->url = arena_strdup_n(a, ub.buf, ub.len);
        sb_free(&ub);
        smap_put(&by_slug, tag->slug, tag);
      }
      if (tag->nposts == tag->capposts) {
        size_t ncap = tag->capposts ? tag->capposts * 2 : 8;
        Source **np = (Source **)arena_alloc(a, ncap * sizeof(Source *));
        if (tag->posts) memcpy(np, tag->posts, tag->nposts * sizeof(Source *));
        tag->posts = np;
        tag->capposts = ncap;
      }
      tag->posts[tag->nposts++] = p;
      p->tag_refs[p->ntag_refs++] = tag;
    }
  }
  for (size_t i = 0; i < b.nsources; i++) {
    Source *s = b.sources[i];
    if (s->post) continue;
    size_t nn = 0;
    const char **names = own_tags(&b, s, &nn);
    s->tag_refs = (Tag **)arena_alloc(a, (nn ? nn : 1) * sizeof(Tag *));
    s->ntag_refs = 0;
    for (size_t t = 0; t < nn; t++) {
      char *slug = slugify(a, names[t]);
      Tag *tag = (Tag *)smap_get(&by_slug, slug);
      if (!tag) {
        tag = (Tag *)arena_alloc(a, sizeof(Tag));
        memset(tag, 0, sizeof(*tag));
        tag->name = arena_strdup(a, names[t]);
        tag->slug = slug;
        tag->url = arena_strdup(a, "");
        tag->posts = NULL;
        tag->nposts = 0;
        tag->capposts = 0;
      }
      s->tag_refs[s->ntag_refs++] = tag;
    }
  }
  /* b.tags = by_slug values, sorted by count desc then slug */
  {
    size_t n = 0;
    for (size_t i = 0; i < by_slug.cap; i++)
      if (by_slug.keys[i]) n++;
    b.tags = (Tag **)malloc((n ? n : 1) * sizeof(Tag *));
    b.ntags = 0;
    for (size_t i = 0; i < by_slug.cap; i++)
      if (by_slug.keys[i]) b.tags[b.ntags++] = (Tag *)by_slug.vals[i];
    qsort(b.tags, b.ntags, sizeof(Tag *), cmp_tag_order);
  }

  /* layouts */
  JVal *site = jnew(a, J_OBJ);
  const char *site_title = jstr(jobj_get(site_in, "title"));
  jobj_put(a, site, "title", jstr_val(a, site_title ? site_title : ""));
  jobj_put(a, site, "base_url", jstr_val(a, b.base));
  JVal *all_tags = jnew(a, J_ARR);
  for (size_t i = 0; i < b.ntags; i++) {
    Tag *t = b.tags[i];
    JVal *o = jnew(a, J_OBJ);
    jobj_put(a, o, "name", jstr_val(a, t->name));
    jobj_put(a, o, "slug", jstr_val(a, t->slug));
    jobj_put(a, o, "url", jstr_val(a, t->url));
    jobj_put(a, o, "count", jnum_val(a, (double)t->nposts));
    jarr_push(a, all_tags, o);
  }
  const JVal *recent_in = jobj_get(site_in, "recent");
  long recent_n = (recent_in && recent_in->t == J_NUM) ? (long)recent_in->num : 0;
  if (recent_n < 0) recent_n = 0;
  JVal *recent = jnew(a, J_ARR);
  for (long k = 0; k < recent_n && (size_t)k < b.nposts; k++) jarr_push(a, recent, card_of(&b, b.posts[k]));

  for (size_t i = 0; i < b.nsources; i++) {
    Source *s = b.sources[i];
    JVal *vars = jnew(a, J_OBJ);
    jobj_put(a, vars, "page", page_of(&b, s));
    StrBuf pb;
    sb_init(&pb);
    sb_add(&pb, s->out_dir);
    sb_add(&pb, "/index.html");
    char *path = arena_strdup_n(a, pb.buf, pb.len);
    sb_free(&pb);
    render_into(&b, path, s->layout, vars, site, all_tags, recent);
  }
  const JVal *per_in = jobj_get(site_in, "per_page");
  long per = (per_in && per_in->t == J_NUM) ? (long)per_in->num : 0;
  long pages = 1;
  if (per >= 1) {
    pages = (long)((b.nposts + (size_t)per - 1) / (size_t)per);
    if (pages < 1) pages = 1;
  }
  for (long k = 1; k <= pages; k++) {
    JVal *vars = jnew(a, J_OBJ);
    JVal *posts = jnew(a, J_ARR);
    long from = (k - 1) * per;
    long to = k * per;
    if (per < 1) {
      from = 0;
      to = 0;
    }
    for (long p = from; p < to && (size_t)p < b.nposts; p++) jarr_push(a, posts, card_of(&b, b.posts[p]));
    jobj_put(a, vars, "posts", posts);
    JVal *pager = jnew(a, J_OBJ);
    jobj_put(a, pager, "number", jnum_val(a, (double)k));
    jobj_put(a, pager, "total", jnum_val(a, (double)pages));
    StrBuf ub;
    sb_init(&ub);
    if (k == 1) {
      sb_add(&ub, b.base);
      sb_addc(&ub, '/');
    } else {
      sb_add(&ub, b.base);
      char tmp[32];
      int m = snprintf(tmp, sizeof(tmp), "/page/%ld/", k);
      sb_addn(&ub, tmp, (size_t)m);
    }
    char *url = arena_strdup_n(a, ub.buf, ub.len);
    sb_free(&ub);
    jobj_put(a, pager, "url", jstr_val(a, url));
    /* newer_url */
    StrBuf nb;
    sb_init(&nb);
    if (k != 1) {
      if (k - 1 == 1) {
        sb_add(&nb, b.base);
        sb_addc(&nb, '/');
      } else {
        sb_add(&nb, b.base);
        char tmp[32];
        int m = snprintf(tmp, sizeof(tmp), "/page/%ld/", k - 1);
        sb_addn(&nb, tmp, (size_t)m);
      }
    }
    jobj_put(a, pager, "newer_url", jstr_val(a, nb.buf));
    sb_free(&nb);
    StrBuf ob;
    sb_init(&ob);
    if (k != pages) {
      sb_add(&ob, b.base);
      char tmp[32];
      int m = snprintf(tmp, sizeof(tmp), "/page/%ld/", k + 1);
      sb_addn(&ob, tmp, (size_t)m);
    }
    jobj_put(a, pager, "older_url", jstr_val(a, ob.buf));
    sb_free(&ob);
    jobj_put(a, vars, "pager", pager);
    char *path;
    if (k == 1) {
      path = arena_strdup(a, "index.html");
    } else {
      StrBuf pb2;
      sb_init(&pb2);
      char tmp[32];
      int m = snprintf(tmp, sizeof(tmp), "page/%ld/index.html", k);
      sb_addn(&pb2, tmp, (size_t)m);
      path = arena_strdup_n(a, pb2.buf, pb2.len);
      sb_free(&pb2);
    }
    render_into(&b, path, "index", vars, site, all_tags, recent);
  }
  for (size_t i = 0; i < b.ntags; i++) {
    Tag *t = b.tags[i];
    JVal *vars = jnew(a, J_OBJ);
    jobj_put(a, vars, "tag", tag_of(&b, t));
    StrBuf pb;
    sb_init(&pb);
    sb_add(&pb, "tags/");
    sb_add(&pb, t->slug);
    sb_add(&pb, "/index.html");
    char *path = arena_strdup_n(a, pb.buf, pb.len);
    sb_free(&pb);
    render_into(&b, path, "tag", vars, site, all_tags, recent);
  }
  {
    JVal *vars = jnew(a, J_OBJ);
    render_into(&b, arena_strdup(a, "tags/index.html"), "tags", vars, site, all_tags, recent);
  }
  for (size_t i = 0; i < b.nstatics; i++) {
    if (smap_has(&b.out_map, b.statics[i].path)) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, "shadowed ");
      sb_add(&sb, b.statics[i].path);
      sb_add(&sb, ": a built page has its path");
      report_push(&b, arena_strdup_n(a, sb.buf, sb.len));
      sb_free(&sb);
    } else {
      out_put(&b, b.statics[i].path, b.statics[i].content);
    }
  }

  /* report */
  {
    size_t total = b.nreport + b.broken.n;
    const char **lines = (const char **)malloc((total ? total : 1) * sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; i < b.nreport; i++) lines[n++] = b.report[i];
    for (size_t i = 0; i < b.broken.cap; i++)
      if (b.broken.keys[i]) lines[n++] = b.broken.keys[i];
    qsort(lines, n, sizeof(char *), cmp_report_line);
    size_t npages = 0;
    for (size_t i = 0; i < b.nsources; i++)
      if (!b.sources[i]->post) npages++;
    StrBuf rb;
    sb_init(&rb);
    for (size_t i = 0; i < n; i++) {
      sb_add(&rb, lines[i]);
      sb_addc(&rb, '\n');
    }
    free(lines);
    char tmp[128];
    int m = snprintf(tmp, sizeof(tmp), "built %zu pages, %zu posts, %zu tags and %zu static files\n",
                     npages, b.nposts, b.ntags, b.nstatics);
    sb_addn(&rb, tmp, (size_t)m);
    out_put(&b, "build-report.txt", arena_strdup_n(a, rb.buf, rb.len));
    sb_free(&rb);
  }

  /* output object, keys sorted */
  {
    size_t n = b.nout;
    size_t *idx = (size_t *)malloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++) idx[i] = i;
    for (size_t i = 1; i < n; i++) {
      size_t id = idx[i];
      size_t k = i;
      while (k > 0 && strcmp(b.out_paths[idx[k - 1]], b.out_paths[id]) > 0) {
        idx[k] = idx[k - 1];
        k--;
      }
      idx[k] = id;
    }
    JVal *out = jnew(a, J_OBJ);
    for (size_t i = 0; i < n; i++) {
      size_t id = idx[i];
      JVal *v = jnew(a, J_STR);
      v->str = b.out_texts[id];
      v->len = strlen(v->str);
      jobj_put(a, out, b.out_paths[id], v);
    }
    free(idx);
    free(b.report);
    free(b.sources);
    free(b.statics);
    free(b.posts);
    free(b.tags);
    free(b.out_paths);
    free(b.out_texts);
    smap_free(&b.templates);
    smap_free(&b.by_source);
    smap_free(&b.broken);
    smap_free(&b.out_map);
    smap_free(&taken_by_dir);
    smap_free(&by_slug);
    return out;
  }
}

#endif
