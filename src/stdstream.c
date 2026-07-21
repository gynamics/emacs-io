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
    efuncall(env, "set", 2, args);
  }

  /* standard-output: fd 1, output */
  {
    cl_stream_t *s = stream_create_fd(STDOUT_FILENO, false,
                                       CL_STREAM_FLAG_OUTPUT);
    emacs_value v = euser_ptr_stream_make(env, s);
    emacs_value args[2];
    args[0] = eintern(env, "*standard-output*");
    args[1] = v;
    efuncall(env, "set", 2, args);
  }

  /* error-output: fd 2, output */
  {
    cl_stream_t *s = stream_create_fd(STDERR_FILENO, false,
                                       CL_STREAM_FLAG_OUTPUT);
    emacs_value v = euser_ptr_stream_make(env, s);
    emacs_value args[2];
    args[0] = eintern(env, "*error-output*");
    args[1] = v;
    efuncall(env, "set", 2, args);
  }

  /* trace-output: fd 2, output (alias to error-output) */
  /* debug-io, query-io, terminal-io: bidirectional on fds 0+1 */
  /* For now, all map to *standard-input* or *standard-output* as appropriate */

  emacs_value sin = efuncall(env, "symbol-value", 1,
                              (emacs_value[]){ eintern(env, "*standard-input*") });
  emacs_value sout = efuncall(env, "symbol-value", 1,
                               (emacs_value[]){ eintern(env, "*standard-output*") });
  emacs_value serr = efuncall(env, "symbol-value", 1,
                               (emacs_value[]){ eintern(env, "*error-output*") });

  {
    emacs_value args[2];
    args[0] = eintern(env, "*trace-output*");
    args[1] = serr;
    efuncall(env, "set", 2, args);
  }
  {
    emacs_value args[2];
    args[0] = eintern(env, "*terminal-io*");
    args[1] = sout;   /* bidirectional would need two streams; output for now */
    efuncall(env, "set", 2, args);
  }
  {
    emacs_value args[2];
    args[0] = eintern(env, "*debug-io*");
    args[1] = sout;
    efuncall(env, "set", 2, args);
  }
  {
    emacs_value args[2];
    args[0] = eintern(env, "*query-io*");
    args[1] = sin;
    efuncall(env, "set", 2, args);
  }
}
