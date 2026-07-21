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
    return eintern(env, "nil");
  }

  char *filename = estrdup(env, args[0]);
  if (!filename) {
    signal_error(env, "cl-open: invalid filename");
    return eintern(env, "nil");
  }

  /* defaults */
  unsigned flags = 0;
  flags |= CL_STREAM_FLAG_INPUT;   /* default :input */
  int oflags = O_RDONLY;

  /* parse &key arguments */
  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq(env, args[i], ":direction")) {
      if (kw_eq(env, args[i + 1], ":input")) {
        flags = (flags & ~CL_STREAM_FLAG_OUTPUT) | CL_STREAM_FLAG_INPUT;
        oflags = O_RDONLY;
      } else if (kw_eq(env, args[i + 1], ":output")) {
        flags = (flags & ~CL_STREAM_FLAG_INPUT) | CL_STREAM_FLAG_OUTPUT;
        oflags = O_WRONLY | O_CREAT | O_TRUNC;
      } else if (kw_eq(env, args[i + 1], ":io")) {
        flags |= CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
        oflags = O_RDWR | O_CREAT;
      } else if (kw_eq(env, args[i + 1], ":probe")) {
        flags = CL_STREAM_FLAG_INPUT;
        oflags = O_RDONLY;
      }
    } else if (kw_eq(env, args[i], ":element-type")) {
      /* check for (unsigned-byte 8) — compare as symbol for now */
      if (kw_eq(env, args[i + 1], "character"))
        flags &= ~CL_STREAM_FLAG_BINARY;
    } else if (kw_eq(env, args[i], ":if-exists")) {
      if (kw_eq(env, args[i + 1], ":supersede"))
        oflags |= O_TRUNC;
      else if (kw_eq(env, args[i + 1], ":append"))
        oflags |= O_APPEND;
      else if (kw_eq(env, args[i + 1], ":error"))
        oflags |= O_EXCL;
    }
  }

  int fd = open(filename, oflags, 0644);
  free(filename);

  if (fd < 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Cannot open file: %s", strerror(errno));
    signal_file_error(env, buf);
    return eintern(env, "nil");
  }

  /* use mmap for regular files opened read-only (zero-copy reads) */
  cl_stream_t *s = NULL;
  if (oflags == O_RDONLY) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
      s = stream_create_mmap(fd, true);
  }
  if (!s)
    s = stream_create_fd(fd, true, flags);

  if (!s) {
    close(fd);
    signal_error(env, "Cannot create stream");
    return eintern(env, "nil");
  }

  return euser_ptr_stream_make(env, s);
}

/* ── cl-close ──────────────────────────────────────────────────────────── */

emacs_value Fclose(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  /* (cl-close stream) */
  if (nargs < 1) {
    signal_error(env, "cl-close requires 1 argument (stream)");
    return eintern(env, "nil");
  }

  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) {
    signal_error(env, "cl-close: invalid stream");
    return eintern(env, "nil");
  }

  stream_close(s);
  return eintern(env, "t");
}

/* ── cl-make-string-input-stream ──────────────────────────────────────── */

emacs_value Fmake_string_input(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data) {
  (void)data;
  /* (cl-make-string-input-stream string &optional start end) */
  if (nargs < 1) {
    signal_error(env, "cl-make-string-input-stream requires 1 argument");
    return eintern(env, "nil");
  }

  ptrdiff_t slen = 0;
  char *str = estring_alloc(env, args[0], &slen);
  if (!str) {
    signal_error(env, "Cannot extract string");
    return eintern(env, "nil");
  }

  ptrdiff_t start = 0;
  ptrdiff_t end = slen;
  if (nargs >= 2 && env->is_not_nil(env, args[1]))
    start = eint_extract(env, args[1]);
  if (nargs >= 3 && env->is_not_nil(env, args[2]))
    end = eint_extract(env, args[2]);

  if (start < 0) start = 0;
  if (end > slen) end = slen;
  if (end < start) end = start;

  /* pass the full malloc'd buffer; set cursor via impl fields.
     sis_destroy will free(str) — must be the base of the allocation. */
  cl_stream_t *s = stream_create_string_input(str, (size_t)slen);
  if (!s) {
    free(str);
    signal_error(env, "Cannot create string input stream");
    return eintern(env, "nil");
  }
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  si->pos = (size_t)start;
  si->len = (size_t)end;

  return euser_ptr_stream_make(env, s);
}

/* ── cl-make-string-output-stream ──────────────────────────────────────── */

emacs_value Fmake_string_output(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data) {
  (void)data;
  /* (cl-make-string-output-stream &key element-type size) */
  size_t hint = 0;
  for (ptrdiff_t i = 0; i < nargs - 1; i += 2) {
    if (kw_eq(env, args[i], ":size"))
      hint = (size_t)eint_extract(env, args[i + 1]);
  }
  cl_stream_t *s = stream_create_string_output(hint);
  if (!s) {
    signal_error(env, "Cannot create string output stream");
    return eintern(env, "nil");
  }
  return euser_ptr_stream_make(env, s);
}

/* ── cl-get-output-stream-string ──────────────────────────────────────── */

emacs_value Fget_output_stream_string(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-get-output-stream-string requires 1 argument");
    return eintern(env, "nil");
  }
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->kind != CL_STREAM_KIND_STRING_OUTPUT) {
    signal_error(env, "Not a string output stream");
    return eintern(env, "nil");
  }
  return str_output_to_string(env, s);
}

/* ── stream predicates ────────────────────────────────────────────────── */

emacs_value Fopen_stream_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return (s && stream_is_open(s)) ? eintern(env, "t") : eintern(env, "nil");
}

emacs_value Finput_stream_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                             void *data) {
  (void)data;
  (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return (s && stream_is_input(s)) ? eintern(env, "t") : eintern(env, "nil");
}

emacs_value Foutput_stream_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                              void *data) {
  (void)data;
  (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return (s && stream_is_output(s)) ? eintern(env, "t") : eintern(env, "nil");
}

emacs_value Fstream_element_type(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data) {
  (void)data;
  (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s) {
    signal_error(env, "Invalid stream");
    return eintern(env, "nil");
  }
  if (s->flags & CL_STREAM_FLAG_BINARY)
    return estring(env, "(unsigned-byte 8)");
  return eintern(env, "character");
}

/* ── file type predicates ──────────────────────────────────────────── */

emacs_value Ffile_regular_p(emacs_env *env, ptrdiff_t nargs,
                             emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return stream_is_regular(s) ? eintern(env, "t") : eintern(env, "nil");
}

emacs_value Ffile_tty_p(emacs_env *env, ptrdiff_t nargs,
                         emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  return stream_is_tty(s) ? eintern(env, "t") : eintern(env, "nil");
}

/* ── cl-make-buffer-input-stream ────────────────────────────────────────
   (cl-make-buffer-input-stream buffer &key start end) */

emacs_value Fmake_buffer_input_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-buffer-input-stream requires 1 argument (buffer)");
    return eintern(env, "nil");
  }

  ptrdiff_t start = 0;
  ptrdiff_t end = eint_extract(env,
    efuncall(env, "buffer-size", 1, (emacs_value[]){ args[0] }));

  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq(env, args[i], ":start"))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq(env, args[i], ":end"))
      end = eint_extract(env, args[i + 1]);
  }

  cl_stream_t *s = stream_create_buffer_input(env, args[0], start, end);
  if (!s) {
    signal_error(env, "Cannot create buffer input stream");
    return eintern(env, "nil");
  }
  return euser_ptr_stream_make(env, s);
}
