#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ── cl-open ─────────────────────────────────────────────────────────────
   (cl-open filename &key DIRECTION ELEMENT-TYPE
             IF-EXISTS IF-DOES-NOT-EXIST EXTERNAL-FORMAT) */

/* Direction codes stored in open_options_t.dir */
enum { OPEN_INPUT = 0, OPEN_OUTPUT = 1, OPEN_IO = 2, OPEN_PROBE = 3 };

/* Values for the if-exists / if-does-not-exist actions.  ACTION_DEFAULT
   means "unspecified", which falls back to the CL defaults. */
enum { ACTION_DEFAULT = -1, ACTION_NIL = 0, ACTION_SUPERSEDE = 1,
       ACTION_APPEND = 2, ACTION_ERROR = 3, ACTION_OVERWRITE = 4 };

typedef struct {
  int         dir;                   /* OPEN_* */
  unsigned    flags;                 /* CL_STREAM_FLAG_* */
  int         oflags;                /* base open(2) flags for the direction */
  int         if_exists;             /* ACTION_* */
  int         if_does_not_exist;     /* ACTION_* */
  emacs_value external_format;
  bool        have_external_format;
} open_options_t;

static int if_exists_action(emacs_env *env, emacs_value v) {
  if (kw_eq_cached(env, v, syms.k_supersede)) return ACTION_SUPERSEDE;
  if (kw_eq_cached(env, v, syms.k_append))    return ACTION_APPEND;
  if (kw_eq_cached(env, v, syms.k_error))     return ACTION_ERROR;
  if (kw_eq_cached(env, v, syms.k_overwrite)) return ACTION_OVERWRITE;
  if (env->eq(env, v, syms.v_nil))            return ACTION_NIL;
  return ACTION_DEFAULT;
}

static int if_does_not_exist_action(emacs_env *env, emacs_value v) {
  if (kw_eq_cached(env, v, syms.k_error)) return ACTION_ERROR;
  if (env->eq(env, v, syms.v_nil))        return ACTION_NIL;
  return ACTION_DEFAULT;
}

static void apply_direction(emacs_env *env, emacs_value d, open_options_t *o) {
  if (kw_eq_cached(env, d, syms.d_input)) {
    o->dir = OPEN_INPUT;
    o->flags = (o->flags & ~CL_STREAM_FLAG_OUTPUT) | CL_STREAM_FLAG_INPUT;
    o->oflags = O_RDONLY;
  } else if (kw_eq_cached(env, d, syms.d_output)) {
    o->dir = OPEN_OUTPUT;
    o->flags = CL_STREAM_FLAG_OUTPUT;
    o->oflags = O_WRONLY;
  } else if (kw_eq_cached(env, d, syms.d_io)) {
    o->dir = OPEN_IO;
    o->flags = CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
    o->oflags = O_RDWR;
  } else if (kw_eq_cached(env, d, syms.d_probe)) {
    o->dir = OPEN_PROBE;
    o->flags = CL_STREAM_FLAG_INPUT;
    o->oflags = O_RDONLY;
  }
}

static void parse_open_options(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, open_options_t *o) {
  o->dir = OPEN_INPUT;
  o->flags = CL_STREAM_FLAG_INPUT;
  o->oflags = O_RDONLY;
  o->if_exists = ACTION_DEFAULT;
  o->if_does_not_exist = ACTION_DEFAULT;
  o->external_format = syms.v_nil;
  o->have_external_format = false;

  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    emacs_value key = args[i], val = args[i + 1];
    if (kw_eq_cached(env, key, syms.k_direction)) {
      apply_direction(env, val, o);
    } else if (kw_eq_cached(env, key, syms.k_element_type)) {
      if (env->is_not_nil(env, val) && !env->eq(env, val, syms.v_character))
        o->flags |= CL_STREAM_FLAG_BINARY;
    } else if (kw_eq_cached(env, key, syms.k_if_exists)) {
      o->if_exists = if_exists_action(env, val);
    } else if (kw_eq_cached(env, key, syms.k_if_does_not_exist)) {
      o->if_does_not_exist = if_does_not_exist_action(env, val);
    } else if (kw_eq_cached(env, key, syms.k_external_format)) {
      o->external_format = val;
      o->have_external_format = true;
    }
  }
}

/* Fold :if-exists / :if-does-not-exist into the final open(2) flags.
   Returns false when the file should not be opened (the NIL cases). */
static bool resolve_open_flags(const open_options_t *o, const char *filename,
                               int *oflags_out) {
  int oflags = o->oflags;

  switch (o->if_exists) {
  case ACTION_NIL:
    /* output/io: give up if the file already exists */
    if (o->dir == OPEN_OUTPUT || o->dir == OPEN_IO) {
      struct stat st;
      if (stat(filename, &st) == 0) return false;
    }
    break;
  case ACTION_SUPERSEDE: oflags |= O_CREAT | O_TRUNC; break;
  case ACTION_APPEND:    oflags |= O_CREAT | O_APPEND; break;
  case ACTION_ERROR:     oflags |= O_EXCL; break;
  case ACTION_OVERWRITE: oflags |= O_CREAT; break;
  default:
    /* CL default: :output supersedes; :input/:io fall through to open(2) */
    if (o->dir == OPEN_OUTPUT) oflags |= O_CREAT | O_TRUNC;
    break;
  }

  switch (o->if_does_not_exist) {
  case ACTION_NIL: {
    /* give up if the file is missing */
    struct stat st;
    if (stat(filename, &st) < 0 && errno == ENOENT) return false;
    break;
  }
  case ACTION_ERROR:
    break;  /* leave it to open(2) to fail */
  default:
    /* CL default: :output/:io create the file */
    if (o->dir == OPEN_OUTPUT || o->dir == OPEN_IO) oflags |= O_CREAT;
    break;
  }

  *oflags_out = oflags;
  return true;
}

/* :probe — stat only; return a closed stream, or nil if unreadable. */
static emacs_value open_probe(emacs_env *env, const char *filename,
                              const open_options_t *o) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0) return syms.v_nil;
  cl_stream_t *s = stream_create_fd(fd, true, CL_STREAM_FLAG_INPUT);
  if (!s) { close(fd); return syms.v_nil; }
  stream_close(s);
  if (o->have_external_format)
    s->external_format = env->make_global_ref(env, o->external_format);
  return euser_ptr_stream_make(env, s);
}

/* Prefer a zero-copy mmap stream for plain readable regular files. */
static cl_stream_t *make_file_stream(int fd, int oflags,
                                     const open_options_t *o) {
  if (oflags == O_RDONLY && !(o->flags & CL_STREAM_FLAG_BINARY)) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
      return stream_create_mmap(fd, true);
  }
  return stream_create_fd(fd, true, o->flags);
}

emacs_value Fopen(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                  void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-open requires at least 1 argument (filename)");
    return syms.v_nil;
  }

  char *filename = estrdup(env, args[0]);
  if (!filename) {
    signal_error(env, "cl-open: invalid filename");
    return syms.v_nil;
  }

  open_options_t o;
  parse_open_options(env, nargs, args, &o);

  if (o.dir == OPEN_PROBE) {
    emacs_value result = open_probe(env, filename, &o);
    free(filename);
    return result;
  }

  int oflags;
  if (!resolve_open_flags(&o, filename, &oflags)) {
    free(filename);
    return syms.v_nil;
  }

  int fd = open(filename, oflags, 0644);
  int saved_errno = errno;
  free(filename);

  if (fd < 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Cannot open file: %s", strerror(saved_errno));
    signal_error(env, buf);
    return syms.v_nil;
  }

  cl_stream_t *s = make_file_stream(fd, oflags, &o);
  if (!s) {
    close(fd);
    signal_error(env, "Cannot create stream");
    return syms.v_nil;
  }

  return euser_ptr_stream_make(env, s);
}

/* ── cl-close ───────────────────────────────────────────────────────────
   (cl-close stream &key abort) */

emacs_value Fclose(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-close requires 1 argument (stream)");
    return syms.v_nil;
  }

  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) {
    signal_error(env, "cl-close: invalid stream");
    return syms.v_nil;
  }

  /* flush pending buffer-stream content before close */
  extern void bos_flush_pending(emacs_env *env, cl_stream_t *s);
  extern const cl_stream_ops_t buffer_output_ops;
  extern const cl_stream_ops_t buffer_io_ops;
  if (s->ops == &buffer_output_ops || s->ops == &buffer_io_ops) {
    /* for buffer_io, flush the internal strbuf via a dedicated wrapper */
    extern void bds_flush_pending(emacs_env *env, cl_stream_t *s);
    if (s->ops == &buffer_io_ops)
      bds_flush_pending(env, s);
    else
      bos_flush_pending(env, s);
  }

  /* :abort — discard output without flushing */
  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_abort) &&
        env->is_not_nil(env, args[i + 1])) {
      pthread_mutex_lock(&s->mutex);
      if (s->ops && s->ops->clear_output)
        s->ops->clear_output(s);
      pthread_mutex_unlock(&s->mutex);
    }
  }

  stream_close(s);
  return syms.v_t;
}

/* ── stream predicates ────────────────────────────────────────────────── */

emacs_value Fstreamp(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                      void *data) {
  (void)data; (void)nargs;
  if (env->is_not_nil(env, args[0]) && env->eq(env,
        env->type_of(env, args[0]), eintern(env, "user-ptr")))
    return syms.v_t;
  return syms.v_nil;
}

emacs_value Fopen_stream_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return (s && stream_is_open(s)) ? syms.v_t : syms.v_nil;
}

emacs_value Finput_stream_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                             void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return (s && stream_is_input(s)) ? syms.v_t : syms.v_nil;
}

emacs_value Foutput_stream_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                              void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return (s && stream_is_output(s)) ? syms.v_t : syms.v_nil;
}

emacs_value Fstream_element_type(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) {
    signal_error(env, "Invalid stream");
    return syms.v_nil;
  }
  if (s->flags & CL_STREAM_FLAG_BINARY) {
    emacs_value args2[2];
    args2[0] = syms.v_unsigned_byte_8;
    args2[1] = eint(env, 8);
    return efuncall_cached(env, syms.fn_list, 2, args2);
  }
  return eintern(env, "character");
}

/* ── file type predicates ──────────────────────────────────────────── */

emacs_value Ffile_regular_p(emacs_env *env, ptrdiff_t nargs,
                             emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return stream_is_regular(s) ? syms.v_t : syms.v_nil;
}

emacs_value Ffile_tty_p(emacs_env *env, ptrdiff_t nargs,
                         emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return stream_is_tty(s) ? syms.v_t : syms.v_nil;
}

/* ── cl-file-position ────────────────────────────────────────────────── */

emacs_value Ffile_position(emacs_env *env, ptrdiff_t nargs,
                            emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-file-position requires 1 argument");
    return syms.v_nil;
  }

  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "Stream is closed");
    return syms.v_nil;
  }

  if (nargs >= 2 && env->is_not_nil(env, args[1])) {
    ptrdiff_t pos = (ptrdiff_t)eint_extract(env, args[1]);
    if (pos < 0) {
      signal_error(env, "cl-file-position: position must be non-negative");
      return syms.v_nil;
    }
    return stream_set_file_position(s, pos)
           ? syms.v_t : syms.v_nil;
  }

  ptrdiff_t pos = stream_file_position(s);
  if (pos < 0) return syms.v_nil;
  return eint(env, (intmax_t)pos);
}

emacs_value Ffile_length(emacs_env *env, ptrdiff_t nargs,
                          emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) {
    signal_error(env, "Invalid stream");
    return syms.v_nil;
  }
  ptrdiff_t len = stream_file_length(s);
  if (len < 0) return syms.v_nil;
  return eint(env, (intmax_t)len);
}

emacs_value Finteractive_stream_p(emacs_env *env, ptrdiff_t nargs,
                                   emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return stream_is_tty(s) ? syms.v_t : syms.v_nil;
}

emacs_value Fstream_external_format(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) { signal_error(env, "Invalid stream"); return syms.v_nil; }
  return s->external_format ? s->external_format : eintern(env, ":default");
}

emacs_value Ffile_string_length(emacs_env *env, ptrdiff_t nargs,
                                 emacs_value *args, void *data) {
  (void)data;
  if (nargs < 2) {
    signal_error(env, "cl-file-string-length requires 2 arguments");
    return syms.v_nil;
  }
  /* return nil if stream cannot determine encoding */
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) { signal_error(env, "Invalid stream"); return syms.v_nil; }
  (void)s;

  /* for character streams, length is printed repr byte count */
  emacs_value printed = efuncall_cached(env, syms.fn_prin1_to_string, 1, &args[1]);
  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, printed, NULL, &slen))
    return syms.v_nil;
  return eint(env, slen > 0 ? slen - 1 : 0);
}

emacs_value Fftruncate(emacs_env *env, ptrdiff_t nargs,
                        emacs_value *args, void *data) {
  (void)data;
  if (nargs < 2) {
    signal_error(env, "cl-ftruncate requires 2 arguments");
    return syms.v_nil;
  }
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "Stream is closed");
    return syms.v_nil;
  }
  ptrdiff_t len = eint_extract(env, args[1]);
  if (len < 0) {
    signal_error(env, "cl-ftruncate: length must be non-negative");
    return syms.v_nil;
  }
  return stream_truncate(s, len) ? syms.v_t : syms.v_nil;
}
