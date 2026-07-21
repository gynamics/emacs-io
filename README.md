# emacs-io

CL-style stream I/O as a dynamic module for GNU Emacs 31.

```elisp
(module-load "emacs-io.so")
```

## Why

Emacs buffers are the universal I/O medium — and the bottleneck.  Every
`insert` triggers modification hooks, maintains undo history, repositions
markers and overlays, and forces gap-buffer moves.  A 100 MB log file loaded
into a temp buffer allocates 100 MB of gap-buffer, then garbage-collects it.

Streams bypass all of that.  Raw `read(2)` / `write(2)` through userspace
ring buffers, zero copy within string streams, and no hook machinery.  Bulk
sequence operations run **15× faster** than the equivalent buffer loop.

## Quick start

```
$ make             # → emacs-io.so
$ make verify      # confirm module loads in Emacs
$ make test        # 48 unit tests
$ make bench       # 6 performance benchmarks
```

## Stream types

| Constructor | Backend | Direction |
|---|---|---|
| `cl-open` | POSIX `open(2)`, 8 KB ring buffers, `O_NONBLOCK` support | any |
| `cl-make-string-input-stream` | C `const char*` with position cursor | input |
| `cl-make-string-output-stream` | Growable C `char*` via `realloc` | output |
| `cl-make-buffer-input-stream` | Emacs buffer via `buffer-substring-no-properties`, 64 KB chunks | input |

Standard streams (`*standard-input*`, `*standard-output*`, `*error-output*`,
`*trace-output*`, `*debug-io*`, `*query-io*`, `*terminal-io*`) are
initialized at module load, mapped to file descriptors 0, 1, and 2.

## API — 35 functions

### Stream lifecycle
`cl-open` `cl-close` `cl-open-stream-p` `cl-input-stream-p` `cl-output-stream-p` `cl-stream-element-type`

### String streams
`cl-make-string-input-stream` `cl-make-string-output-stream` `cl-get-output-stream-string`

### Character I/O
`cl-read-char` `cl-read-line` `cl-read-byte` `cl-peek-char` `cl-unread-char` `cl-listen`
`cl-write-char` `cl-write-string` `cl-write-line` `cl-write-byte`
`cl-terpri` `cl-fresh-line`

### Stream control
`cl-finish-output` (fsync) `cl-force-output` (fflush) `cl-clear-output` `cl-clear-input`

### Sequence I/O
`cl-read-sequence` `cl-write-sequence`

### Lisp reader
`cl-read` `cl-read-from-string` `cl-read-preserving-whitespace`

### Lisp printer
`cl-write` `cl-prin1` `cl-princ` `cl-print` `cl-prin1-to-string` `cl-princ-to-string`

## Performance

All numbers from Emacs 31.0.90, Linux x86-64, GCC 14 `-O2`.

### Benchmarks where streams dominate

| Test | Stream | Buffer | Speedup |
|---|---|---|---|
| `cl-read-sequence` 100 KB | 2.3 ms | 35.9 ms | **15.7×** |
| `cl-write-sequence` 100 KB | 2.3 ms | 36.7 ms | **15.8×** |
| `cl-write-string` accumulate 100 KB | 18.3 ms | 35.7 ms | **1.9×** |
| `cl-write-char` accumulate 1 KB | 0.2 ms | 0.3 ms | **1.5×** |

The 15× gap comes from eliminating per-element Lisp-call overhead: a C
`memcpy` of 100 KB vs. 100,000 iterations of `(aset vec i (char-after ...))`
each crossing the C–Lisp boundary and triggering buffer validation.

### Benchmarks where buffers win (and why)

| Test | Stream | Buffer | Ratio |
|---|---|---|---|
| `cl-read-line` 10 K lines | 7.7 ms | 3.2 ms | 0.4× |
| Comint-style incremental insert + line scan | 6.3 ms | 5.4 ms | 0.9× |

`cl-read-line` reads one byte at a time through the dispatch layer —
mutex lock/unlock + vtable call per character.  `buffer-substring + forward-line`
is a single C-level memcpy.  This is fixable: a bulk chunk-read with internal
newline scanning (in the C layer) would close the gap entirely for line
reading; it just hasn't been done yet.  The pushback ring buffer is already in
place to support it.

## Architecture

```
stream.h      — cl_stream vtable, impl structs, public API
stream.c      — dispatch layer (lock, drain 16-slot pushback ring, call vtable)
fdstream.c    — POSIX fd I/O with 8 KB userspace ring buffers, fcntl O_NONBLOCK
strstream.c   — string-input (cursor), string-output (growable), buffer-input (64 KB chunker)
util.c        — emacs_env wrappers, defun, kw_eq, strbuf, error signaling
emacs-io.c    — emacs_module_init, 35 defun registrations, plugin_is_GPL_compatible
reader.c      — cl-read-char, cl-read-line (CRLF-aware), cl-read-byte, peek, unread, listen, read-sequence
printer.c     — cl-write-*, cl-terpri, cl-fresh-line, flush/force/finish/clear, write-sequence
fileio.c      — cl-open (POSIX open(2), CL keyword args), cl-close, string/buffer stream constructors
lispio.c      — cl-read (16-state form parser → Emacs read-from-string), cl-prin1/princ/print
stdstream.c   — standard stream variable initialization
```

Every I/O operation acquires a per-stream `pthread_mutex_t`.  Streams can be
flagged `O_NONBLOCK` and queried with `cl-listen` — ready for `select`/`poll`
event loops.

## Application prospects

### IPC — pipes as streams

```elisp
;; Subprocess stdout → stream, no buffer overhead
(let ((s (cl-open "/tmp/worker-pipe" :direction :input)))
  (while (cl-listen s)
    (process-message (cl-read-line s nil nil)))
  (cl-close s))
```

### DRM / GPU — mmap + ioctl (planned)

```elisp
;; Future: map GPU framebuffer, write pixels directly
(let* ((fd (cl-open "/dev/dri/card0" :direction :io))
       (buf (cl-stream-mmap fd :size (* 1920 1080 4)))
       (s   (cl-make-string-input-stream buf)))
  ;; zero-copy pixel access
  )
```

### Terminal applications — raw mode + pty

```elisp
;; Future: raw terminal I/O for full-screen TUI
(let ((s (cl-open "/dev/pts/3" :direction :io)))
  (cl-stream-ioctl s :raw-mode t)
  (while (cl-listen s)
    (handle-key (cl-read-char s))))
```

### Data processing

- **Log analysis**: stream-based grep/parse avoiding temp buffer GC pressure
- **JSON/CSV parsing**: bulk `cl-read-sequence` → parse loop without `insert-file-contents`
- **Network servers**: socket fd → stream → message dispatch, no `make-network-process` filter overhead

### Build systems & LSP

- **Compilation output**: write to a broadcast stream → display buffer + log file + ring buffer
- **LSP JSON-RPC**: `cl-write-sequence` encode → `cl-read` decode, per-message zero-copy

## Flaws & known limitations

### ERT byte-compiler quirk (Emacs 31.0.90)

`cl-fresh-line` and `cl-prin1` trigger `(invalid-function #<user-ptr ...>)`
when called with a stream argument inside an `ert-deftest` body.  ERT
byte-compiles its test lambdas; the byte-compiler appears to mis-handle
module function calls with user-ptr arguments in certain positions.  The
functions work perfectly in normal evaluation and from `defun`-compiled
code.  Tests for these use the `*-to-string` variants.  This is believed to
be an upstream Emacs bug, not a module defect.

### Per-character read-line overhead

`cl-read-line` reads individual bytes through the full dispatch layer
(mutex lock → vtable call → memcpy 1 byte → unlock → repeat).  This is
~2× slower than `buffer-substring` for complete text.  A bulk-chunk
approach (read 4 KB, scan for `\n` in C, unread remainder via the pushback
ring) would eliminate this gap.  The 16-slot pushback ring is already in
place; the bulk-read implementation is deferred.

### Pushback ring overflow

The pushback ring holds 16 characters.  `cl-unread-char` drops the
character silently if the ring is full.  This is adequate for
peek-ahead in `cl-read-line` (CRLF handling) and `cl-peek-char` (1
character), but insufficient for bulk-read-line leftover.  The ring can
grow to 256 slots without significant memory increase (2 KB per stream).

### No composite streams yet

Broadcast, two-way, synonym, concatenated, and echo streams are all
unimplemented.  The vtable dispatch is designed for them; each would be a
new `cl_stream_ops` implementation wrapping inner streams.

### No encoding layer

Streams deal in raw bytes.  An encoding transformer (UTF-8 ↔ raw) would
sit between the fd-stream and the character-level primitives.  This is
intentionally deferred — encoding is a transformer, not a core concern.

### Buffer output stream missing

`cl-make-buffer-input-stream` exists but `cl-make-buffer-output-stream`
does not.  Writing to buffers is the next obvious transformer.

### No `file-position` (seek)

Random-access file I/O requires `lseek`.  The fd-stream impl holds an fd,
so adding `cl-file-position` is straightforward — it just hasn't been
exposed yet.

### Single-OS target

This module targets Linux (and likely BSD/macOS with minor tweaks).
`memfd_create`, `O_NONBLOCK` via `fcntl`, and `fsync` are POSIX.1-2001.
Windows is unsupported.

## License

GPL-3.0+
