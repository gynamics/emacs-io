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

  unsigned cl_flags = CL_STREAM_FLAG_INPUT;
  int oflags = O_RDONLY;
  int dir = 0;                   /* 0=input, 1=output, 2=io, 3=probe */
  int if_exists_action = -1;    /* -1=default, 0=nil, 1=supersede,
                                   2=append, 3=error, 4=overwrite */
  int if_does_not_exist_action = -1; /* -1=default, 0=nil, 1=error */

  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_direction)) {
      if (kw_eq_cached(env, args[i + 1], syms.d_input)) {
        dir = 0;
        cl_flags = (cl_flags & ~CL_STREAM_FLAG_OUTPUT) | CL_STREAM_FLAG_INPUT;
        oflags = O_RDONLY;
      } else if (kw_eq_cached(env, args[i + 1], syms.d_output)) {
        dir = 1;
        cl_flags = CL_STREAM_FLAG_OUTPUT;
        oflags = O_WRONLY;
      } else if (kw_eq_cached(env, args[i + 1], syms.d_io)) {
        dir = 2;
        cl_flags = CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
        oflags = O_RDWR;
      } else if (kw_eq_cached(env, args[i + 1], syms.d_probe)) {
        dir = 3;
        cl_flags = CL_STREAM_FLAG_INPUT;
      }
    } else if (kw_eq_cached(env, args[i], syms.k_element_type)) {
      if (env->is_not_nil(env, args[i + 1]) &&
          !env->eq(env, args[i + 1], syms.v_character))
        cl_flags |= CL_STREAM_FLAG_BINARY;
    } else if (kw_eq_cached(env, args[i], syms.k_if_exists)) {
      if (kw_eq_cached(env, args[i + 1], syms.k_supersede))
        if_exists_action = 1;
      else if (kw_eq_cached(env, args[i + 1], syms.k_append))
        if_exists_action = 2;
      else if (kw_eq_cached(env, args[i + 1], syms.k_error))
        if_exists_action = 3;
      else if (kw_eq_cached(env, args[i + 1], syms.k_overwrite))
        if_exists_action = 4;
      else if (env->eq(env, args[i + 1], syms.v_nil))
        if_exists_action = 0;
    } else if (kw_eq_cached(env, args[i], syms.k_if_does_not_exist)) {
      if (kw_eq_cached(env, args[i + 1], syms.k_error))
        if_does_not_exist_action = 1;
      else if (env->eq(env, args[i + 1], syms.v_nil))
        if_does_not_exist_action = 0;
    }
  }

  /* :probe — stat only, return closed stream or nil */
  if (dir == 3) {
    int fd = open(filename, O_RDONLY);
    free(filename);
    if (fd < 0) return syms.v_nil;
    cl_stream_t *s = stream_create_fd(fd, true, CL_STREAM_FLAG_INPUT);
    if (!s) { close(fd); return syms.v_nil; }
    stream_close(s);
    return euser_ptr_stream_make(env, s);
  }

  /* apply :if-exists defaults and actions */
  if (if_exists_action == 0) {
    /* :if-exists nil — for output/io, return nil if file exists */
    if (dir == 1 || dir == 2) {
      struct stat st;
      if (stat(filename, &st) == 0) { free(filename); return syms.v_nil; }
    }
  } else if (if_exists_action == 1) {
    oflags |= O_CREAT | O_TRUNC;       /* :supersede */
  } else if (if_exists_action == 2) {
    oflags |= O_CREAT | O_APPEND;      /* :append */
  } else if (if_exists_action == 3) {
    oflags |= O_EXCL;                  /* :error (fail if file exists, no O_CREAT) */
  } else if (if_exists_action == 4) {
    oflags |= O_CREAT;                 /* :overwrite (no truncation) */
  } else {
    /* defaults per CL spec */
    if (dir == 1) oflags |= O_CREAT | O_TRUNC;  /* :output → supersede */
  }
  /* :input / :io default is :error (no O_CREAT, OS will fail if missing) */

  /* apply :if-does-not-exist */
  if (if_does_not_exist_action == 0) {
    /* :if-does-not-exist nil — return nil if file not present */
    struct stat st;
    if (stat(filename, &st) < 0 && errno == ENOENT) {
      free(filename);
      return syms.v_nil;
    }
  } else if (if_does_not_exist_action == 1) {
    /* :error — don't add O_CREAT */
  } else {
    /* default: :output/:io create, :input auto-signals error through OS */
    if (dir == 1 || dir == 2) oflags |= O_CREAT;
  }

  int fd = open(filename, oflags, 0644);
  free(filename);

  if (fd < 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Cannot open file: %s", strerror(errno));
    signal_error(env, buf);
    return syms.v_nil;
  }

  cl_stream_t *s = NULL;
  if (oflags == O_RDONLY && !(cl_flags & CL_STREAM_FLAG_BINARY)) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
      s = stream_create_mmap(fd, true);
  }
  if (!s)
    s = stream_create_fd(fd, true, cl_flags);

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
  (void)s;
  return eintern(env, ":default");
}
