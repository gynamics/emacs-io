#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include "subrs.h"

int plugin_is_GPL_compatible;

/* ── registration table ───────────────────────────────────────────────── */

typedef struct {
  const char   *name;
  emacs_subr_fn fn;
  ptrdiff_t     min, max;
  const char   *doc;
} subr_def_t;

static const subr_def_t subrs[] = {
  /* ── reader ───────────────────────────────────────────────────────── */
  { "cl-read-char", Fread_char, 0, 4,
    "Read one character from STREAM and return it.\n"
    "\n"
    "STREAM defaults to `*standard-input*'.\n"
    "If EOF-ERROR-P is nil and end of file is reached, return EOF-VALUE\n"
    "(which defaults to nil).\n"
    "If EOF-ERROR-P is non-nil (the default), signal an error on EOF.\n"
    "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
    "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)" },

  { "cl-read-line", Fread_line, 0, 4,
    "Read a line of text from STREAM and return it as a cons.\n"
    "\n"
    "Returns (LINE . MISSING-NEWLINE-P) where MISSING-NEWLINE-P is\n"
    "non-nil if the line was terminated by end-of-file rather than a\n"
    "newline.  The returned line does not include the newline.\n"
    "STREAM defaults to `*standard-input*'.\n"
    "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
    "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)" },

  { "cl-read-byte", Fread_byte, 1, 4,
    "Read one byte from STREAM and return it as an integer.\n"
    "\n"
    "If EOF-ERROR-P is nil and end of file is reached, return EOF-VALUE.\n"
    "If EOF-ERROR-P is non-nil (the default), signal an error on EOF.\n"
    "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
    "\n(fn STREAM &optional EOF-ERROR-P EOF-VALUE RECURSIVE-P)" },

  { "cl-read-char-no-hang", Fread_char_no_hang, 0, 3,
    "Read a character from STREAM, or return nil if none is available.\n"
    "\n"
    "Exactly like `cl-read-char', except that on an interactive stream it\n"
    "returns nil rather than waiting when no character is immediately\n"
    "available; on a non-interactive stream it never needs to wait and so\n"
    "behaves like `cl-read-char' (CL).\n"
    "STREAM defaults to `*standard-input*'.\n"
    "EOF-ERROR-P and EOF-VALUE are accepted for compatibility.\n"
    "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE)" },

  { "cl-peek-char", Fpeek_char, 0, 5,
    "Return the next character from STREAM without removing it.\n"
    "\n"
    "PEEK-TYPE controls which character is returned:\n"
    "  nil  — return the next character (the default)\n"
    "  t    — skip whitespace, then return the next character\n"
    "  char — skip characters until CHAR is found, then return it\n"
    "         (CHAR is left in the stream), as in CLHS\n"
    "STREAM defaults to `*standard-input*'.\n"
    "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
    "\n(fn &optional PEEK-TYPE STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)" },

  { "cl-unread-char", Funread_char, 1, 2,
    "Return CHARACTER to STREAM so it will be read again.\n"
    "\n"
    "CHARACTER must be the most recent character read from STREAM, and it\n"
    "is an error to unread twice without an intervening read (CL).\n"
    "STREAM defaults to `*standard-input*'.\n"
    "\n(fn CHARACTER &optional STREAM)" },

  { "cl-listen", Flisten, 0, 1,
    "Return non-nil if a character is available on STREAM, nil otherwise.\n"
    "\n"
    "On a non-interactive stream this is true except at end of file\n"
    "(CL); an interactive stream is probed for immediate availability.\n"
    "Pushback characters and buffered data always count as available.\n"
    "STREAM defaults to `*standard-input*'.\n"
    "\n(fn &optional STREAM)" },

  { "cl-read-sequence", Fread_sequence, 2, 6,
    "Destructively modify SEQUENCE by replacing its elements\n"
    "with elements read from STREAM.\n"
    "\n"
    "SEQUENCE must be a vector, list, or string.  Reading stops when the sequence\n"
    "is full or end of file is reached.\n"
    "Keywords :START and :END delimit the affected subsequence.\n"
    "Returns the index of the first element not updated.\n"
    "\n(fn SEQUENCE STREAM &key START END)" },

  { "cl-read-string", Fread_string, 2, 5,
    "Read up to N bytes from STREAM and return them as a unibyte string.\n"
    "\n"
    "Fewer bytes may be returned if end of file is reached.\n"
    "If EOF-ERROR-P is nil and end of file is reached, return EOF-VALUE.\n"
    "Use this for bulk reads; it avoids per-element overhead of\n"
    "`cl-read-sequence' for large data.\n"
    "\n(fn N STREAM &optional EOF-ERROR-P EOF-VALUE)" },

  { "cl-read-chunk", Fread_chunk, 1, 3,
    "Read up to MAX-BYTES (default 65536) from STREAM.\n"
    "\n"
    "Returns a cons (CHUNK . EOF-P) where CHUNK is a unibyte string\n"
    "and EOF-P is non-nil if end of file was reached.\n"
    "Use this for incremental bulk reads; it avoids the per-byte\n"
    "overhead of `cl-listen' + `cl-read-char' loops.\n"
    "\n(fn STREAM &optional MAX-BYTES)" },

  /* ── printer ──────────────────────────────────────────────────────── */
  { "cl-write-char", Fwrite_char, 1, 2,
    "Write CHARACTER to STREAM.\n"
    "\n"
    "STREAM defaults to `*standard-output*'.\n"
    "Returns CHARACTER.\n"
    "\n(fn CHARACTER &optional STREAM)" },

  { "cl-write-string", Fwrite_string, 1, 6,
    "Write STRING to STREAM.\n"
    "\n"
    "Keywords :START and :END delimit the substring to write.\n"
    "STREAM defaults to `*standard-output*'.\n"
    "Returns STRING.\n"
    "\n(fn STRING &optional STREAM &key START END)" },

  { "cl-write-line", Fwrite_line, 1, 5,
    "Write STRING followed by a newline to STREAM.\n"
    "\n"
    "Equivalent to (progn (cl-write-string STRING STREAM) (cl-terpri STREAM)).\n"
    "Keywords :START and :END delimit the substring to write.\n"
    "Returns STRING.\n"
    "\n(fn STRING &optional STREAM &key START END)" },

  { "cl-write-byte", Fwrite_byte, 2, 2,
    "Write BYTE (an integer 0–255) to STREAM.\n"
    "\n"
    "Returns BYTE.\n"
    "\n(fn BYTE STREAM)" },

  { "cl-io-fresh-line", Ffresh_line, 0, 1,
    "Output a newline to STREAM if not at the start of a line.\n"
    "\n"
    "If it is uncertain whether output is at the start of a line,\n"
    "a newline is always written (column tracking is not yet\n"
    "implemented for all stream types).\n"
    "Returns non-nil if a newline was written, nil otherwise.\n"
    "\n(fn &optional STREAM)" },

  { "cl-finish-output", Ffinish_output, 0, 1,
    "Attempt to ensure that all output sent to STREAM has reached\n"
    "its destination, and then return.\n"
    "\n"
    "For file streams this calls fsync(2).\n"
    "STREAM defaults to `*standard-output*'.\n"
    "\n(fn &optional STREAM)" },

  { "cl-force-output", Fforce_output, 0, 1,
    "Initiate the emptying of any internal output buffers on STREAM.\n"
    "\n"
    "For file streams this calls fflush(3).\n"
    "STREAM defaults to `*standard-output*'.\n"
    "\n(fn &optional STREAM)" },

  { "cl-clear-output", Fclear_output, 0, 1,
    "Abort any outstanding output operation on STREAM.\n"
    "\n"
    "Any buffered output that has not yet been written is discarded.\n"
    "STREAM defaults to `*standard-output*'.\n"
    "\n(fn &optional STREAM)" },

  { "cl-clear-input", Fclear_input, 0, 1,
    "Clear any buffered input on STREAM.\n"
    "\n"
    "Discards any characters in the stream's read buffer and\n"
    "pushback ring.\n"
    "STREAM defaults to `*standard-input*'.\n"
    "\n(fn &optional STREAM)" },

  { "cl-write-sequence", Fwrite_sequence, 2, 6,
    "Write the elements of SEQUENCE to STREAM.\n"
    "\n"
    "SEQUENCE must be a vector, list, or string.  Writing starts at :START\n"
    "and stops at :END (default the length of SEQUENCE).\n"
    "Returns SEQUENCE.\n"
    "\n(fn SEQUENCE STREAM &key START END)" },

  /* ── file I/O and streams ─────────────────────────────────────────── */
  { "cl-open", Fopen, 1, 8,
    "Open a file and return a stream connected to it.\n"
    "\n"
    "FILENAME is a string naming the file.\n"
    "Keyword arguments:\n"
    "  :DIRECTION   — :INPUT (default), :OUTPUT, :IO, or :PROBE\n"
    "  :ELEMENT-TYPE — character or (unsigned-byte 8)\n"
    "  :IF-EXISTS   — :ERROR (default), :SUPERSEDE, :APPEND, :OVERWRITE, NIL\n"
    "  :IF-DOES-NOT-EXIST — :ERROR, :CREATE, or NIL\n"
    "  :EXTERNAL-FORMAT — accepted for compatibility, currently ignored\n"
    "PROBE returns a closed stream if the file exists, NIL otherwise.\n"
    "\n(fn FILENAME &key DIRECTION ELEMENT-TYPE IF-EXISTS IF-DOES-NOT-EXIST EXTERNAL-FORMAT)" },

  { "cl-close", Fclose, 1, 3,
    "Close STREAM.\n"
    "\n"
    "If :ABORT is non-nil, pending output is discarded without flushing.\n"
    "Closing an already-closed stream is harmless and returns t.\n"
    "\n(fn STREAM &key ABORT)" },

  { "cl-make-string-input-stream", Fmake_string_input, 1, 3,
    "Return a stream which reads characters from STRING.\n"
    "\n"
    "Optional START and END arguments delimit the substring to read.\n"
    "The returned stream is an input stream with element type CHARACTER.\n"
    "\n(fn STRING &optional START END)" },

  { "cl-make-string-output-stream", Fmake_string_output, 0, 5,
    "Return a stream which accumulates output in a string.\n"
    "\n"
    "The accumulated string can be retrieved with\n"
    "`cl-get-output-stream-string'.\n"
    "The :ELEMENT-TYPE keyword is accepted for compatibility but\n"
    "currently ignored; the element type is always CHARACTER.\n"
    ":SIZE N pre-allocates a single block of N bytes, eliminating\n"
    "allocation overhead during writes.  Use when the output size\n"
    "is known in advance.\n"
    "\n(fn &key ELEMENT-TYPE SIZE)" },

  { "cl-get-output-stream-string", Fget_output_stream_string, 1, 1,
    "Return the accumulated string from a string output stream.\n"
    "\n"
    "STREAM must have been created by `cl-make-string-output-stream'.\n"
    "The stream remains open but its accumulated output is cleared.\n"
    "\n(fn STREAM)" },

  { "cl-make-buffer-input-stream", Fmake_buffer_input_stream, 1, 5,
    "Return a stream which reads characters from BUFFER.\n"
    "\n"
    "BUFFER may be a buffer object or a buffer name.\n"
    "Keywords :START and :END delimit the region to read.\n"
    "The stream reads buffer text in 64 KB chunks for efficiency.\n"
    "\n(fn BUFFER &key START END)" },

  { "cl-make-buffer-output-stream", Fmake_buffer_output_stream, 1, 1,
    "Return an output stream that inserts content into BUFFER.\n"
    "\n"
    "String writes (write-string, prin1, etc.) are inserted immediately.\n"
    "Byte writes (write-char, write-byte) are buffered and flushed\n"
    "on the next string write.\n"
    "\n(fn BUFFER)" },

  { "cl-make-buffer-io-stream", Fmake_buffer_io_stream, 1, 5,
    "Return a bidirectional stream reading from and writing to BUFFER.\n"
    "\n"
    "Reads operate on a snapshot taken at creation time.\n"
    "Writes accumulate and are inserted into BUFFER on close\n"
    "or on each string-write.  Keywords :START and :END delimit\n"
    "the initial read region.\n"
    "\n(fn BUFFER &key START END)" },

  { "cl-make-synonym-stream", Fmake_synonym_stream, 1, 1,
    "Return a stream that redirects I/O to SYMBOL's value.\n"
    "\n"
    "SYMBOL is a symbol whose dynamic value should be a stream.  The\n"
    "target is re-resolved on each operation.\n"
    "\n(fn SYMBOL)" },

  { "cl-synonym-stream-symbol", Fsynonym_stream_symbol, 1, 1,
    "Return the symbol whose value is the synonym stream's target.\n"
    "\n(fn SYNONYM-STREAM)" },

  { "cl-make-broadcast-stream", Fmake_broadcast_stream, 0, 16,
    "Return an output stream broadcasting to all given streams.\n"
    "\n(fn &rest STREAMS)" },

  { "cl-broadcast-stream-streams", Fbroadcast_stream_streams, 1, 1,
    "Return the streams composing a broadcast stream.\n"
    "\n(fn BROADCAST-STREAM)" },

  { "cl-make-concatenated-stream", Fmake_concatenated_stream, 0, 16,
    "Return an input stream reading from streams in sequence.\n"
    "\n(fn &rest STREAMS)" },

  { "cl-concatenated-stream-streams", Fconcatenated_stream_streams, 1, 1,
    "Return the streams not yet exhausted, starting with the current one.\n"
    "\n(fn CONCATENATED-STREAM)" },

  { "cl-make-two-way-stream", Fmake_two_way_stream, 2, 2,
    "Return a bidirectional stream combining INPUT and OUTPUT.\n"
    "\n"
    "Read/listen operations are forwarded to INPUT-STREAM.\n"
    "Write/flush operations are forwarded to OUTPUT-STREAM.\n"
    "\n(fn INPUT-STREAM OUTPUT-STREAM)" },

  { "cl-two-way-stream-input-stream", Ftwo_way_stream_input_stream, 1, 1,
    "Return the input stream of a two-way stream.\n"
    "\n(fn TWO-WAY-STREAM)" },

  { "cl-two-way-stream-output-stream", Ftwo_way_stream_output_stream, 1, 1,
    "Return the output stream of a two-way stream.\n"
    "\n(fn TWO-WAY-STREAM)" },

  { "cl-make-echo-stream", Fmake_echo_stream, 2, 2,
    "Return a bidirectional stream that echoes input to output.\n"
    "\n"
    "Characters read from the echo stream are read from\n"
    "INPUT-STREAM and also written to OUTPUT-STREAM.\n"
    "Write operations are forwarded to OUTPUT-STREAM.\n"
    "\n(fn INPUT-STREAM OUTPUT-STREAM)" },

  { "cl-echo-stream-input-stream", Fecho_stream_input_stream, 1, 1,
    "Return the input stream of an echo stream.\n"
    "\n(fn ECHO-STREAM)" },

  { "cl-echo-stream-output-stream", Fecho_stream_output_stream, 1, 1,
    "Return the output stream of an echo stream.\n"
    "\n(fn ECHO-STREAM)" },

  /* ── predicates ───────────────────────────────────────────────────── */
  { "cl-streamp", Fstreamp, 1, 1,
    "Return t if OBJECT is a stream (open or closed).\n"
    "\n(fn OBJECT)" },

  { "cl-open-stream-p", Fopen_stream_p, 1, 1,
    "Return t if OBJECT is an open stream, nil otherwise.\n"
    "\n(fn OBJECT)" },

  { "cl-input-stream-p", Finput_stream_p, 1, 1,
    "Return t if STREAM is an input stream, nil otherwise.\n"
    "\n(fn STREAM)" },

  { "cl-output-stream-p", Foutput_stream_p, 1, 1,
    "Return t if STREAM is an output stream, nil otherwise.\n"
    "\n(fn STREAM)" },

  { "cl-stream-element-type", Fstream_element_type, 1, 1,
    "Return a type specifier for the kind of object returned by\n"
    "`cl-read-char' on STREAM.\n"
    "\n"
    "Currently returns `character' or the list `(unsigned-byte 8)'.\n"
    "\n(fn STREAM)" },

  { "cl-file-regular-p", Ffile_regular_p, 1, 1,
    "Return t if STREAM is backed by a regular file (S_ISREG).\n"
    "\n(fn STREAM)" },

  { "cl-file-tty-p", Ffile_tty_p, 1, 1,
    "Return t if STREAM is connected to a terminal (isatty).\n"
    "\n(fn STREAM)" },

  { "cl-interactive-stream-p", Finteractive_stream_p, 1, 1,
    "Return t if STREAM is an interactive stream.\n"
    "\n"
    "An interactive stream is one connected to a terminal\n"
    "(i.e., isatty).\n"
    "\n(fn STREAM)" },

  { "cl-stream-external-format", Fstream_external_format, 1, 1,
    "Return the external format of STREAM.\n"
    "\n"
    "Returns the :EXTERNAL-FORMAT value passed to `cl-open',\n"
    "or :default if none was specified.\n"
    "\n(fn STREAM)" },

  { "cl-file-position", Ffile_position, 1, 3,
    "Return the current position of STREAM, or set it.\n"
    "\n"
    "With one argument, return the current position as a\n"
    "non-negative integer, or nil if the stream does not support\n"
    "position queries.\n"
    "With a second argument POSITION (an integer or :START/:END), set\n"
    "the position and return t, signalling an error if inappropriate.\n"
    "\n(fn STREAM &optional POSITION)" },

  { "cl-file-length", Ffile_length, 1, 1,
    "Return the length of the file associated with STREAM.\n"
    "\n"
    "Returns a non-negative integer if the stream supports length\n"
    "queries, or nil if the length cannot be determined.\n"
    "\n(fn STREAM)" },

  { "cl-ftruncate", Fftruncate, 2, 2,
    "Truncate the file associated with STREAM to LENGTH bytes.\n"
    "\n"
    "STREAM must be an open stream backed by a regular file; it is an error\n"
    "to truncate anything else (e.g. a pipe).\n"
    "Returns t on success.\n"
    "\n(fn STREAM LENGTH)" },

  /* ── Lisp reader / printer ──────────────────────────────────────────
     cl-read, cl-read-preserving-whitespace and cl-read-delimited-list are
     implemented by the `emacs-io' Lisp layer (emacs-io.el) on top of the
     character primitives and Emacs' own reader, so they are not registered
     here. */
};

/* ── module entry point ───────────────────────────────────────────────── */

int emacs_module_init(struct emacs_runtime *runtime) {
  emacs_env *env = runtime->get_environment(runtime);

  /* ensure we have at least emacs_env_31 */
  if ((size_t)env->size < sizeof(struct emacs_env_31)) return 1;

  syms_init(env);
  cl_rt = runtime;

  for (size_t i = 0; i < sizeof(subrs) / sizeof(subrs[0]); i++)
    defun(env, subrs[i].name, subrs[i].fn, subrs[i].min, subrs[i].max,
          subrs[i].doc, NULL);

  init_standard_streams(env);
  provide(env, "emacs-io-core");
  return 0;
}
