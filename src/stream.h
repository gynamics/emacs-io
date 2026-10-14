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

/* Pushback ring capacity (a power of two).  It only holds characters
   returned by cl-unread-char / cl-peek-char; readers scan a non-consuming
   window (stream_direct) instead of over-reading a chunk, so a small ring
   suffices and no data can be lost. */
#define CL_PUSHBACK_SIZE 256

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
typedef bool      (*stream_direct_fn)(cl_stream_t *s, const void **addr, size_t *len);
typedef void      (*stream_advance_fn)(cl_stream_t *s, ptrdiff_t n);
typedef ptrdiff_t (*stream_write_string_fn)(emacs_env *env, cl_stream_t *s, emacs_value str);
typedef void      (*stream_clear_fn)(cl_stream_t *s);
typedef ptrdiff_t (*stream_file_position_fn)(cl_stream_t *s);
typedef bool      (*stream_set_file_position_fn)(cl_stream_t *s, ptrdiff_t pos);
typedef ptrdiff_t (*stream_file_length_fn)(cl_stream_t *s);
typedef void      (*stream_resolve_fn)(cl_stream_t *s);
typedef bool      (*stream_truncate_fn)(cl_stream_t *s, ptrdiff_t len);

struct cl_stream_ops {
  stream_read_fn         read;
  stream_write_fn        write;
  stream_close_fn        close;
  stream_flush_fn        flush;
  stream_finish_fn       finish;
  stream_listen_fn       listen;
  stream_destroy_fn      destroy;
  stream_direct_fn       direct;
  stream_advance_fn      advance;
  stream_write_string_fn write_string;
  stream_clear_fn        clear_input;
  stream_clear_fn        clear_output;
  stream_file_position_fn file_position;
  stream_set_file_position_fn set_file_position;
  stream_file_length_fn   file_length;
  stream_resolve_fn       resolve;
  stream_truncate_fn      truncate;
};

/* ── core stream object ──────────────────────────────────────────────── */

typedef enum {
  CL_STREAM_KIND_FD,
  CL_STREAM_KIND_MMAP,
  CL_STREAM_KIND_STRING_INPUT,
  CL_STREAM_KIND_STRING_OUTPUT,
  CL_STREAM_KIND_BUFFER_INPUT,
  CL_STREAM_KIND_BUFFER_OUTPUT,
  CL_STREAM_KIND_BUFFER_IO,
  CL_STREAM_KIND_SYNONYM,
  CL_STREAM_KIND_BROADCAST,
  CL_STREAM_KIND_CONCATENATED,
  CL_STREAM_KIND_TWO_WAY,
  CL_STREAM_KIND_ECHO,
} cl_stream_kind_t;

struct cl_stream {
  const cl_stream_ops_t *ops;
  void                  *impl;
  cl_stream_kind_t       kind;
  pthread_mutex_t        mutex;
  unsigned               flags;
  _Atomic bool           open;
  mode_t                 file_mode;   /* from fstat, 0 if not fd-backed */

  /* pushback ring buffer for cl-unread-char / cl-peek-char.  Readers use
     the non-consuming window (stream_direct) rather than pushing an
     over-read tail here, so this stays small. */
  int                    pushback_chars[CL_PUSHBACK_SIZE];
  int                    pushback_head;
  int                    pushback_tail;

  /* cl-unread-char contract (CL): the character most recently read, and
     whether an unread is already outstanding (unread twice consecutively is
     an error).  Updated by the character-reading primitives. */
  int                    unread_last;
  bool                   unread_last_valid;
  bool                   unread_pending;

  /* output column tracking for cl-io-fresh-line */
  ptrdiff_t              column;       /* current column, 0 = BOL */
  int                    last_char;    /* last byte written */

  /* external format (for stream-external-format), NULL if :default */
  emacs_value            external_format;
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
  char          *path;     /* set by cl-open, for :abort cleanup; may be NULL */
  bool           created;  /* true if cl-open created the file */
} fd_stream_impl_t;

#define FD_STREAM_RBUF_DEFAULT 4096
#define FD_STREAM_WBUF_DEFAULT 4096

/* mmap-backed input stream (regular files only) */
typedef struct {
  void   *addr;
  size_t  size;
  size_t  pos;
  int     fd;
  bool    owns_fd;
} mmap_stream_impl_t;

/* Shared cursor for snapshot-backed in-memory streams (string input,
   buffer input, buffer I/O).  Placed first in each impl struct so the
   mem_cursor_* ops can reach it via `(mem_cursor_t *)s->impl'. */
typedef struct {
  char   *data;   /* owned snapshot; NULL for an empty buffer */
  size_t  len;
  size_t  pos;
} mem_cursor_t;

ptrdiff_t mem_cursor_read(cl_stream_t *s, void *buf, ptrdiff_t n);
bool      mem_cursor_direct(cl_stream_t *s, const void **addr, size_t *len);
bool      mem_cursor_listen(cl_stream_t *s);
ptrdiff_t mem_cursor_file_position(cl_stream_t *s);
bool      mem_cursor_set_file_position(cl_stream_t *s, ptrdiff_t pos);
ptrdiff_t mem_cursor_file_length(cl_stream_t *s);
void      mem_cursor_advance(cl_stream_t *s, ptrdiff_t n);
void      mem_cursor_clear_input(cl_stream_t *s);

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
#define BUFFER_STREAM_CHUNK 65536

/* ── public stream API ───────────────────────────────────────────────── */

/* Allocate a stream object wrapped around IMPL and initialise its
   common fields (ops, kind, flags, open flag, mutex).  Returns NULL on
   allocation failure; the caller still owns IMPL and must free it in
   that case. */
cl_stream_t *stream_alloc(const cl_stream_ops_t *ops, void *impl,
                          cl_stream_kind_t kind, unsigned flags);

/* Registry of live streams, so predicates can recognise emacs-io streams
   (Emacs cannot otherwise tell one module's user-ptr from another's). */
void stream_registry_add(cl_stream_t *s);
void stream_registry_remove(cl_stream_t *s);
bool stream_registered(cl_stream_t *s);

cl_stream_t *stream_create_fd(int fd, bool owns_fd, unsigned flags);
cl_stream_t *stream_create_mmap(int fd, bool owns_fd);
cl_stream_t *stream_create_string_input(const char *data, size_t len);
cl_stream_t *stream_create_string_output(size_t hint_size);
cl_stream_t *stream_create_buffer_input(emacs_env *env, emacs_value buffer,
                                        ptrdiff_t start, ptrdiff_t end);
cl_stream_t *stream_create_buffer_output(emacs_env *env, emacs_value buffer);
cl_stream_t *stream_create_buffer_io(emacs_env *env, emacs_value buffer,
                                      ptrdiff_t start, ptrdiff_t end);
extern const cl_stream_ops_t buffer_output_ops;
void bos_flush_pending(emacs_env *env, cl_stream_t *s);
extern const cl_stream_ops_t buffer_io_ops;
cl_stream_t *stream_create_synonym(cl_stream_t *target, emacs_env *env,
                                    emacs_value symbol);
cl_stream_t *stream_create_concatenated(cl_stream_t **streams, ptrdiff_t count);
cl_stream_t *stream_create_twoway(cl_stream_t *input, cl_stream_t *output);
cl_stream_t *stream_create_echo(cl_stream_t *input, cl_stream_t *output);

/* Advance the output column/last-char bookkeeping by N written bytes.
   Used by the byte-write dispatch and by write_string implementations that
   write directly (bypassing stream_write). */
void stream_update_column(cl_stream_t *s, const unsigned char *p, ptrdiff_t n);

ptrdiff_t stream_read(cl_stream_t *s, void *buf, ptrdiff_t n);
ptrdiff_t stream_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
ptrdiff_t stream_write_string(emacs_env *env, cl_stream_t *s, emacs_value str);
int       stream_close(cl_stream_t *s);
int       stream_flush(cl_stream_t *s);
int       stream_finish(cl_stream_t *s);
bool      stream_listen(cl_stream_t *s);
void      stream_unread(cl_stream_t *s, int ch);
/* Push N bytes back in one lock acquisition. */
void      stream_unread_bulk(cl_stream_t *s, const unsigned char *data, ptrdiff_t n);
/* Drain only the pushback ring into BUF (up to N bytes), leaving the
   underlying window untouched.  Used by the readers before they scan. */
ptrdiff_t stream_read_pushback(cl_stream_t *s, void *buf, ptrdiff_t n);
bool      stream_direct(cl_stream_t *s, const void **addr, size_t *len);
void      stream_advance(cl_stream_t *s, ptrdiff_t n);
ptrdiff_t stream_file_position(cl_stream_t *s);
bool      stream_set_file_position(cl_stream_t *s, ptrdiff_t pos);
ptrdiff_t stream_file_length(cl_stream_t *s);
bool      stream_truncate(cl_stream_t *s, ptrdiff_t len);

emacs_value str_output_to_string(emacs_env *env, cl_stream_t *s);

static inline bool stream_is_open(cl_stream_t *s)
  { return s != NULL && atomic_load(&s->open); }

static inline bool stream_is_input(cl_stream_t *s)
  { return s != NULL && (s->flags & CL_STREAM_FLAG_INPUT); }

static inline bool stream_is_output(cl_stream_t *s)
  { return s != NULL && (s->flags & CL_STREAM_FLAG_OUTPUT); }

static inline bool stream_is_regular(cl_stream_t *s)
  { return s && S_ISREG(s->file_mode); }
static inline bool stream_is_tty(cl_stream_t *s)
  { return s && (s->flags & CL_STREAM_FLAG_INTERACTIVE); }

void stream_free(cl_stream_t *s);

#endif /* EMACS_IO_STREAM_H */
