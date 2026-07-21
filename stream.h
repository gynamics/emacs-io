#ifndef EMACS_IO_STREAM_H
#define EMACS_IO_STREAM_H

#include "emacs-module.h"
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── flags ────────────────────────────────────────────────────────────── */
#define CL_STREAM_FLAG_INPUT       (1u << 0)
#define CL_STREAM_FLAG_OUTPUT      (1u << 1)
#define CL_STREAM_FLAG_NONBLOCK    (1u << 2)
#define CL_STREAM_FLAG_INTERACTIVE (1u << 3)
#define CL_STREAM_FLAG_BINARY      (1u << 4)

/* ── forward declarations ─────────────────────────────────────────────── */

typedef struct cl_stream      cl_stream_t;
typedef struct cl_stream_ops  cl_stream_ops_t;

/* ── vtable ──────────────────────────────────────────────────────────── */

typedef ptrdiff_t (*stream_read_fn)(cl_stream_t *s, void *buf, ptrdiff_t n);
typedef ptrdiff_t (*stream_write_fn)(cl_stream_t *s, const void *buf, ptrdiff_t n);
typedef int       (*stream_close_fn)(cl_stream_t *s);
typedef int       (*stream_flush_fn)(cl_stream_t *s);
typedef int       (*stream_finish_fn)(cl_stream_t *s);
typedef bool      (*stream_listen_fn)(cl_stream_t *s);
typedef void      (*stream_destroy_fn)(cl_stream_t *s);

struct cl_stream_ops {
  stream_read_fn    read;
  stream_write_fn   write;
  stream_close_fn   close;
  stream_flush_fn   flush;
  stream_finish_fn  finish;
  stream_listen_fn  listen;
  stream_destroy_fn destroy;
};

/* ── core stream object ──────────────────────────────────────────────── */

typedef enum {
  CL_STREAM_KIND_FD,
  CL_STREAM_KIND_STRING_INPUT,
  CL_STREAM_KIND_STRING_OUTPUT,
  CL_STREAM_KIND_BUFFER_INPUT,
} cl_stream_kind_t;

struct cl_stream {
  const cl_stream_ops_t *ops;
  void                  *impl;
  cl_stream_kind_t       kind;
  pthread_mutex_t        mutex;
  unsigned               flags;
  _Atomic bool           open;
};

/* ── concrete impl structs ───────────────────────────────────────────── */

/* fd-backed stream */
typedef struct {
  int            fd;
  unsigned char *rbuf;
  size_t         rsz;       /* buffer capacity */
  size_t         rpos;      /* next byte to return */
  size_t         rlen;      /* valid bytes in buffer */
  unsigned char *wbuf;
  size_t         wsz;       /* buffer capacity */
  size_t         wlen;      /* bytes pending flush */
  bool           owns_fd;   /* close fd on stream-close */
} fd_stream_impl_t;

#define FD_STREAM_RBUF_DEFAULT 8192
#define FD_STREAM_WBUF_DEFAULT 8192

/* string input stream */
typedef struct {
  const char *data;
  size_t      len;
  size_t      pos;
} string_input_impl_t;

/* string output stream */
typedef struct {
  char   *data;
  size_t  len;
  size_t  cap;
} string_output_impl_t;

#define STRING_OUTPUT_INITIAL_CAP 4096

/* ── buffer stream (for future buffer-bridge transformer) ────────────── */

typedef struct {
  emacs_value  buffer;   /* global ref to Emacs buffer */
  ptrdiff_t    pos;      /* current read position */
  ptrdiff_t    end;      /* exclusive end bound */
  bool         owns_ref;
} buffer_input_impl_t;

#define BUFFER_STREAM_CHUNK 65536

/* ── public stream API ───────────────────────────────────────────────── */

cl_stream_t *stream_create_fd(int fd, bool owns_fd, unsigned flags);
cl_stream_t *stream_create_string_input(const char *data, size_t len);
cl_stream_t *stream_create_string_output(void);
cl_stream_t *stream_create_buffer_input(emacs_env *env, emacs_value buffer,
                                        ptrdiff_t start, ptrdiff_t end);

ptrdiff_t stream_read(cl_stream_t *s, void *buf, ptrdiff_t n);
ptrdiff_t stream_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
int       stream_close(cl_stream_t *s);
int       stream_flush(cl_stream_t *s);
int       stream_finish(cl_stream_t *s);
bool      stream_listen(cl_stream_t *s);

static inline bool stream_is_open(cl_stream_t *s)
  { return s != NULL && atomic_load(&s->open); }

static inline bool stream_is_input(cl_stream_t *s)
  { return s != NULL && (s->flags & CL_STREAM_FLAG_INPUT); }

static inline bool stream_is_output(cl_stream_t *s)
  { return s != NULL && (s->flags & CL_STREAM_FLAG_OUTPUT); }

static inline void stream_set_nonblock(cl_stream_t *s, bool val)
  { if (val) s->flags |= CL_STREAM_FLAG_NONBLOCK;
    else s->flags &= ~CL_STREAM_FLAG_NONBLOCK; }

void stream_free(cl_stream_t *s);   /* calls close, then free */

#endif /* EMACS_IO_STREAM_H */
