#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <unistd.h>

/* Set the Lisp variable NAME to VALUE. */
static void set_var(emacs_env *env, const char *name, emacs_value value) {
  emacs_value args[2] = { eintern(env, name), value };
  efuncall_cached(env, syms.fn_set, 2, args);
}

/* Set the Lisp variable NAME to the value of an existing variable SRC. */
static void alias_var(emacs_env *env, const char *name, const char *src) {
  emacs_value value = efuncall_cached(env, syms.fn_symbol_value, 1,
                                      (emacs_value[]){ eintern(env, src) });
  set_var(env, name, value);
}

/* Create a non-owning fd stream and bind it to the Lisp variable NAME. */
static void bind_fd_stream(emacs_env *env, const char *name, int fd,
                           unsigned flags) {
  set_var(env, name, euser_ptr_stream_make(env, stream_create_fd(fd, false, flags)));
}

void init_standard_streams(emacs_env *env) {
  bind_fd_stream(env, "*standard-input*",  STDIN_FILENO,  CL_STREAM_FLAG_INPUT);
  bind_fd_stream(env, "*standard-output*", STDOUT_FILENO, CL_STREAM_FLAG_OUTPUT);
  bind_fd_stream(env, "*error-output*",    STDERR_FILENO, CL_STREAM_FLAG_OUTPUT);

  /* trace-output is an alias for error-output */
  alias_var(env, "*trace-output*", "*error-output*");

  /* terminal-io, debug-io and query-io are bidirectional when stdin is a
     terminal; otherwise (batch mode) they alias the standard streams. */
  if (isatty(STDIN_FILENO)) {
    unsigned flags = CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
    bind_fd_stream(env, "*terminal-io*", STDIN_FILENO, flags);
    bind_fd_stream(env, "*debug-io*",    STDIN_FILENO, flags);
    bind_fd_stream(env, "*query-io*",    STDIN_FILENO, flags);
  } else {
    alias_var(env, "*terminal-io*", "*standard-output*");
    alias_var(env, "*debug-io*",    "*standard-output*");
    alias_var(env, "*query-io*",    "*standard-input*");
  }
}
