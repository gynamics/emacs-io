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

/* ── form-parsing state machine ─────────────────────────────────────────
   Track parens, strings, comments to know when we have a complete form.
   A form is complete when:
     - depth == 0 after processing ''
     - we have a top-level atom followed by a delimiter
*/

typedef enum {
  FS_TOP,              /* at top level, between forms */
  FS_IN_ATOM,          /* inside a symbol/number */
  FS_AFTER_ATOM_NEG,   /* just saw '-' that might start negative number */
  FS_IN_STRING,        /* inside "..." */
  FS_AFTER_BACKSLASH,  /* inside string, just saw \ */
  FS_IN_COMMENT,       /* inside ; comment */
  FS_AFTER_SHARP,      /* just saw # */
  FS_AFTER_QUOTE,      /* just saw ' */
} form_state_t;

static bool is_delimiter(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_atom_start(char c) {
  return c != '(' && c != ')' && c != '"' && c != ';'
         && c != '#' && c != '\'' && !is_delimiter(c);
}

static bool form_is_complete(const char *text, ptrdiff_t len) {
  int depth = 0;
  form_state_t state = FS_TOP;

  for (ptrdiff_t i = 0; i < len; i++) {
    char c = text[i];

    switch (state) {
    case FS_TOP:
      if (c == '(') { depth++; }
      else if (c == ')') { depth--; if (depth < 0) return true; }
      else if (c == '"') { state = FS_IN_STRING; }
      else if (c == ';') { state = FS_IN_COMMENT; }
      else if (c == '#') { state = FS_AFTER_SHARP; }
      else if (c == '\'') { state = FS_AFTER_QUOTE; }
      else if (is_delimiter(c)) { /* skip whitespace */ }
      else if (c == '-') { state = FS_AFTER_ATOM_NEG; }
      else { state = FS_IN_ATOM; }
      break;

    case FS_IN_ATOM:
      if (is_delimiter(c) || c == '(' || c == ')' || c == ';'
          || c == '"' || c == '\'') {
        /* atom ends here; process the delimiter in FS_TOP */
        state = FS_TOP;
        i--; /* re-process this char as FS_TOP */
      }
      /* else continue in atom */
      break;

    case FS_AFTER_ATOM_NEG:
      /* '-' followed by digit = negative number, else standalone symbol */
      if (c >= '0' && c <= '9') { state = FS_IN_ATOM; }
      else if (is_delimiter(c) || c == '(' || c == ')' || c == ';'
               || c == '"' || c == '\'') {
        /* '-' as a standalone symbol ended */
        state = FS_TOP;
        i--;
      } else { state = FS_IN_ATOM; } /* '-abc' is a symbol */
      break;

    case FS_IN_STRING:
      if (c == '\\') { state = FS_AFTER_BACKSLASH; }
      else if (c == '"') { state = FS_TOP; }
      break;

    case FS_AFTER_BACKSLASH:
      state = FS_IN_STRING; /* skip the escaped char */
      break;

    case FS_IN_COMMENT:
      if (c == '\n') { state = FS_TOP; }
      break;

    case FS_AFTER_SHARP:
      if (c == '\'') { /* #'function */ depth++; state = FS_TOP; }
      else if (c == '(') { /* #(vector */ depth++; state = FS_TOP; }
      else if (c == '\\') { state = FS_IN_ATOM; } /* #\char */
      else if (is_atom_start(c)) { state = FS_IN_ATOM; } /* #:symbol, #0... */
      else { state = FS_TOP; i--; } /* # alone is weird, treat as atomic */
      break;

    case FS_AFTER_QUOTE:
      if (c == '(') { depth++; state = FS_TOP; } /* '( */
      else { state = FS_IN_ATOM; } /* 'symbol */
      break;
    }
  }

  return depth == 0 && state == FS_TOP;
}

/* Skip leading whitespace and return true if content left */
static bool skip_whitespace(strbuf_t *sb) {
  ptrdiff_t i = 0;
  while (i < (ptrdiff_t)sb->len &&
         (sb->data[i] == ' ' || sb->data[i] == '\t' ||
          sb->data[i] == '\n' || sb->data[i] == '\r' ||
          sb->data[i] == '\f')) i++;
  if (i > 0) {
    memmove(sb->data, sb->data + i, sb->len - (size_t)i);
    sb->len -= (size_t)i;
    sb->data[sb->len] = '\0';
  }
  return sb->len > 0;
}

/* ── cl-read ────────────────────────────────────────────────────────────
   Reads characters from stream, accumulates into a buffer, and when a
   complete Lisp form is found, delegates to Emacs read-from-string. */

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

  unsigned char ch;

  while (1) {
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) {
      /* EOF — if we have content, treat it as a complete form */
      if (sb->len > 0) {
        /* strip trailing whitespace */
        while (sb->len > 0 && is_delimiter(sb->data[sb->len - 1]))
          sb->data[--sb->len] = '\0';
        if (sb->len > 0) break;
      }
      /* no complete form — signal EOF */
      strbuf_free(sb);
      if (nargs >= 2 && !env->is_not_nil(env, args[1]))
        return (nargs >= 3) ? args[2] : eintern(env, "nil");
      signal_end_of_file(env, s);
      return eintern(env, "nil");
    }

    strbuf_append_char(sb, ch);

    if (form_is_complete(sb->data, (ptrdiff_t)sb->len)) {
      if (skip_whitespace(sb) && form_is_complete(sb->data, (ptrdiff_t)sb->len))
        break;
      if (sb->len > 0 && form_is_complete(sb->data, (ptrdiff_t)sb->len))
        break;
    }
  }

  /* Delegate to Emacs read-from-string */
  emacs_value estr = env->make_string(env, sb->data, (ptrdiff_t)sb->len);
  strbuf_free(sb);

  emacs_value read_args[2];
  read_args[0] = estr;
  read_args[1] = eint(env, 0);
  emacs_value result = efuncall(env, "read-from-string", 2, read_args);

  /* read-from-string returns (object . position), extract car */
  return efuncall(env, "car", 1, &result);
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
