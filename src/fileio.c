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
    signal_error(env, buf);
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
