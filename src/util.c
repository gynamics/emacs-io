#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ── runtime pointer ──────────────────────────────────────────────────── */

struct emacs_runtime *cl_rt;
emacs_env *cl_env(void) { return cl_rt ? cl_rt->get_environment(cl_rt) : NULL; }

/* ── emacs_env helpers ────────────────────────────────────────────────── */

emacs_value eintern(emacs_env *env, const char *name) {
  return env->intern(env, name);
}

emacs_value efuncall(emacs_env *env, const char *fname, ptrdiff_t nargs,
                     emacs_value *args) {
  emacs_value sym = eintern(env, fname);
  return env->funcall(env, sym, nargs, args);
}

emacs_value estring(emacs_env *env, const char *s) {
  return env->make_string(env, s, strlen(s));
}

char *estrdup(emacs_env *env, emacs_value val) {
  ptrdiff_t len = 0;
  if (!estring_copy(env, val, NULL, &len)) return NULL;
  char *buf = malloc(len);
  if (!buf) return NULL;
  if (!estring_copy(env, val, buf, &len)) {
    free(buf);
    return NULL;
  }
  return buf;
}

bool estring_copy(emacs_env *env, emacs_value val, char *buf, ptrdiff_t *lenp) {
  return env->copy_string_contents(env, val, buf, lenp);
}

char *estring_alloc(emacs_env *env, emacs_value val, ptrdiff_t *len_out) {
  ptrdiff_t sz = 0;
  env->copy_string_contents(env, val, NULL, &sz);
  if (sz == 0) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  char *buf = malloc(sz);
  if (!buf) return NULL;
  if (!env->copy_string_contents(env, val, buf, &sz)) {
    free(buf);
    return NULL;
  }
  if (len_out) *len_out = sz - 1; /* without null terminator */
  return buf;
}

emacs_value eint(emacs_env *env, intmax_t n) {
  return env->make_integer(env, n);
}

intmax_t eint_extract(emacs_env *env, emacs_value v) {
  return env->extract_integer(env, v);
}

/* ── defun registration ───────────────────────────────────────────────── */

void defun(emacs_env *env, const char *name, emacs_subr_fn fn,
           ptrdiff_t min, ptrdiff_t max, const char *doc, void *data) {
  emacs_value f = env->make_function(env, min, max, fn, doc, data);
  emacs_value args[2];
  args[0] = eintern(env, name);
  args[1] = f;
  emacs_value fset_sym = eintern(env, "defalias");
  env->funcall(env, fset_sym, 2, args);
}

void provide(emacs_env *env, const char *feature) {
  emacs_value args[1];
  args[0] = eintern(env, feature);
  efuncall(env, "provide", 1, args);
}

/* ── stream <-> emacs_value helpers ────────────────────────────────────── */

static void stream_user_ptr_finalizer(void *ptr) {
  cl_stream_t *s = (cl_stream_t *)ptr;
  stream_close(s);
  stream_free(s);
}

cl_stream_t *euser_ptr_stream_get(emacs_env *env, emacs_value val) {
  return (cl_stream_t *)env->get_user_ptr(env, val);
}

emacs_value euser_ptr_stream_make(emacs_env *env, cl_stream_t *s) {
  return env->make_user_ptr(env, stream_user_ptr_finalizer, s);
}

/* ── stream default resolution ──────────────────────────────────────── */

cl_stream_t *get_stream_or_default(emacs_env *env, emacs_value *args,
                                    ptrdiff_t nargs, int argidx,
                                    const char *var) {
  if (argidx < nargs && env->is_not_nil(env, args[argidx]))
    return euser_ptr_stream_get(env, args[argidx]);

  emacs_value sv = efuncall(env, "symbol-value", 1,
                             (emacs_value[]){ eintern(env, var) });
  return euser_ptr_stream_get(env, sv);
}

/* ── error signaling ──────────────────────────────────────────────────── */

void signal_error(emacs_env *env, const char *msg) {
  emacs_value args[1];
  args[0] = estring(env, msg);
  efuncall(env, "error", 1, args);
}

/* ── keyword helpers ──────────────────────────────────────────────────── */

bool kw_eq(emacs_env *env, emacs_value kw, const char *name) {
  return env->eq(env, kw, eintern(env, name));
}

/* ── string builder ───────────────────────────────────────────────────── */

strbuf_t *strbuf_create(size_t initial_cap) {
  strbuf_t *sb = malloc(sizeof(*sb));
  if (!sb) return NULL;
  sb->data = malloc(initial_cap);
  if (!sb->data) { free(sb); return NULL; }
  sb->len = 0;
  sb->cap = initial_cap;
  sb->data[0] = '\0';
  return sb;
}

void strbuf_free(strbuf_t *sb) {
  if (!sb) return;
  free(sb->data);
  free(sb);
}

bool strbuf_append(strbuf_t *sb, const char *s, ptrdiff_t len) {
  if (len <= 0) return true;
  size_t needed = sb->len + (size_t)len + 1;
  if (needed > sb->cap) {
    size_t new_cap = sb->cap;
    while (new_cap < needed) new_cap = (new_cap * 3) / 2;
    char *new_data = realloc(sb->data, new_cap);
    if (!new_data) return false;
    sb->data = new_data;
    sb->cap = new_cap;
  }
  memcpy(sb->data + sb->len, s, (size_t)len);
  sb->len += (size_t)len;
  sb->data[sb->len] = '\0';
  return true;
}

emacs_value strbuf_to_estring(emacs_env *env, strbuf_t *sb) {
  return env->make_string(env, sb->data, (ptrdiff_t)sb->len);
}
