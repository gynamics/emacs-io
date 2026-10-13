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

emacs_value stream_or_error(emacs_env *env, cl_stream_t *s, const char *errmsg) {
  if (!s) {
    signal_error(env, errmsg);
    return syms.v_nil;
  }
  return euser_ptr_stream_make(env, s);
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
  efuncall_cached(env, syms.fn_error, 1, args);
}

void signal_end_of_file(emacs_env *env, cl_stream_t *s) {
  emacs_value sv = euser_ptr_stream_make(env, s);
  emacs_value data = efuncall_cached(env, syms.fn_list, 1, &sv);
  emacs_value args[2] = { syms.v_end_of_file, data };
  efuncall_cached(env, syms.fn_signal, 2, args);
}

/* ── pre-interned symbol table ─────────────────────────────────────────── */

syms_t syms;

void syms_init(emacs_env *env) {
#define G(e) env->make_global_ref(env, (e))
  syms.k_direction    = G(eintern(env, ":direction"));
  syms.k_if_exists    = G(eintern(env, ":if-exists"));
  syms.k_if_does_not_exist = G(eintern(env, ":if-does-not-exist"));
  syms.k_element_type = G(eintern(env, ":element-type"));
  syms.k_external_format = G(eintern(env, ":external-format"));
  syms.k_supersede    = G(eintern(env, ":supersede"));
  syms.k_append       = G(eintern(env, ":append"));
  syms.k_overwrite    = G(eintern(env, ":overwrite"));
  syms.k_error        = G(eintern(env, ":error"));
  syms.k_start        = G(eintern(env, ":start"));
  syms.k_end          = G(eintern(env, ":end"));
  syms.k_size         = G(eintern(env, ":size"));
  syms.k_stream       = G(eintern(env, ":stream"));
  syms.k_escape       = G(eintern(env, ":escape"));
  syms.k_radix        = G(eintern(env, ":radix"));
  syms.k_base         = G(eintern(env, ":base"));
  syms.k_circle       = G(eintern(env, ":circle"));
  syms.k_pretty       = G(eintern(env, ":pretty"));
  syms.k_level        = G(eintern(env, ":level"));
  syms.k_length       = G(eintern(env, ":length"));
  syms.k_case         = G(eintern(env, ":case"));
  syms.k_gensym       = G(eintern(env, ":gensym"));
  syms.k_array        = G(eintern(env, ":array"));
  syms.k_readably     = G(eintern(env, ":readably"));
  syms.k_right_margin = G(eintern(env, ":right-margin"));
  syms.k_miser_width  = G(eintern(env, ":miser-width"));
  syms.k_pprint_dispatch = G(eintern(env, ":pprint-dispatch"));
  syms.k_lines        = G(eintern(env, ":lines"));
  syms.k_preserve_whitespace = G(eintern(env, ":preserve-whitespace"));
  syms.k_abort        = G(eintern(env, ":abort"));

  syms.d_input  = G(eintern(env, ":input"));
  syms.d_output = G(eintern(env, ":output"));
  syms.d_io     = G(eintern(env, ":io"));
  syms.d_probe  = G(eintern(env, ":probe"));

  syms.fn_car            = G(eintern(env, "car"));
  syms.fn_cdr            = G(eintern(env, "cdr"));
  syms.fn_list           = G(eintern(env, "list"));
  syms.fn_cons           = G(eintern(env, "cons"));
  syms.fn_concat         = G(eintern(env, "concat"));
  syms.fn_set            = G(eintern(env, "set"));
  syms.fn_symbol_value   = G(eintern(env, "symbol-value"));
  syms.fn_read_from_string = G(eintern(env, "read-from-string"));
  syms.fn_prin1_to_string = G(eintern(env, "prin1-to-string"));
  syms.fn_buffer_substring = G(eintern(env, "buffer-substring-no-properties"));
  syms.fn_buffer_size    = G(eintern(env, "buffer-size"));
  syms.fn_defalias       = G(eintern(env, "defalias"));
  syms.fn_provide        = G(eintern(env, "provide"));
  syms.fn_error          = G(eintern(env, "error"));
  syms.fn_signal         = G(eintern(env, "signal"));

  syms.v_nil       = G(eintern(env, "nil"));
  syms.v_t         = G(eintern(env, "t"));
  syms.v_character = G(eintern(env, "character"));
  syms.v_string    = G(eintern(env, "string"));
  syms.v_end_of_file = G(eintern(env, "end-of-file"));
  syms.v_unsigned_byte_8 = G(eintern(env, "unsigned-byte"));

  syms.var_sin  = G(eintern(env, "*standard-input*"));
  syms.var_sout = G(eintern(env, "*standard-output*"));
  syms.var_serr = G(eintern(env, "*error-output*"));
#undef G
}

/* ── cached API (pre-interned symbol variants) ────────────────────────── */

bool kw_eq_cached(emacs_env *env, emacs_value kw, emacs_value sym) {
  return env->eq(env, kw, sym);
}

emacs_value efuncall_cached(emacs_env *env, emacs_value sym, ptrdiff_t nargs,
                            emacs_value *args) {
  return env->funcall(env, sym, nargs, args);
}

cl_stream_t *get_stream_or_default_cached(emacs_env *env, emacs_value *args,
                                           ptrdiff_t nargs, int argidx,
                                           emacs_value var_sym) {
  if (argidx < nargs && env->is_not_nil(env, args[argidx]))
    return euser_ptr_stream_get(env, args[argidx]);

  emacs_value sv = efuncall_cached(env, syms.fn_symbol_value, 1,
                                   (emacs_value[]){ var_sym });
  return euser_ptr_stream_get(env, sv);
}

/* ── keyword helpers ──────────────────────────────────────────────────── */

bool kw_eq(emacs_env *env, emacs_value kw, const char *name) {
  return env->eq(env, kw, eintern(env, name));
}

/* ── sequence classification ──────────────────────────────────────────── */

cl_seq_kind_t seq_classify(emacs_env *env, emacs_value seq, ptrdiff_t *len_out) {
  emacs_value type = env->type_of(env, seq);

  if (env->eq(env, type, eintern(env, "vector"))) {
    *len_out = env->vec_size(env, seq);
    return CL_SEQ_VECTOR;
  }
  if (env->eq(env, type, syms.v_string)) {
    ptrdiff_t n = 0;
    env->copy_string_contents(env, seq, NULL, &n);
    *len_out = n - 1;  /* exclude the trailing NUL */
    return CL_SEQ_STRING;
  }
  if (env->eq(env, type, eintern(env, "cons"))) {
    *len_out = 1 << 30;  /* a list has no cheap length: treat as unbounded */
    return CL_SEQ_LIST;
  }

  *len_out = 0;
  return CL_SEQ_INVALID;
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
