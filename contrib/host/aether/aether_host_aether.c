// aether_host_aether.c — Aether-hosts-Aether Sandbox Module
//
// Compiles Aether scripts to native binaries and runs them as
// sandboxed subprocesses using the existing aether_spawn_sandboxed()
// infrastructure (LD_PRELOAD + grants via shared memory).
//
// Data exchange via aether_shared_map (POSIX shared memory).
//
// Linux and macOS only. Windows gets stubs that return -1.

#include "aether_host_aether.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Path to the Aether compiler.
static const char* get_ae_path(void) {
  const char* env = getenv("AETHER_AE_PATH");
  if (env && env[0]) return env;
  return "ae";
}

#if defined(__linux__) || defined(__APPLE__)

#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"
/* The process's spawn discipline (#3040, #3046): its close-on-exec pipe and its fork. */
#include "../../../runtime/utils/aether_spawn.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

// List operations (from libaether.a)
extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

// Where libaether_sandbox.so is: the runtime's lookup, shared with
// spawn_sandboxed (#3177).
#include "../../../runtime/sandbox/aether_sandbox_preload_path.h"

// Serialize grants to POSIX shared memory
// Format: "category:pattern\n" lines, null-terminated
static char* serialize_grants_to_shm(void* perms) {
  if (!perms) return NULL;
  int n = list_size(perms);
  if (n <= 0 || n % 2 != 0) return NULL;

  char buf[8192];
  int pos = 0;
  for (int i = 0; i < n && pos < 8000; i += 2) {
    const char* cat = aether_string_data(list_get_raw(perms, i));
    const char* pat = aether_string_data(list_get_raw(perms, i + 1));
    if (!cat || !pat) continue;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s:%s\n", cat, pat);
  }

  char* shm_name = malloc(64);
  if (!shm_name) return NULL;
  snprintf(shm_name, 64, "/aether_host_%d", getpid());

  int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
  if (fd < 0) { free(shm_name); return NULL; }

  ftruncate(fd, pos + 1);
  void* mem = mmap(NULL, pos + 1, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    close(fd);
    shm_unlink(shm_name);
    free(shm_name);
    return NULL;
  }

  memcpy(mem, buf, pos + 1);
  munmap(mem, pos + 1);
  close(fd);
  return shm_name;
}

// ---------------------------------------------------------------
// Compile
// ---------------------------------------------------------------

int aether_host_compile(const char* script_path,
                        const char* out_path) {
  if (!script_path || !out_path) return -1;

  char cmd[2048];
  snprintf(cmd, sizeof(cmd),
           "%s build \"%s\" -o \"%s\" 2>/dev/null",
           get_ae_path(), script_path, out_path);

  int rc = system(cmd);
  if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) return 0;
  return -1;
}

// ---------------------------------------------------------------
// Core: sandboxed fork/exec with optional map + stdout capture
// ---------------------------------------------------------------

static int spawn_sandboxed_full(
    void* perms, const char* binary_path,
    uint64_t map_token, int capture, char** out_buf) {
  if (!binary_path) return -1;

  // 1. Find preload library
  char preload_path[1024];
  if (perms) {
    char tried[2048];
    if (!aether_sandbox_find_preload(preload_path, sizeof(preload_path), tried, sizeof(tried))) {
        aether_sandbox_preload_missing("[aether-host]", tried);
      return -1;
    }
  }

  // 2. Serialize grants to shm
  char* grants_shm = NULL;
  if (perms) {
    grants_shm = serialize_grants_to_shm(perms);
    if (!grants_shm) {
      fprintf(stderr,
              "[aether-host] cannot serialize grants to shm\n");
      return -1;
    }
  }

  // 3. Serialize shared map to shm (freeze inputs first)
  char* map_shm = NULL;
  if (map_token != 0) {
    aether_shared_map_freeze_inputs_by_token(map_token);
    map_shm = aether_shared_map_to_shm_by_token(map_token);
    if (!map_shm) {
      fprintf(stderr,
              "[aether-host] cannot serialize map to shm: %s\n",
              errno == E2BIG ? "the map is larger than the 64 KiB block" : strerror(errno));
      if (grants_shm) {
        shm_unlink(grants_shm); free(grants_shm);
      }
      return -1;
    }
  }

  // 4. The child's env, made here (#3200): this process's, with the
  //    preload, the grant source and the map put in. The child used to
  //    setenv them between fork and exec, which a child of a threaded
  //    parent may not do: setenv mallocs and takes the environment's lock,
  //    and a lock another thread held at the fork stays held in the child.
  //    The program is binary_path as given, as execl took it.
  char preload_var[sizeof preload_path + 16];
  char grants_var[512];
  char map_var[512];
  const char* set[3];
  int nset = 0;
  if (perms) {
    snprintf(preload_var, sizeof preload_var, "LD_PRELOAD=%s", preload_path);
    snprintf(grants_var, sizeof grants_var, "AETHER_SANDBOX_SHM=%s", grants_shm);
    set[nset++] = preload_var;
    set[nset++] = grants_var;
  }
  if (map_shm) {
    snprintf(map_var, sizeof map_var, "AETHER_MAP_SHM=%s", map_shm);
    set[nset++] = map_var;
  }
  char** env = aether_spawn_env(aether_spawn_environ(), set, nset);
  char* child_argv[] = { (char*)binary_path, NULL };

  // 5. Set up stdout pipe if capturing. Close-on-exec (#3040): a child
  //    another thread starts while it is open must not hold its write end,
  //    or the read below waits for that child too. Ours keeps it through
  //    dup2 onto stdout, which clears the flag on the copy.
  int pipefd[2] = {-1, -1};
  if (!env || (capture && aether_pipe_cloexec(pipefd) != 0)) {
    free(env);
    if (grants_shm) {
      shm_unlink(grants_shm); free(grants_shm);
    }
    if (map_shm) {
      aether_shared_map_unlink_shm(map_shm); free(map_shm);
    }
    return -1;
  }
  // A program that cannot start is an error, -1, as std.os reports one
  // (#3170), never an exit of 127, which a program that ran can give: the
  // child writes its exec errno to this close-on-exec pipe, and an exec that
  // succeeds closes it unwritten. Made after the capture pipe, which so
  // holds the lowest free descriptors.
  int errp[2];
  if (aether_pipe_cloexec(errp) != 0) {
    free(env);
    if (capture) { close(pipefd[0]); close(pipefd[1]); }
    if (grants_shm) {
      shm_unlink(grants_shm); free(grants_shm);
    }
    if (map_shm) {
      aether_shared_map_unlink_shm(map_shm); free(map_shm);
    }
    return -1;
  }

  // 6. Fork
  pid_t pid = aether_fork();
  if (pid < 0) {
    free(env);
    if (capture) { close(pipefd[0]); close(pipefd[1]); }
    close(errp[0]); close(errp[1]);
    if (grants_shm) {
      shm_unlink(grants_shm); free(grants_shm);
    }
    if (map_shm) {
      aether_shared_map_unlink_shm(map_shm); free(map_shm);
    }
    return -1;
  }

  if (pid == 0) {
    // ---- Child process ----
    // Only async-signal-safe calls from here to the exec (#3200).
    close(errp[0]);
    if (capture) {
      close(pipefd[0]);
      // Already stdout when the parent had stdout closed (#3199): dup2
      // onto itself does nothing, and the close after it closed stdout.
      if (pipefd[1] == STDOUT_FILENO) {
        fcntl(STDOUT_FILENO, F_SETFD, 0);   // kept across the exec
      } else {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
      }
    }
    execve(binary_path, child_argv, env);
    int e = errno;
    ssize_t w;
    do { w = write(errp[1], &e, sizeof e); } while (w < 0 && errno == EINTR);
    _exit(127);
  }

  // ---- Parent process ----
  free(env);
  close(errp[1]);
  int exec_errno = 0;
  ssize_t got;
  do { got = read(errp[0], &exec_errno, sizeof exec_errno); } while (got < 0 && errno == EINTR);
  close(errp[0]);
  int not_started = got == (ssize_t)sizeof exec_errno;
  if (capture) {
    close(pipefd[1]);

    size_t buf_size = 4096;
    size_t buf_used = 0;
    char* buf = malloc(buf_size);
    if (buf) {
      ssize_t n;
      while ((n = read(pipefd[0], buf + buf_used,
                       buf_size - buf_used - 1)) > 0) {
        buf_used += n;
        if (buf_used >= buf_size - 1) {
          buf_size *= 2;
          char* nb = realloc(buf, buf_size);
          if (!nb) { free(buf); buf = NULL; break; }
          buf = nb;
        }
      }
      if (buf) buf[buf_used] = '\0';
    }
    close(pipefd[0]);
    if (out_buf) *out_buf = buf ? buf : strdup("");
  }

  int status = 0;
  waitpid(pid, &status, 0);

  // 7. Read outputs back from shared map
  if (map_shm && map_token != 0) {
    // A block that is not layout 2 (a child built against another Aether,
    // or one that wrote garbage) is refused whole, and said so.
    if (aether_shared_map_read_outputs_from_shm_by_token(map_token, map_shm) != 0)
      fprintf(stderr,
              "[aether-host] the child's map block was refused (not a layout-2 Aether shared map block, or more keys than a map holds)\n");
  }

  // 8. Cleanup shm segments
  if (grants_shm) {
    shm_unlink(grants_shm); free(grants_shm);
  }
  if (map_shm) {
    aether_shared_map_unlink_shm(map_shm); free(map_shm);
  }

  if (not_started) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

int aether_host_run_sandboxed(void* perms,
                              const char* binary_path) {
  return spawn_sandboxed_full(perms, binary_path, 0, 0, NULL);
}

int aether_host_run(const char* binary_path) {
  return spawn_sandboxed_full(NULL, binary_path, 0, 0, NULL);
}

int aether_host_run_sandboxed_with_map(
    void* perms, const char* binary_path,
    uint64_t map_token) {
  return spawn_sandboxed_full(
      perms, binary_path, map_token, 0, NULL);
}

char* aether_host_capture_sandboxed(void* perms,
                                    const char* binary_path) {
  char* buf = NULL;
  spawn_sandboxed_full(perms, binary_path, 0, 1, &buf);
  return buf ? buf : strdup("");
}

// ---------------------------------------------------------------
// Script convenience wrappers (compile + run)
// ---------------------------------------------------------------

static char* make_temp_binary(const char* script_path) {
  static char tmp[512];
  snprintf(tmp, sizeof(tmp), "/tmp/aether_host_%d", getpid());
  if (aether_host_compile(script_path, tmp) != 0) return NULL;
  return tmp;
}

int aether_host_run_script_sandboxed(void* perms,
                                     const char* script_path) {
  char* bin = make_temp_binary(script_path);
  if (!bin) return -1;
  int rc = aether_host_run_sandboxed(perms, bin);
  unlink(bin);
  return rc;
}

int aether_host_run_script(const char* script_path) {
  char* bin = make_temp_binary(script_path);
  if (!bin) return -1;
  int rc = aether_host_run(bin);
  unlink(bin);
  return rc;
}

int aether_host_run_script_sandboxed_with_map(
    void* perms, const char* script_path,
    uint64_t map_token) {
  char* bin = make_temp_binary(script_path);
  if (!bin) return -1;
  int rc = aether_host_run_sandboxed_with_map(
      perms, bin, map_token);
  unlink(bin);
  return rc;
}

char* aether_host_capture_script_sandboxed(
    void* perms, const char* script_path) {
  char* bin = make_temp_binary(script_path);
  if (!bin) return strdup("");
  char* out = aether_host_capture_sandboxed(perms, bin);
  unlink(bin);
  return out;
}

#else
// ---------------------------------------------------------------
// Windows / non-POSIX stubs
// ---------------------------------------------------------------

int aether_host_compile(const char* script_path,
                        const char* out_path) {
  if (!script_path || !out_path) return -1;
  char cmd[2048];
  snprintf(cmd, sizeof(cmd),
           "%s build \"%s\" -o \"%s\" 2>NUL",
           get_ae_path(), script_path, out_path);
  return system(cmd) == 0 ? 0 : -1;
}

static int stub_err(void) {
  fprintf(stderr,
      "[aether-host] sandboxed spawn requires "
      "Linux or macOS\n");
  return -1;
}

int aether_host_run_sandboxed(void* p, const char* b) {
  (void)p; (void)b; return stub_err();
}
int aether_host_run(const char* b) {
  (void)b; return stub_err();
}
int aether_host_run_sandboxed_with_map(
    void* p, const char* b, uint64_t t) {
  (void)p; (void)b; (void)t; return stub_err();
}
char* aether_host_capture_sandboxed(void* p, const char* b) {
  (void)p; (void)b; stub_err(); return strdup("");
}
int aether_host_run_script_sandboxed(
    void* p, const char* s) {
  (void)p; (void)s; return stub_err();
}
int aether_host_run_script(const char* s) {
  (void)s; return stub_err();
}
int aether_host_run_script_sandboxed_with_map(
    void* p, const char* s, uint64_t t) {
  (void)p; (void)s; (void)t; return stub_err();
}
char* aether_host_capture_script_sandboxed(
    void* p, const char* s) {
  (void)p; (void)s; stub_err(); return strdup("");
}

#endif
