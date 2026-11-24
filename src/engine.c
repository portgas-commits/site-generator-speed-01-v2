/* N-API binding for the native makefun-site engine.
   Exports:
     build(jsonStr)  - one case's input as a JSON string -> its output tree
                       as a JSON string.
     runAll(jsonStr) - the whole cases.json text (a JSON array of
                       {id, input}) -> the answers array as a JSON string.
   Each case is built in its own fresh arena; nothing is carried from one
   case to the next. The returned strings are copied into JS before the
   arena and buffers are freed. */
#include <node_api.h>
#include <stdlib.h>
#include <string.h>
#include "build.c"

static napi_value throw_type_error(napi_env env, const char *msg) {
  napi_throw_type_error(env, NULL, msg);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/* Read a JS string argument as UTF-8 bytes (malloc'd, NUL-terminated). */
static char *read_js_string(napi_env env, napi_value arg, size_t *out_len) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, arg, NULL, 0, &len) != napi_ok) return NULL;
  char *buf = (char *)malloc(len + 1);
  if (!buf) return NULL;
  size_t written = 0;
  if (napi_get_value_string_utf8(env, arg, buf, len + 1, &written) != napi_ok) {
    free(buf);
    return NULL;
  }
  buf[written] = 0;
  *out_len = written;
  return buf;
}

static napi_value make_js_string(napi_env env, const char *s, size_t n) {
  napi_value ret;
  if (napi_create_string_utf8(env, s, n, &ret) != napi_ok) return NULL;
  return ret;
}

/* Read a JS string or Buffer argument as bytes. For a Buffer the bytes are
   borrowed from V8 (no copy); for a string they are malloc'd and must be
   freed by the caller. Sets *is_buffer. */
static char *read_js_input(napi_env env, napi_value arg, size_t *out_len, bool *is_buffer) {
  bool buf = false;
  if (napi_is_buffer(env, arg, &buf) == napi_ok && buf) {
    void *data = NULL;
    size_t len = 0;
    if (napi_get_buffer_info(env, arg, &data, &len) != napi_ok) return NULL;
    *out_len = len;
    *is_buffer = true;
    return (char *)data;
  }
  *is_buffer = false;
  return read_js_string(env, arg, out_len);
}

/* Finalizer for an external output Buffer: frees the StrBuf's buffer. */
static void free_strbuf(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  free(data);
}

static napi_value js_build(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1)
    return throw_type_error(env, "build(json) needs one argument");
  size_t len = 0;
  char *text = read_js_string(env, args[0], &len);
  if (!text) return throw_type_error(env, "build(json) needs a string");
  Arena a;
  arena_init(&a);
  const JVal *input = json_parse(&a, text, len);
  if (!input) {
    free(text);
    arena_free(&a);
    return throw_type_error(env, "build(json): invalid JSON");
  }
  JVal *out = build_case(&a, input);
  StrBuf sb;
  sb_init(&sb);
  json_write(&sb, out);
  napi_value ret = make_js_string(env, sb.buf, sb.len);
  sb_free(&sb);
  free(text);
  arena_free(&a);
  return ret;
}

static napi_value js_run_all(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1)
    return throw_type_error(env, "runAll(json) needs one argument");
  size_t len = 0;
  bool is_buffer = false;
  char *text = read_js_input(env, args[0], &len, &is_buffer);
  if (!text) return throw_type_error(env, "runAll(json) needs a string or Buffer");
  Arena a;
  arena_init(&a);
  const JVal *cases = json_parse(&a, text, len);
  if (!cases || cases->t != J_ARR) {
    if (!is_buffer) free(text);
    arena_free(&a);
    return throw_type_error(env, "runAll(json): expected a JSON array of cases");
  }
  StrBuf sb;
  sb_init(&sb);
  sb_addc(&sb, '[');
  for (size_t i = 0; i < cases->n; i++) {
    if (i) sb_addc(&sb, ',');
    const JVal *input = jobj_get(cases->items[i], "input");
    if (!input) {
      sb_addn(&sb, "null", 4);
      continue;
    }
    /* A fresh arena per case: each case answered alone. build_case only
       reads its input and copies what it keeps, so the shared parsed tree
       may live in the outer arena. */
    Arena ca;
    arena_init(&ca);
    JVal *out = build_case(&ca, input);
    json_write(&sb, out);
    arena_free(&ca);
  }
  sb_addc(&sb, ']');
  /* Return the output as an external Buffer: the bytes are handed to V8
     without a copy, and the finalizer frees them when the Buffer is
     collected. (The input Buffer's bytes were borrowed, never freed.) */
  napi_value ret;
  if (napi_create_external_buffer(env, sb.len, sb.buf, free_strbuf, NULL, &ret) != napi_ok) {
    sb_free(&sb);
    if (!is_buffer) free(text);
    arena_free(&a);
    return throw_type_error(env, "runAll(json): could not allocate output");
  }
  if (!is_buffer) free(text);
  arena_free(&a);
  return ret;
}

NAPI_MODULE_INIT() {
  napi_value build_fn, runall_fn;
  napi_create_function(env, "build", NAPI_AUTO_LENGTH, js_build, NULL, &build_fn);
  napi_create_function(env, "runAll", NAPI_AUTO_LENGTH, js_run_all, NULL, &runall_fn);
  napi_set_named_property(env, exports, "build", build_fn);
  napi_set_named_property(env, exports, "runAll", runall_fn);
  return exports;
}
