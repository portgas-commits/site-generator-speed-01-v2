/* makefun-site's template dialect (lib/template.js): tokenize, parse into
   a node tree, render with a scope chain. Error strings match the reference
   exactly. Values are the JSON value tree (json.h); lookup walks the scope
   chain for the first name, then own properties. */
#ifndef TEMPLATE_C
#define TEMPLATE_C

#include "common.h"
#include "json.h"

#define INCLUDE_DEPTH 8

/* ---------------------------------------------------------------- nodes */

typedef enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE } NodeKind;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  const char *text; /* N_TEXT */
  char **path;      /* N_VAR/N_RAW/N_IF/N_FOR: split path */
  size_t path_len;
  bool not;               /* N_IF */
  Node *then_branch;      /* N_IF: array */
  size_t then_len;
  Node *else_branch;      /* N_IF: array or NULL */
  size_t else_len;
  const char *var_name;   /* N_FOR */
  Node *body;             /* N_FOR: array */
  size_t body_len;
  const char *inc_name;   /* N_INCLUDE */
};

/* A parsed template: its node array. */
typedef struct {
  Node *nodes;
  size_t n;
} Template;

/* Template parse error: arena message `template "<name>": <reason>`. */
typedef struct {
  bool failed;
  const char *msg;
} TplResult;

static char **split_path(Arena *a, const char *text, size_t len, size_t *out_n) {
  size_t n = 1;
  for (size_t i = 0; i < len; i++)
    if (text[i] == '.') n++;
  char **parts = (char **)arena_alloc(a, n * sizeof(char *));
  size_t idx = 0, start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || text[i] == '.') {
      parts[idx++] = arena_strdup_n(a, text + start, i - start);
      start = i + 1;
    }
  }
  *out_n = n;
  return parts;
}

static bool valid_path_text(const char *s, size_t n) {
  /* /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/ */
  if (n == 0) return false;
  size_t i = 0;
  for (;;) {
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '_')) return false;
    i++;
    while (i < n && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) i++;
    if (i == n) return true;
    if (s[i] != '.') return false;
    i++;
    if (i >= n) return false;
  }
}

static const char *tpl_err(Arena *a, const char *name, const char *reason) {
  StrBuf b;
  sb_init(&b);
  sb_add(&b, "template \"");
  sb_add(&b, name);
  sb_add(&b, "\": ");
  sb_add(&b, reason);
  char *m = arena_strdup_n(a, b.buf, b.len);
  sb_free(&b);
  return m;
}

/* Tokenize: produces a flat token list. Standalone {% %} tags take their
   whole line (including the newline) with them. */
typedef enum { T_TEXT, T_VAR, T_RAW, T_TAG } TokKind;

typedef struct {
  TokKind kind;
  const char *text; /* T_TEXT */
  size_t len;
  char **path; /* T_VAR/T_RAW */
  size_t path_len;
  const char *tag; /* T_TAG (trimmed) */
} Token;

typedef struct {
  Token *toks;
  size_t n, cap;
} TokList;

static void tok_push(Arena *a, TokList *l, Token *t) {
  if (l->n == l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 32;
    Token *nt = (Token *)arena_alloc(a, ncap * sizeof(Token));
    if (l->toks) memcpy(nt, l->toks, l->n * sizeof(Token));
    l->toks = nt;
    l->cap = ncap;
  }
  l->toks[l->n++] = *t;
}

static bool is_all_ws(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (s[i] != ' ' && s[i] != '\t') return false;
  return true;
}

/* Find needle in haystack starting at `from`; -1 when absent. */
static long find_sub(const char *s, size_t n, const char *needle, size_t from) {
  size_t nl = strlen(needle);
  if (nl == 0 || from + nl > n) return -1;
  for (size_t i = from; i + nl <= n; i++)
    if (memcmp(s + i, needle, nl) == 0) return (long)i;
  return -1;
}

static TplResult tokenize(Arena *a, const char *name, const char *src, size_t len, TokList *out) {
  memset(out, 0, sizeof(*out));
  TplResult r;
  r.failed = false;
  r.msg = NULL;
  StrBuf text;
  sb_init(&text);
  size_t i = 0;
  while (i < len) {
    long open = -1;
    for (size_t k = i; k < len; k++)
      if (src[k] == '{') {
        open = (long)k;
        break;
      }
    if (open == -1 || (size_t)open == len - 1) {
      sb_addn(&text, src + i, len - i);
      break;
    }
    char next = src[open + 1];
    if (next != '{' && next != '%') {
      sb_addn(&text, src + i, (size_t)(open - (long)i) + 1);
      i = (size_t)open + 1;
      continue;
    }
    sb_addn(&text, src + i, (size_t)(open - (long)i));
    if (next == '{') {
      bool raw = src[open + 2] == '{';
      long close = find_sub(src, len, raw ? "}}}" : "}}", (size_t)open + (raw ? 3 : 2));
      if (close == -1) {
        sb_free(&text);
        r.failed = true;
        r.msg = tpl_err(a, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        return r;
      }
      if (text.len > 0) {
        Token t;
        memset(&t, 0, sizeof(t));
        t.kind = T_TEXT;
        t.text = arena_strdup_n(a, text.buf, text.len);
        t.len = text.len;
        tok_push(a, out, &t);
        text.len = 0;
        text.buf[0] = 0;
      }
      size_t ps = (size_t)open + (raw ? 3 : 2);
      size_t pe = (size_t)close;
      /* trim the path text */
      while (ps < pe && (src[ps] == ' ' || src[ps] == '\t' || src[ps] == '\n' || src[ps] == '\r')) ps++;
      while (pe > ps && (src[pe - 1] == ' ' || src[pe - 1] == '\t' || src[pe - 1] == '\n' || src[pe - 1] == '\r')) pe--;
      if (!valid_path_text(src + ps, pe - ps)) {
        sb_free(&text);
        r.failed = true;
        /* the reference quotes the path with JSON.stringify */
        StrBuf rb;
        sb_init(&rb);
        sb_addc(&rb, '"');
        for (size_t q = ps; q < pe; q++) {
          char c = src[q];
          if (c == '"' || c == '\\') sb_addc(&rb, '\\');
          sb_addc(&rb, c);
        }
        sb_addc(&rb, '"');
        sb_add(&rb, " is not a path");
        r.msg = tpl_err(a, name, rb.buf);
        sb_free(&rb);
        return r;
      }
      Token t;
      memset(&t, 0, sizeof(t));
      t.kind = raw ? T_RAW : T_VAR;
      t.path = split_path(a, src + ps, pe - ps, &t.path_len);
      tok_push(a, out, &t);
      i = (size_t)close + (raw ? 3 : 2);
      continue;
    }
    /* {% %} */
    long close = find_sub(src, len, "%}", (size_t)open + 2);
    if (close == -1) {
      sb_free(&text);
      r.failed = true;
      r.msg = tpl_err(a, name, "a {% is not closed");
      return r;
    }
    size_t end = (size_t)close + 2;
    /* standalone: only spaces/tabs beside it on its line */
    size_t line_start = (size_t)open;
    while (line_start > 0 && src[line_start - 1] != '\n') line_start--;
    long nl = find_sub(src, len, "\n", end);
    size_t line_end = nl == -1 ? len : (size_t)nl;
    bool standalone = is_all_ws(src + line_start, (size_t)open - line_start) &&
                      is_all_ws(src + end, line_end - end);
    if (standalone) {
      /* drop the text back to the line start */
      text.len -= (size_t)open - line_start;
      text.buf[text.len] = 0;
    }
    if (text.len > 0) {
      Token t;
      memset(&t, 0, sizeof(t));
      t.kind = T_TEXT;
      t.text = arena_strdup_n(a, text.buf, text.len);
      t.len = text.len;
      tok_push(a, out, &t);
      text.len = 0;
      text.buf[0] = 0;
    }
    size_t ts = (size_t)open + 2;
    size_t te = (size_t)close;
    while (ts < te && (src[ts] == ' ' || src[ts] == '\t' || src[ts] == '\n' || src[ts] == '\r')) ts++;
    while (te > ts && (src[te - 1] == ' ' || src[te - 1] == '\t' || src[te - 1] == '\n' || src[te - 1] == '\r')) te--;
    Token t;
    memset(&t, 0, sizeof(t));
    t.kind = T_TAG;
    t.tag = arena_strdup_n(a, src + ts, te - ts);
    tok_push(a, out, &t);
    i = standalone ? (nl == -1 ? len : (size_t)nl + 1) : end;
  }
  if (text.len > 0) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.kind = T_TEXT;
    t.text = arena_strdup_n(a, text.buf, text.len);
    t.len = text.len;
    tok_push(a, out, &t);
  }
  sb_free(&text);
  return r;
}

/* ---------------------------------------------------------------- parse */

/* Node-list builder with a stack of open for/if nodes, mirroring the
   reference's stack of bodies. */
typedef struct {
  Node *arr;
  size_t n, cap;
  Arena *a;
} NodeList;

static void nl_push(Arena *a, NodeList *l, Node *nd) {
  if (l->n == l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 16;
    Node *na = (Node *)arena_alloc(a, ncap * sizeof(Node));
    if (l->arr) memcpy(na, l->arr, l->n * sizeof(Node));
    l->arr = na;
    l->cap = ncap;
  }
  l->arr[l->n++] = *nd;
}

static bool parse_for_tag(const char *tag, const char **var_name, size_t *var_len, const char **path_text, size_t *path_len) {
  /* /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/ */
  if (strncmp(tag, "for ", 4) != 0) return false;
  const char *p = tag + 4;
  if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')) return false;
  const char *e = p;
  while ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9') || *e == '_') e++;
  if (strncmp(e, " in ", 4) != 0) return false;
  const char *path = e + 4;
  if (*path == 0) return false;
  for (const char *q = path; *q; q++)
    if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') return false;
  *var_name = p;
  *var_len = (size_t)(e - p);
  *path_text = path;
  *path_len = strlen(path);
  return true;
}

static bool parse_if_tag(const char *tag, bool *not, const char **path_text, size_t *path_len) {
  /* /^if (not )?(\S+)$/ */
  if (strncmp(tag, "if ", 3) != 0) return false;
  const char *p = tag + 3;
  *not = false;
  if (strncmp(p, "not ", 4) == 0) {
    *not = true;
    p += 4;
  }
  if (*p == 0) return false;
  for (const char *q = p; *q; q++)
    if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') return false;
  *path_text = (char *)p;
  *path_len = strlen(p);
  return true;
}

static bool parse_include_tag(const char *tag, const char **name, size_t *name_len) {
  /* /^include "([a-z0-9-]+)"$/ */
  if (strncmp(tag, "include \"", 9) != 0) return false;
  const char *p = tag + 9;
  const char *e = p;
  while ((*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9') || *e == '-') e++;
  if (*e != '"' || e[1] != 0) return false;
  *name = p;
  *name_len = (size_t)(e - p);
  return true;
}

/* Parse tokens into a node list. On error, result.failed with the message.
   Frames store (parent list, index of the open node) rather than a Node*:
   a later push may realloc the list's array, which would stale a pointer. */
static TplResult parse_template(Arena *a, const char *name, TokList *toks, Template *out) {
  TplResult r;
  r.failed = false;
  r.msg = NULL;
  typedef struct {
    NodeList *parent_list;
    size_t index;   /* the open node within parent_list */
    NodeList *body_list;
    bool in_else;   /* for if: we passed the else */
  } Frame;
  Frame *frames = NULL;
  size_t fcap = 0;
  int sp = 0;
  NodeList root;
  memset(&root, 0, sizeof(root));
  root.a = a;
  NodeList *cur = &root;

  for (size_t ti = 0; ti < toks->n; ti++) {
    Token *tok = &toks->toks[ti];
    if (tok->kind == T_TEXT) {
      Node nd;
      memset(&nd, 0, sizeof(nd));
      nd.kind = N_TEXT;
      nd.text = tok->text;
      nl_push(a, cur, &nd);
    } else if (tok->kind == T_VAR || tok->kind == T_RAW) {
      Node nd;
      memset(&nd, 0, sizeof(nd));
      nd.kind = tok->kind == T_VAR ? N_VAR : N_RAW;
      nd.path = tok->path;
      nd.path_len = tok->path_len;
      nl_push(a, cur, &nd);
    } else {
      const char *tag = tok->tag;
      const char *var_name = NULL;
      size_t var_len = 0;
      const char *path_text = NULL;
      size_t path_len = 0;
      bool not = false;
      const char *inc = NULL;
      size_t inc_len = 0;
      if (parse_for_tag(tag, &var_name, &var_len, &path_text, &path_len)) {
        Node nd;
        memset(&nd, 0, sizeof(nd));
        nd.kind = N_FOR;
        nd.var_name = arena_strdup_n(a, var_name, var_len);
        nd.path = split_path(a, path_text, path_len, &nd.path_len);
        size_t index = cur->n;
        nl_push(a, cur, &nd);
        if ((size_t)sp == fcap) {
          fcap = fcap ? fcap * 2 : 16;
          frames = (Frame *)realloc(frames, fcap * sizeof(Frame));
        }
        NodeList *bl = (NodeList *)arena_alloc(a, sizeof(NodeList));
        memset(bl, 0, sizeof(*bl));
        bl->a = a;
        frames[sp].parent_list = cur;
        frames[sp].index = index;
        frames[sp].body_list = bl;
        frames[sp].in_else = false;
        sp++;
        cur = bl;
      } else if (parse_if_tag(tag, &not, &path_text, &path_len)) {
        Node nd;
        memset(&nd, 0, sizeof(nd));
        nd.kind = N_IF;
        nd.not = not;
        nd.path = split_path(a, path_text, path_len, &nd.path_len);
        size_t index = cur->n;
        nl_push(a, cur, &nd);
        if ((size_t)sp == fcap) {
          fcap = fcap ? fcap * 2 : 16;
          frames = (Frame *)realloc(frames, fcap * sizeof(Frame));
        }
        NodeList *bl = (NodeList *)arena_alloc(a, sizeof(NodeList));
        memset(bl, 0, sizeof(*bl));
        bl->a = a;
        frames[sp].parent_list = cur;
        frames[sp].index = index;
        frames[sp].body_list = bl;
        frames[sp].in_else = false;
        sp++;
        cur = bl;
      } else if (strcmp(tag, "else") == 0) {
        if (sp == 0) goto else_err;
        {
          Node *nodep = &frames[sp - 1].parent_list->arr[frames[sp - 1].index];
          if (nodep->kind != N_IF || frames[sp - 1].in_else) goto else_err;
          nodep->then_branch = frames[sp - 1].body_list->arr;
          nodep->then_len = frames[sp - 1].body_list->n;
          NodeList *bl = (NodeList *)arena_alloc(a, sizeof(NodeList));
          memset(bl, 0, sizeof(*bl));
          bl->a = a;
          frames[sp - 1].body_list = bl;
          frames[sp - 1].in_else = true;
          cur = bl;
        }
        continue;
      else_err:
        r.failed = true;
        r.msg = tpl_err(a, name, "an else outside an if");
        free(frames);
        return r;
      } else if (strcmp(tag, "endfor") == 0 || strcmp(tag, "endif") == 0) {
        NodeKind want = strcmp(tag, "endfor") == 0 ? N_FOR : N_IF;
        if (sp == 0) goto close_err;
        {
          Node *nodep = &frames[sp - 1].parent_list->arr[frames[sp - 1].index];
          if (nodep->kind != want) goto close_err;
          sp--;
          NodeList *bl = frames[sp].body_list;
          if (nodep->kind == N_FOR) {
            nodep->body = bl->arr;
            nodep->body_len = bl->n;
          } else if (frames[sp].in_else) {
            nodep->else_branch = bl->arr;
            nodep->else_len = bl->n;
          } else {
            nodep->then_branch = bl->arr;
            nodep->then_len = bl->n;
          }
          cur = sp == 0 ? &root : frames[sp - 1].body_list;
        }
        continue;
      close_err:
        r.failed = true;
        {
          char *reason = (char *)arena_alloc(a, 64);
          snprintf(reason, 64, "an %s with no open %s", tag, want == N_FOR ? "for" : "if");
          r.msg = tpl_err(a, name, reason);
        }
        free(frames);
        return r;
      } else if (parse_include_tag(tag, &inc, &inc_len)) {
        Node nd;
        memset(&nd, 0, sizeof(nd));
        nd.kind = N_INCLUDE;
        nd.inc_name = arena_strdup_n(a, inc, inc_len);
        nl_push(a, cur, &nd);
      } else {
        r.failed = true;
        StrBuf rb;
        sb_init(&rb);
        sb_add(&rb, "{% ");
        sb_add(&rb, tag);
        sb_add(&rb, " %} is not a tag");
        r.msg = tpl_err(a, name, rb.buf);
        sb_free(&rb);
        free(frames);
        return r;
      }
    }
  }
  if (sp > 0) {
    r.failed = true;
    Node *nodep = &frames[sp - 1].parent_list->arr[frames[sp - 1].index];
    char *reason = (char *)arena_alloc(a, 48);
    snprintf(reason, 48, "a %s is not closed", nodep->kind == N_FOR ? "for" : "if");
    r.msg = tpl_err(a, name, reason);
    free(frames);
    return r;
  }
  free(frames);
  out->nodes = root.arr;
  out->n = root.n;
  return r;
}

/* ---------------------------------------------------------------- render */

/* A scope frame: a JVal object whose own properties are the frame's vars,
   plus a parent frame. The root frame's object is the render vars object. */
typedef struct Scope Scope;
struct Scope {
  const JVal *vars;
  const Scope *parent;
};

static const JVal *lookup_path(const Scope *scope, char **path, size_t path_len) {
  const Scope *s = scope;
  while (s && !jobj_get(s->vars, path[0])) s = s->parent;
  if (!s) return NULL;
  const JVal *v = jobj_get(s->vars, path[0]);
  for (size_t k = 1; k < path_len; k++) {
    if (!v || v->t != J_OBJ) return NULL;
    v = jobj_get(v, path[k]);
    if (!v) return NULL;
  }
  return v;
}

/* A value as text (textOf): string itself, number in JS digits, true/false,
   array items joined by ", ", anything else "". */
static void text_of_into(StrBuf *b, const JVal *v) {
  if (!v) return;
  switch (v->t) {
    case J_STR:
      sb_addn(b, v->str, v->len);
      break;
    case J_NUM: {
      double d = v->num;
      if (d == (double)(long long)d) {
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
    case J_BOOL:
      sb_add(b, v->b ? "true" : "false");
      break;
    case J_ARR:
      for (size_t i = 0; i < v->n; i++) {
        if (i) sb_addn(b, ", ", 2);
        text_of_into(b, v->items[i]);
      }
      break;
    default:
      break;
  }
}

static bool truthy(const JVal *v) {
  if (!v) return false;
  if (v->t == J_ARR) return v->n > 0;
  if (v->t == J_NULL) return false;
  if (v->t == J_BOOL) return v->b;
  if (v->t == J_STR) return v->len > 0;
  if (v->t == J_NUM) return v->num != 0;
  return true; /* objects (incl. empty) are truthy */
}

typedef struct {
  StrMap *templates; /* name -> Template* */
  Arena *a;
} RenderCtx;

static void render_nodes(StrBuf *out, Node *nodes, size_t n, const Scope *scope, RenderCtx *ctx, int depth);

static void render_nodes(StrBuf *out, Node *nodes, size_t n, const Scope *scope, RenderCtx *ctx, int depth) {
  for (size_t i = 0; i < n; i++) {
    Node *nd = &nodes[i];
    switch (nd->kind) {
      case N_TEXT:
        sb_add(out, nd->text);
        break;
      case N_VAR: {
        const JVal *v = lookup_path(scope, nd->path, nd->path_len);
        StrBuf tmp;
        sb_init(&tmp);
        text_of_into(&tmp, v);
        escape_html_into(out, tmp.buf, tmp.len);
        sb_free(&tmp);
        break;
      }
      case N_RAW: {
        const JVal *v = lookup_path(scope, nd->path, nd->path_len);
        text_of_into(out, v);
        break;
      }
      case N_IF: {
        const JVal *v = lookup_path(scope, nd->path, nd->path_len);
        bool tv = truthy(v);
        Node *branch = NULL;
        size_t blen = 0;
        if (tv != nd->not) {
          branch = nd->then_branch;
          blen = nd->then_len;
        } else {
          branch = nd->else_branch;
          blen = nd->else_len;
        }
        if (branch) render_nodes(out, branch, blen, scope, ctx, depth);
        break;
      }
      case N_FOR: {
        const JVal *xs = lookup_path(scope, nd->path, nd->path_len);
        if (!xs || xs->t != J_ARR) break;
        /* One vars object and one loop object per for-execution, mutated
           per iteration (the old code allocated a fresh vars object, a
           fresh loop object and four field values per item). The values
           are only ever read during that iteration's render, so mutating
           them between iterations is safe. var_name is items[0]: it is
           put first, before "loop". */
        JVal *vars = jnew(ctx->a, J_OBJ);
        JVal *loop = jnew(ctx->a, J_OBJ);
        JVal *idx = jnew(ctx->a, J_NUM);
        JVal *first = jnew(ctx->a, J_BOOL);
        JVal *last = jnew(ctx->a, J_BOOL);
        JVal *len = jnew(ctx->a, J_NUM);
        jobj_put(ctx->a, loop, "index", idx);
        jobj_put(ctx->a, loop, "first", first);
        jobj_put(ctx->a, loop, "last", last);
        jobj_put(ctx->a, loop, "length", len);
        jobj_put(ctx->a, vars, nd->var_name, xs->n ? xs->items[0] : NULL);
        jobj_put(ctx->a, vars, "loop", loop);
        Scope sub = {vars, scope};
        for (size_t k = 0; k < xs->n; k++) {
          vars->items[0] = xs->items[k];
          idx->num = (double)(k + 1);
          first->b = k == 0;
          last->b = k == xs->n - 1;
          len->num = (double)xs->n;
          render_nodes(out, nd->body, nd->body_len, &sub, ctx, depth);
        }
        break;
      }
      case N_INCLUDE: {
        Template *t = (Template *)smap_get(ctx->templates, nd->inc_name);
        if (t && depth < INCLUDE_DEPTH) render_nodes(out, t->nodes, t->n, scope, ctx, depth + 1);
        break;
      }
    }
  }
}

#endif
