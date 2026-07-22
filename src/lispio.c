#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── try-read-from-buffer ────────────────────────────────────────────── */

static bool try_read_from_buffer(emacs_env *env, strbuf_t *sb,
                                  emacs_value *result, ptrdiff_t *consumed) {
  emacs_value estr = env->make_string(env, sb->data, (ptrdiff_t)sb->len);
  emacs_value read_args[2];
  read_args[0] = estr;
  read_args[1] = eint(env, 0);

  emacs_value raw = env->funcall(env, syms.fn_read_from_string,
                                  2, read_args);

  enum emacs_funcall_exit exit = env->non_local_exit_check(env);
  if (exit == emacs_funcall_exit_return) {
    emacs_value pair = raw;
    *result = efuncall_cached(env, syms.fn_car, 1, &pair);
    emacs_value pos  = efuncall_cached(env, syms.fn_cdr, 1, &pair);
    *consumed = eint_extract(env, pos);
    return true;
  }

  emacs_value sym = syms.v_nil;
  emacs_value data = syms.v_nil;
  env->non_local_exit_get(env, &sym, &data);
  bool is_eof = env->eq(env, sym, syms.v_end_of_file);
  env->non_local_exit_clear(env);

  if (is_eof)
    return false;

  env->non_local_exit_signal(env, sym, data);
  *consumed = 0;
  return false;
}

/* ── cl-read ──────────────────────────────────────────────────────────── */

emacs_value Fread(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                  void *data) {
  (void)data;
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-read: stream is closed");
    return syms.v_nil;
  }

  strbuf_t *sb = strbuf_create(256);
  if (!sb) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  emacs_value result = syms.v_nil;
  ptrdiff_t consumed = 0;
  bool parsed = false;
  unsigned char chunk[4096];

  while (!parsed) {
    if (sb->len > 0) {
      if (try_read_from_buffer(env, sb, &result, &consumed)) {
        parsed = true;
        break;
      }
      if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
        strbuf_free(sb);
        return syms.v_nil;
      }
    }

    ptrdiff_t r = stream_read(s, chunk, sizeof(chunk));
    if (r <= 0) {
      if (sb->len > 0) {
        while (sb->len > 0 &&
               (sb->data[sb->len - 1] == ' '  ||
                sb->data[sb->len - 1] == '\t' ||
                sb->data[sb->len - 1] == '\n' ||
                sb->data[sb->len - 1] == '\r' ||
                sb->data[sb->len - 1] == '\f'))
          sb->data[--sb->len] = '\0';
        if (sb->len > 0) {
          if (try_read_from_buffer(env, sb, &result, &consumed)) {
            parsed = true;
            break;
          }
          if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
            strbuf_free(sb);
            return syms.v_nil;
          }
        }
      }
      strbuf_free(sb);
      if (nargs >= 2 && !env->is_not_nil(env, args[1]))
        return (nargs >= 3) ? args[2] : syms.v_nil;
      signal_error(env, "End of file on stream");
      return syms.v_nil;
    }

    strbuf_append(sb, (const char *)chunk, r);
  }

  if (consumed < (ptrdiff_t)sb->len) {
    for (ptrdiff_t i = consumed; i < (ptrdiff_t)sb->len; i++)
      stream_unread(s, (unsigned char)sb->data[i]);
  }

  strbuf_free(sb);
  return result;
}

/* ── cl-read-from-string ─────────────────────────────────────────────── */

emacs_value Fread_from_string(emacs_env *env, ptrdiff_t nargs,
                              emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-read-from-string requires 1 argument");
    return syms.v_nil;
  }

  ptrdiff_t start = 0;
  ptrdiff_t end = -1;
  bool preserve_ws = false;
  bool eoferr = (nargs < 2) || env->is_not_nil(env, args[1]);
  emacs_value eofval = (nargs >= 3) ? args[2] : syms.v_nil;

  for (ptrdiff_t i = 3; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_start))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_end))
      end = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_preserve_whitespace))
      preserve_ws = env->is_not_nil(env, args[i + 1]);
  }

  ptrdiff_t slen = 0;
  char *str = estring_alloc(env, args[0], &slen);
  if (!str) return syms.v_nil;

  if (end < 0 || end > slen) end = slen;
  if (start < 0) start = 0;
  if (start > slen) { free(str); return eofval; }

  emacs_value sstr = env->make_string(env, str + start, end - start);

  emacs_value raw = efuncall_cached(env, syms.fn_read_from_string, 1, &sstr);
  if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
    env->non_local_exit_clear(env);
    free(str);
    if (!eoferr) return eofval;
    signal_error(env, "End of file on string");
    return syms.v_nil;
  }

  emacs_value car_val = efuncall_cached(env, syms.fn_car, 1, &raw);
  ptrdiff_t pos = eint_extract(env, efuncall_cached(env, syms.fn_cdr, 1, &raw));
  if (pos == 0 && end > start) {
    free(str);
    if (!eoferr) return eofval;
    signal_error(env, "End of file on string");
    return syms.v_nil;
  }

  /* strip trailing whitespace unless :preserve-whitespace */
  if (pos > 0 && !preserve_ws) {
    const char *base = str + start;
    while (pos > 0 &&
           (base[pos - 1] == ' '  || base[pos - 1] == '\t' ||
            base[pos - 1] == '\n' || base[pos - 1] == '\r' ||
            base[pos - 1] == '\f'))
      pos--;
  }
  free(str);

  emacs_value cons_args[2];
  cons_args[0] = car_val;
  cons_args[1] = eint(env, start + pos);
  return efuncall_cached(env, syms.fn_cons, 2, cons_args);
}

/* ── cl-write ─────────────────────────────────────────────────────────── */

static emacs_value write_bind_var(emacs_env *env, emacs_value privar,
                                   emacs_value val) {
  emacs_value sv_fn = syms.fn_symbol_value;
  emacs_value o = efuncall_cached(env, sv_fn, 1, &privar);
  emacs_value a[2] = {privar, val};
  efuncall_cached(env, syms.fn_set, 2, a);
  return o;
}

static void write_restore_var(emacs_env *env, emacs_value privar,
                               emacs_value old) {
  emacs_value a[2] = {privar, old};
  efuncall_cached(env, syms.fn_set, 2, a);
}

emacs_value Fwrite(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-write requires 1 argument");
    return syms.v_nil;
  }

  emacs_value obj = args[0];
  cl_stream_t *s = NULL;
  emacs_value esc = syms.v_nil, pretty = syms.v_nil, circle = syms.v_nil;
  emacs_value level = syms.v_nil, length = syms.v_nil, base = syms.v_nil;
  emacs_value radix = syms.v_nil, kase = syms.v_nil, gensym = syms.v_nil;
  emacs_value readably = syms.v_nil, array = syms.v_nil;
  bool h_esc = false, h_pretty = false, h_circle = false;
  bool h_level = false, h_length = false, h_base = false;
  bool h_radix = false, h_case = false, h_gensym = false;
  bool h_readably = false, h_array = false;

  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_stream))
      { s = euser_ptr_stream_get(env, args[i + 1]); }
    else if (kw_eq_cached(env, args[i], syms.k_escape))
      { esc = args[i + 1]; h_esc = true; }
    else if (kw_eq_cached(env, args[i], syms.k_pretty))
      { pretty = args[i + 1]; h_pretty = true; }
    else if (kw_eq_cached(env, args[i], syms.k_circle))
      { circle = args[i + 1]; h_circle = true; }
    else if (kw_eq_cached(env, args[i], syms.k_level))
      { level = args[i + 1]; h_level = true; }
    else if (kw_eq_cached(env, args[i], syms.k_length))
      { length = args[i + 1]; h_length = true; }
    else if (kw_eq_cached(env, args[i], syms.k_base))
      { base = args[i + 1]; h_base = true; }
    else if (kw_eq_cached(env, args[i], syms.k_radix))
      { radix = args[i + 1]; h_radix = true; }
    else if (kw_eq_cached(env, args[i], syms.k_case))
      { kase = args[i + 1]; h_case = true; }
    else if (kw_eq_cached(env, args[i], syms.k_gensym))
      { gensym = args[i + 1]; h_gensym = true; }
    else if (kw_eq_cached(env, args[i], syms.k_readably))
      { readably = args[i + 1]; h_readably = true; }
    else if (kw_eq_cached(env, args[i], syms.k_array))
      { array = args[i + 1]; h_array = true; }
  }

  /* bind print variables, call prin1-to-string, restore */
  emacs_value o_esc = syms.v_nil, o_pretty = syms.v_nil, o_circle = syms.v_nil;
  emacs_value o_level = syms.v_nil, o_length = syms.v_nil, o_base = syms.v_nil;
  emacs_value o_radix = syms.v_nil, o_case = syms.v_nil;
  emacs_value o_gensym = syms.v_nil, o_array = syms.v_nil;
  emacs_value o_readably = syms.v_nil;

  if (h_esc)     { emacs_value sn = eintern(env, "print-escape");
                   o_esc = write_bind_var(env, sn, esc); }
  if (h_pretty)  { emacs_value sn = eintern(env, "print-pretty");
                   o_pretty = write_bind_var(env, sn, pretty); }
  if (h_circle)  { emacs_value sn = eintern(env, "print-circle");
                   o_circle = write_bind_var(env, sn, circle); }
  if (h_level)   { emacs_value sn = eintern(env, "print-level");
                   o_level = write_bind_var(env, sn, level); }
  if (h_length)  { emacs_value sn = eintern(env, "print-length");
                   o_length = write_bind_var(env, sn, length); }
  if (h_base)    { emacs_value sn = eintern(env, "print-base");
                   o_base = write_bind_var(env, sn, base); }
  if (h_radix)   { emacs_value sn = eintern(env, "print-radix");
                   o_radix = write_bind_var(env, sn, radix); }
  if (h_case)    { emacs_value sn = eintern(env, "print-case");
                   o_case = write_bind_var(env, sn, kase); }
  if (h_gensym)  { emacs_value sn = eintern(env, "print-gensym");
                   o_gensym = write_bind_var(env, sn, gensym); }
  if (h_readably){ emacs_value sn = eintern(env, "print-readably");
                   o_readably = write_bind_var(env, sn, readably); }
  if (h_array)   { emacs_value sn = eintern(env, "print-array");
                   o_array = write_bind_var(env, sn, array); }

  emacs_value str = efuncall_cached(env, syms.fn_prin1_to_string, 1, &obj);

  if (h_esc)     { emacs_value sn = eintern(env, "print-escape");
                   write_restore_var(env, sn, o_esc); }
  if (h_pretty)  { emacs_value sn = eintern(env, "print-pretty");
                   write_restore_var(env, sn, o_pretty); }
  if (h_circle)  { emacs_value sn = eintern(env, "print-circle");
                   write_restore_var(env, sn, o_circle); }
  if (h_level)   { emacs_value sn = eintern(env, "print-level");
                   write_restore_var(env, sn, o_level); }
  if (h_length)  { emacs_value sn = eintern(env, "print-length");
                   write_restore_var(env, sn, o_length); }
  if (h_base)    { emacs_value sn = eintern(env, "print-base");
                   write_restore_var(env, sn, o_base); }
  if (h_radix)   { emacs_value sn = eintern(env, "print-radix");
                   write_restore_var(env, sn, o_radix); }
  if (h_case)    { emacs_value sn = eintern(env, "print-case");
                   write_restore_var(env, sn, o_case); }
  if (h_gensym)  { emacs_value sn = eintern(env, "print-gensym");
                   write_restore_var(env, sn, o_gensym); }
  if (h_readably){ emacs_value sn = eintern(env, "print-readably");
                   write_restore_var(env, sn, o_readably); }
  if (h_array)   { emacs_value sn = eintern(env, "print-array");
                   write_restore_var(env, sn, o_array); }

  if (s && stream_is_open(s))
    stream_write_string(env, s, str);

  return obj;
}

/* ── cl-prin1 ─────────────────────────────────────────────────────────── */

emacs_value Fprin1(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                   void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-prin1 requires 1 argument");
    return syms.v_nil;
  }

  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-prin1: stream is closed");
    return syms.v_nil;
  }

  emacs_value str = efuncall_cached(env, syms.fn_prin1_to_string, 1, args);
  stream_write_string(env, s, str);
  return args[0];
}

/* ── cl-princ ─────────────────────────────────────────────────────────── */

emacs_value Fprinc(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                   void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-princ requires 1 argument");
    return syms.v_nil;
  }

  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-princ: stream is closed");
    return syms.v_nil;
  }

  emacs_value str = efuncall_cached(env, syms.fn_prin1_to_string, 1, args);
  if (env->eq(env, env->type_of(env, args[0]), syms.v_string)) {
    ptrdiff_t slen = 0;
    char *cstr = estring_alloc(env, str, &slen);
    if (cstr && slen >= 2 && cstr[0] == '"' && cstr[slen - 1] == '"') {
      stream_write(s, cstr + 1, slen - 2);
      free(cstr);
      return args[0];
    }
    free(cstr);
  }
  {
    ptrdiff_t slen = 0;
    char *cstr = estring_alloc(env, str, &slen);
    if (cstr) {
      stream_write(s, cstr, slen);
      free(cstr);
    }
  }
  return args[0];
}

/* ── cl-print ─────────────────────────────────────────────────────────── */

emacs_value Fprint(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  /* CL: newline + prin1 + space */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  if (s && stream_is_open(s)) {
    unsigned char nl = '\n';
    stream_write(s, &nl, 1);
  }
  emacs_value result = Fprin1(env, nargs, args, data);
  if (s && stream_is_open(s)) {
    unsigned char sp = ' ';
    stream_write(s, &sp, 1);
  }
  return result;
}

/* ── cl-prin1-to-string, cl-princ-to-string ──────────────────────────── */

emacs_value Fprin1_to_string(emacs_env *env, ptrdiff_t nargs,
                              emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) { signal_error(env, "Requires 1 argument"); return syms.v_nil; }
  return efuncall_cached(env, syms.fn_prin1_to_string, 1, args);
}

emacs_value Fprinc_to_string(emacs_env *env, ptrdiff_t nargs,
                              emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) { signal_error(env, "Requires 1 argument"); return syms.v_nil; }
  emacs_value str = efuncall_cached(env, syms.fn_prin1_to_string, 1, args);
  if (env->eq(env, env->type_of(env, args[0]), syms.v_string)) {
    ptrdiff_t slen = 0;
    char *cstr = estring_alloc(env, str, &slen);
    if (cstr && slen >= 2 && cstr[0] == '"' && cstr[slen - 1] == '"') {
      emacs_value raw = env->make_string(env, cstr + 1, slen - 2);
      free(cstr);
      return raw;
    }
    free(cstr);
  }
  return str;
}
