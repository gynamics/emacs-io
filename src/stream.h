#ifndef EMACS_IO_STREAM_H
#define EMACS_IO_STREAM_H

#include "emacs-module.h"
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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
  CL_STREAM_KIND_MMAP,
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
  mode_t                 file_mode;   /* from fstat, 0 if not fd-backed */

  /* pushback ring buffer for unread */
  int                    pushback_chars[256];
  int                    pushback_head;
  int                    pushback_tail;
  int                    column;
};

/* ── concrete impl structs ───────────────────────────────────────────── */

/* fd-backed stream */
typedef struct {
  int            fd;
  unsigned char *rbuf;
  size_t         rsz;
  size_t         rpos;
  size_t         rlen;
  unsigned char *wbuf;
  size_t         wsz;
  size_t         wlen;
  bool           owns_fd;
} fd_stream_impl_t;

#define FD_STREAM_RBUF_DEFAULT 8192
#define FD_STREAM_WBUF_DEFAULT 8192

/* mmap-backed input stream (regular files only) */
typedef struct {
  void   *addr;
  size_t  size;
  size_t  pos;
  int     fd;
  bool    owns_fd;
} mmap_stream_impl_t;

/* string input stream */
typedef struct {
  const char *data;
  size_t      len;
  size_t      pos;
} string_input_impl_t;

/* string output stream — block-chain, no realloc copies */
typedef struct str_block {
  struct str_block *next;
  size_t            len;
  size_t            cap;
  char              data[];
} str_block_t;

typedef struct {
  str_block_t *head;
  str_block_t *tail;
  size_t       total_len;
} str_output_impl_t;

#define STR_BLOCK_DEFAULT_CAP 65536

/* buffer stream */
typedef struct {
  emacs_value  buffer;
  ptrdiff_t    pos;
  ptrdiff_t    end;
  bool         owns_ref;
} buffer_input_impl_t;

#define BUFFER_STREAM_CHUNK 65536

/* ── public stream API ───────────────────────────────────────────────── */

cl_stream_t *stream_create_fd(int fd, bool owns_fd, unsigned flags);
cl_stream_t *stream_create_mmap(int fd, bool owns_fd);
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
void      stream_unread(cl_stream_t *s, int ch);

emacs_value str_output_to_string(emacs_env *env, cl_stream_t *s);

static inline bool stream_is_open(cl_stream_t *s)
  { return s != NULL && atomic_load(&s->open); }

static inline bool stream_is_input(cl_stream_t *s)
  { return s != NULL && (s->flags & CL_STREAM_FLAG_INPUT); }

static inline bool stream_is_output(cl_stream_t *s)
  { return s != NULL && (s->flags & CL_STREAM_FLAG_OUTPUT); }

static inline void stream_set_nonblock(cl_stream_t *s, bool val)
  { if (val) s->flags |= CL_STREAM_FLAG_NONBLOCK;
    else s->flags &= ~CL_STREAM_FLAG_NONBLOCK; }

static inline bool stream_is_regular(cl_stream_t *s)
  { return s && S_ISREG(s->file_mode); }
static inline bool stream_is_pipe(cl_stream_t *s)
  { return s && S_ISFIFO(s->file_mode); }
static inline bool stream_is_tty(cl_stream_t *s)
  { return s && (s->flags & CL_STREAM_FLAG_INTERACTIVE); }

void stream_free(cl_stream_t *s);

#endif /* EMACS_IO_STREAM_H */
