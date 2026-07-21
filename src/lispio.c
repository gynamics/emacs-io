#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── helpers ──────────────────────────────────────────────────────────── */

static cl_stream_t *get_lisp_input_stream(emacs_env *env, emacs_value *args,
                                           ptrdiff_t nargs) {
  if (nargs >= 1 && env->is_not_nil(env, args[0]))
    return euser_ptr_stream_get(env, args[0]);
  emacs_value sv = efuncall(env, "symbol-value", 1,
                             (emacs_value[]){ eintern(env, "*standard-input*") });
  return euser_ptr_stream_get(env, sv);
}

static cl_stream_t *get_lisp_output_stream(emacs_env *env, emacs_value *args,
                                            ptrdiff_t nargs, int pos) {
  if (pos < nargs && env->is_not_nil(env, args[pos]))
    return euser_ptr_stream_get(env, args[pos]);
  emacs_value sv = efuncall(env, "symbol-value", 1,
                             (emacs_value[]){ eintern(env, "*standard-output*") });
  return euser_ptr_stream_get(env, sv);
}

/* ── try-read-from-buffer ──────────────────────────────────────────────
   Attempt to call read-from-string on sb->data.
   On success: store the parsed value in *result, set *consumed to the
   number of bytes consumed, and return true.
   On end-of-file (incomplete form): return false.
   On any other error: clear the exit, return false (caller should retry
   or propagate). */

static bool try_read_from_buffer(emacs_env *env, strbuf_t *sb,
                                  emacs_value *result, ptrdiff_t *consumed) {
  emacs_value estr = env->make_string(env, sb->data, (ptrdiff_t)sb->len);
  emacs_value read_args[2];
  read_args[0] = estr;
  read_args[1] = eint(env, 0);

  emacs_value raw = env->funcall(env, eintern(env, "read-from-string"),
                                  2, read_args);

  enum emacs_funcall_exit exit = env->non_local_exit_check(env);
  if (exit == emacs_funcall_exit_return) {
    /* Success: raw = (VALUE . POSITION).  Extract car and consumed count. */
    emacs_value pair = raw;
    *result = efuncall(env, "car", 1, &pair);
    emacs_value pos  = efuncall(env, "cdr", 1, &pair);
    *consumed = eint_extract(env, pos);
    return true;
  }

  /* read-from-string signalled an error.  Detect end-of-file (incomplete form). */
  emacs_value sym = eintern(env, "nil");
  emacs_value data = eintern(env, "nil");
  env->non_local_exit_get(env, &sym, &data);
  bool is_eof = env->eq(env, sym, eintern(env, "end-of-file"));
  env->non_local_exit_clear(env);

  if (is_eof)
    return false;  /* incomplete — need more data */

  /* Some other read error (invalid syntax, etc.) — re-signal it. */
  env->non_local_exit_signal(env, sym, data);
  *consumed = 0;
  return false;
}

/* ── cl-read ────────────────────────────────────────────────────────────
   Reads characters from STREAM, feeding them incrementally to Emacs'
   read-from-string.  Emacs handles all form parsing; we are just a
   transport layer.  Unconsumed bytes after a successful parse are
   pushed back into the stream's ring buffer. */

emacs_value Fread(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                  void *data) {
  (void)data;
  /* (cl-read &optional stream eof-error-p eof-value recursive-p) */
  cl_stream_t *s = get_lisp_input_stream(env, args, nargs);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-read: stream is closed");
    return eintern(env, "nil");
  }

  strbuf_t *sb = strbuf_create(256);
  if (!sb) { signal_error(env, "Memory allocation failed"); return eintern(env, "nil"); }

  emacs_value result = eintern(env, "nil");
  ptrdiff_t consumed = 0;
  bool parsed = false;
  unsigned char chunk[4096];

  while (!parsed) {
    /* try with what we have */
    if (sb->len > 0) {
      if (try_read_from_buffer(env, sb, &result, &consumed)) {
        parsed = true;
        break;
      }
      /* if a non-eof error was re-signalled, bail out */
      if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
        strbuf_free(sb);
        return eintern(env, "nil");
      }
    }

    /* read more data from stream */
    ptrdiff_t r = stream_read(s, chunk, sizeof(chunk));
    if (r <= 0) {
      /* EOF — strip trailing whitespace and try one last time */
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
            return eintern(env, "nil");
          }
        }
      }
      /* nothing parsed — signal EOF or return eof-value */
      strbuf_free(sb);
      if (nargs >= 2 && !env->is_not_nil(env, args[1]))
        return (nargs >= 3) ? args[2] : eintern(env, "nil");
      signal_end_of_file(env, s);
      return eintern(env, "nil");
    }

    strbuf_append(sb, (const char *)chunk, r);
  }

  /* parsed successfully — push back unconsumed bytes */
  if (consumed < (ptrdiff_t)sb->len) {
    /* push in forward order; unread enqueues at tail, read dequeues from head (FIFO) */
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
  /* (cl-read-from-string string &optional eof-error-p eof-value
                           &key start end preserve-whitespace)
     Just delegate to Emacs read-from-string. */
  if (nargs < 1) {
    signal_error(env, "cl-read-from-string requires 1 argument");
    return eintern(env, "nil");
  }

  emacs_value read_args[2];
  read_args[0] = args[0];
  read_args[1] = eint(env, 0);

  /* parse &key start */
  for (ptrdiff_t i = 2; i < nargs - 1; i += 2) {
    if (kw_eq(env, args[i], ":start"))
      read_args[1] = args[i + 1];
  }

  emacs_value result = efuncall(env, "read-from-string", 2, read_args);
  return efuncall(env, "car", 1, &result);
}

/* ── cl-read-preserving-whitespace ──────────────────────────────────── */

emacs_value Fread_preserving_whitespace(emacs_env *env, ptrdiff_t nargs,
                                        emacs_value *args, void *data) {
  (void)data;
  /* Same as cl-read for now */
  return Fread(env, nargs, args, data);
}

/* ── cl-write ───────────────────────────────────────────────────────────
   (cl-write object &key STREAM ESCAPE RADIX BASE CIRCLE PRETTY LEVEL
            LENGTH CASE GENSYM ARRAY READABLY RIGHT-MARGIN MISER-WIDTH
            PP-DISPATCH LINES) */

emacs_value Fwrite(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-write requires 1 argument");
    return eintern(env, "nil");
  }

  emacs_value obj = args[0];
  cl_stream_t *s = NULL;

  /* find :stream key */
  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq(env, args[i], ":stream")) {
      s = euser_ptr_stream_get(env, args[i + 1]);
      break;
    }
  }

  /* Use prin1-to-string + write to stream */
  if (s && stream_is_open(s)) {
    emacs_value str = efuncall(env, "prin1-to-string", 1, &obj);
    ptrdiff_t slen = 0;
    char *cstr = estring_alloc(env, str, &slen);
    if (cstr) {
      stream_write(s, cstr, slen);
      free(cstr);
    }
  }

  return obj;
}

/* ── cl-prin1 ─────────────────────────────────────────────────────────── */

emacs_value Fprin1(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                   void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-prin1 requires 1 argument");
    return eintern(env, "nil");
  }

  cl_stream_t *s = get_lisp_output_stream(env, args, nargs, 1);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-prin1: stream is closed");
    return eintern(env, "nil");
  }

  emacs_value str = efuncall(env, "prin1-to-string", 1, args);
  ptrdiff_t slen = 0;
  char *cstr = estring_alloc(env, str, &slen);
  if (cstr) {
    stream_write(s, cstr, slen);
    free(cstr);
  }
  return args[0];
}

/* ── cl-princ ─────────────────────────────────────────────────────────── */

emacs_value Fprinc(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                   void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-princ requires 1 argument");
    return eintern(env, "nil");
  }

  cl_stream_t *s = get_lisp_output_stream(env, args, nargs, 1);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-princ: stream is closed");
    return eintern(env, "nil");
  }

  emacs_value str = efuncall(env, "prin1-to-string", 1, args);
  /* for strings: strip quotes for princ */
  if (env->eq(env, env->type_of(env, args[0]),
              eintern(env, "string"))) {
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
  /* cl-print = print with newline before */
  cl_stream_t *s = get_lisp_output_stream(env, args, nargs, 1);
  if (s && stream_is_open(s)) {
    unsigned char nl = '\n';
    stream_write(s, &nl, 1);
  }
  return Fprin1(env, nargs, args, data);
}

/* ── cl-prin1-to-string, cl-princ-to-string ──────────────────────────── */

emacs_value Fprin1_to_string(emacs_env *env, ptrdiff_t nargs,
                              emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) { signal_error(env, "Requires 1 argument"); return eintern(env, "nil"); }
  return efuncall(env, "prin1-to-string", 1, args);
}

emacs_value Fprinc_to_string(emacs_env *env, ptrdiff_t nargs,
                              emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) { signal_error(env, "Requires 1 argument"); return eintern(env, "nil"); }
  emacs_value str = efuncall(env, "prin1-to-string", 1, args);
  /* for strings: remove surrounding quotes that prin1 added */
  if (env->eq(env, env->type_of(env, args[0]),
              eintern(env, "string"))) {
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
