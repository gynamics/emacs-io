#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <unistd.h>

void init_standard_streams(emacs_env *env) {
  /* standard-input: fd 0, input */
  {
    cl_stream_t *s = stream_create_fd(STDIN_FILENO, false,
                                       CL_STREAM_FLAG_INPUT);
    emacs_value v = euser_ptr_stream_make(env, s);
    emacs_value args[2];
    args[0] = eintern(env, "*standard-input*");
    args[1] = v;
    efuncall_cached(env, syms.fn_set, 2, args);
  }

  /* standard-output: fd 1, output */
  {
    cl_stream_t *s = stream_create_fd(STDOUT_FILENO, false,
                                       CL_STREAM_FLAG_OUTPUT);
    emacs_value v = euser_ptr_stream_make(env, s);
    emacs_value args[2];
    args[0] = eintern(env, "*standard-output*");
    args[1] = v;
    efuncall_cached(env, syms.fn_set, 2, args);
  }

  /* error-output: fd 2, output */
  {
    cl_stream_t *s = stream_create_fd(STDERR_FILENO, false,
                                       CL_STREAM_FLAG_OUTPUT);
    emacs_value v = euser_ptr_stream_make(env, s);
    emacs_value args[2];
    args[0] = eintern(env, "*error-output*");
    args[1] = v;
    efuncall_cached(env, syms.fn_set, 2, args);
  }

  /* trace-output: fd 2, output (alias to error-output) */
  {
    emacs_value serr = efuncall_cached(env, syms.fn_symbol_value, 1,
                                (emacs_value[]){ eintern(env, "*error-output*") });
    emacs_value args[2];
    args[0] = eintern(env, "*trace-output*");
    args[1] = serr;
    efuncall_cached(env, syms.fn_set, 2, args);
  }

  /* terminal-io, debug-io, query-io: bidirectional if fd 0 is a terminal.
     In batch mode, fall back to single-direction fd streams. */
  if (isatty(STDIN_FILENO)) {
    cl_stream_t *tio = stream_create_fd(STDIN_FILENO, false,
                            CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT);
    cl_stream_t *dio = stream_create_fd(STDIN_FILENO, false,
                            CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT);
    cl_stream_t *qio = stream_create_fd(STDIN_FILENO, false,
                            CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT);
    {
      emacs_value args[2];
      args[0] = eintern(env, "*terminal-io*");
      args[1] = euser_ptr_stream_make(env, tio);
      efuncall_cached(env, syms.fn_set, 2, args);
    }
    {
      emacs_value args[2];
      args[0] = eintern(env, "*debug-io*");
      args[1] = euser_ptr_stream_make(env, dio);
      efuncall_cached(env, syms.fn_set, 2, args);
    }
    {
      emacs_value args[2];
      args[0] = eintern(env, "*query-io*");
      args[1] = euser_ptr_stream_make(env, qio);
      efuncall_cached(env, syms.fn_set, 2, args);
    }
  } else {
    /* batch mode: alias to standard-input or standard-output */
    emacs_value sin = efuncall_cached(env, syms.fn_symbol_value, 1,
                           (emacs_value[]){ eintern(env, "*standard-input*") });
    emacs_value sout = efuncall_cached(env, syms.fn_symbol_value, 1,
                            (emacs_value[]){ eintern(env, "*standard-output*") });
    {
      emacs_value args[2];
      args[0] = eintern(env, "*terminal-io*");
      args[1] = sout;
      efuncall_cached(env, syms.fn_set, 2, args);
    }
    {
      emacs_value args[2];
      args[0] = eintern(env, "*debug-io*");
      args[1] = sout;
      efuncall_cached(env, syms.fn_set, 2, args);
    }
    {
      emacs_value args[2];
      args[0] = eintern(env, "*query-io*");
      args[1] = sin;
      efuncall_cached(env, syms.fn_set, 2, args);
    }
  }
}
