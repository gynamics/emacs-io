#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include "subrs.h"
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

/* Drop trailing whitespace from SB (used when a form is unfinished at
   EOF, e.g. "(1 2 " should still parse as the list (1 2)). */
static void trim_trailing_whitespace(strbuf_t *sb) {
  while (sb->len > 0) {
    char c = sb->data[sb->len - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
      sb->data[--sb->len] = '\0';
    else
      break;
  }
}

/* Read chunks from S into SB until one complete form can be parsed.
   Returns true and stores the value and its byte length in *result and
   *consumed.  Returns false when EOF is reached first; a pending non-local
   exit (a genuine reader error) is left for the caller to notice. */
static bool read_object_from_stream(emacs_env *env, cl_stream_t *s,
                                    strbuf_t *sb, emacs_value *result,
                                    ptrdiff_t *consumed) {
  unsigned char chunk[4096];
  for (;;) {
    if (sb->len > 0) {
      if (try_read_from_buffer(env, sb, result, consumed))
        return true;
      if (env->non_local_exit_check(env) != emacs_funcall_exit_return)
        return false;
    }

    ptrdiff_t r = stream_read(s, chunk, sizeof(chunk));
    if (r <= 0) {
      /* At EOF, strip trailing whitespace and try once more. */
      trim_trailing_whitespace(sb);
      if (sb->len > 0 && try_read_from_buffer(env, sb, result, consumed))
        return true;
      return false;
    }

    strbuf_append(sb, (const char *)chunk, r);
  }
}

static bool read_ws_byte(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Shared by cl-read and cl-read-preserving-whitespace.  With PRESERVE_WS
   false, the single whitespace character that delimits the object is
   discarded, as CL's reader does; with true it is pushed back. */
static emacs_value read_form(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                             bool preserve_ws) {
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-read: stream is closed");
    return syms.v_nil;
  }

  strbuf_t *sb = strbuf_create(256);
  if (!sb) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  emacs_value result = syms.v_nil;
  ptrdiff_t consumed = 0;
  bool parsed = read_object_from_stream(env, s, sb, &result, &consumed);

  if (!parsed) {
    bool reader_error =
      env->non_local_exit_check(env) != emacs_funcall_exit_return;
    strbuf_free(sb);
    if (reader_error) return syms.v_nil;   /* propagate the pending error */
    return eof_result(env, s, nargs, args, 1, 2);
  }

  /* Push back the unconsumed bytes; when not preserving whitespace, drop
     the one delimiting whitespace character first. */
  ptrdiff_t first = consumed;
  if (!preserve_ws && first < (ptrdiff_t)sb->len &&
      read_ws_byte(sb->data[first]))
    first++;
  for (ptrdiff_t i = first; i < (ptrdiff_t)sb->len; i++)
    stream_unread(s, (unsigned char)sb->data[i]);

  strbuf_free(sb);
  return result;
}

emacs_value Fread(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                  void *data) {
  (void)data;
  return read_form(env, nargs, args, false);
}

emacs_value Fread_preserving_whitespace(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value *args, void *data) {
  (void)data;
  return read_form(env, nargs, args, true);
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
    signal_error_type(env, "end-of-file", "End of file on string");
    return syms.v_nil;
  }

  emacs_value car_val = efuncall_cached(env, syms.fn_car, 1, &raw);
  ptrdiff_t pos = eint_extract(env, efuncall_cached(env, syms.fn_cdr, 1, &raw));
  if (pos == 0 && end > start) {
    free(str);
    if (!eoferr) return eofval;
    signal_error_type(env, "end-of-file", "End of file on string");
    return syms.v_nil;
  }

  /* Unless :preserve-whitespace, consume the single whitespace character
     that delimits the object (as CL's reader does), so POSITION matches
     CLHS: (read-from-string " 1 3 5" t nil :start 2) => 3, 5. */
  if (!preserve_ws && pos < end - start &&
      read_ws_byte(str[start + pos]))
    pos++;
  free(str);

  emacs_value cons_args[2];
  cons_args[0] = car_val;
  cons_args[1] = eint(env, start + pos);
  return efuncall_cached(env, syms.fn_cons, 2, cons_args);
}

/* ── cl-write ─────────────────────────────────────────────────────────── */

/* CL print-control keywords cl-write accepts, each paired with the Emacs
   variable it should temporarily bind during printing.  :escape and
   :readably have no Emacs variable and are handled separately below. */
/* CL print-control keywords whose Emacs variable actually exists (so it can
   be dynamically bound).  The others (:base :radix :pretty :case :array
   :readably :lines :miser-width :pprint-dispatch :right-margin) have no
   Emacs counterpart and are accepted and ignored -- except :escape and
   :pretty, handled specially below. */
#define N_PRINT_CONTROLS 4

emacs_value Fwrite(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-write requires 1 argument");
    return syms.v_nil;
  }

  emacs_value obj = args[0];
  cl_stream_t *s = NULL;
  bool escape_present = false, escape_value = false;
  bool pretty_present = false, pretty_value = false;

  const struct { emacs_value key; const char *var; } controls[N_PRINT_CONTROLS] = {
    { syms.k_circle, "print-circle" },
    { syms.k_level,  "print-level"  },
    { syms.k_length, "print-length" },
    { syms.k_gensym, "print-gensym" },
  };

  emacs_value values[N_PRINT_CONTROLS];
  emacs_value saved[N_PRINT_CONTROLS];
  bool present[N_PRINT_CONTROLS];
  for (int j = 0; j < N_PRINT_CONTROLS; j++) present[j] = false;

  /* collect the supplied options */
  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    emacs_value key = args[i];
    if (kw_eq_cached(env, key, syms.k_stream)) {
      s = euser_ptr_stream_get(env, args[i + 1]);
      continue;
    }
    if (kw_eq_cached(env, key, syms.k_escape)) {
      escape_present = true;
      escape_value = env->is_not_nil(env, args[i + 1]);
      continue;
    }
    if (kw_eq_cached(env, key, syms.k_pretty)) {
      pretty_present = true;
      pretty_value = env->is_not_nil(env, args[i + 1]);
      continue;
    }
    for (int j = 0; j < N_PRINT_CONTROLS; j++) {
      if (kw_eq_cached(env, key, controls[j].key)) {
        values[j] = args[i + 1];
        present[j] = true;
        break;
      }
    }
  }

  /* bind the supplied variables, remembering their previous values */
  for (int j = 0; j < N_PRINT_CONTROLS; j++) {
    if (!present[j]) continue;
    emacs_value var = eintern(env, controls[j].var);
    saved[j] = efuncall_cached(env, syms.fn_symbol_value, 1, &var);
    emacs_value a[2] = { var, values[j] };
    efuncall_cached(env, syms.fn_set, 2, a);
  }

  /* Emacs has no `print-escape'; emulate it via prin1 (escaped) vs the
     princ-style `(format "%s" obj)' (unescaped).  :pretty routes through
     Emacs' pretty-printer. */
  emacs_value str;
  if (escape_present && !escape_value) {
    emacs_value fmt = estring(env, "%s");
    emacs_value fa[2] = { fmt, obj };
    str = efuncall(env, "format", 2, fa);
  } else if (pretty_present && pretty_value) {
    str = efuncall(env, "pp-to-string", 1, &obj);
  } else {
    str = efuncall_cached(env, syms.fn_prin1_to_string, 1, &obj);
  }

  /* restore */
  for (int j = 0; j < N_PRINT_CONTROLS; j++) {
    if (!present[j]) continue;
    emacs_value var = eintern(env, controls[j].var);
    emacs_value a[2] = { var, saved[j] };
    efuncall_cached(env, syms.fn_set, 2, a);
  }

  /* CL: :stream defaults to *standard-output* */
  if (!s) {
    emacs_value sout = efuncall_cached(env, syms.fn_symbol_value, 1,
                                       (emacs_value[]){ syms.var_sout });
    s = euser_ptr_stream_get(env, sout);
  }
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-write: stream is closed");
    return syms.v_nil;
  }

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

emacs_value Fwrite_to_string(emacs_env *env, ptrdiff_t nargs,
                              emacs_value *args, void *data) {
  (void)data;
  /* (cl-write-to-string object &rest args) — create temp stream,
     delegate to Fwrite with :stream inserted, return string */
  if (nargs < 1) {
    signal_error(env, "cl-write-to-string requires 1 argument");
    return syms.v_nil;
  }

  cl_stream_t *s = stream_create_string_output(0);
  if (!s) { signal_error(env, "Cannot create string stream"); return syms.v_nil; }

  /* build new arg list: [object, :stream, stream-user-ptr, ...rest] */
  emacs_value new_args[32];
  new_args[0] = args[0];
  new_args[1] = syms.k_stream;
  new_args[2] = euser_ptr_stream_make(env, s);
  ptrdiff_t k = 3;
  for (ptrdiff_t i = 1; i < nargs && k < 30; i++, k++)
    new_args[k] = args[i];

  extern emacs_value Fwrite(emacs_env *env, ptrdiff_t nargs,
                             emacs_value *args, void *data);
  Fwrite(env, k, new_args, data);

  emacs_value result = str_output_to_string(env, s);
  /* stream lifetime managed by user-ptr finalizer — do not free here */
  return result;
}

/* ── cl-pprint ─────────────────────────────────────────────────────────── */

emacs_value Fpprint(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                     void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-pprint requires 1 argument");
    return syms.v_nil;
  }

  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-pprint: stream is closed");
    return syms.v_nil;
  }

  /* use Emacs pp-to-string, then write to stream.
     CL: pprint returns no values; a subr cannot, so return nil. */
  emacs_value pp_str = efuncall(env, "pp-to-string", 1, args);
  stream_write_string(env, s, pp_str);
  return syms.v_nil;
}
