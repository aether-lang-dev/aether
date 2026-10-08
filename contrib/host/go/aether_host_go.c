// aether_host_go.c — Go Language Host Module
//
// Go ships its own runtime + scheduler that can't coexist with
// another runtime in the same process. Rather than fighting that,
// this host runs Go code as a separate subprocess with the Aether
// LD_PRELOAD sandbox library injected. Same pattern as
// contrib/host/aether/, adapted for the Go toolchain.
//
// Two modes: run a pre-built binary, or shell out to `go run
// script.go`. In the second case the `go` toolchain itself runs
// under LD_PRELOAD, so its own libc calls (reading $GOROOT,
// invoking linker, etc.) are sandbox-checked. That's either useful
// or annoying depending on the use case — for defense-in-depth,
// pre-build the binary and use run_sandboxed to narrow the grant
// surface to just what the Go program itself does.
//
// Linux-only for real enforcement. LD_PRELOAD doesn't work under
// macOS System Integrity Protection; on macOS the binary runs
// without preload and the grants are advisory only (in-process
// checker has nothing to intercept).

#include "aether_host_go.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)

#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// -----------------------------------------------------------------
// Locate the `go` toolchain. Honour $GO_BIN if set, otherwise fall
// back to "go" (relying on $PATH at exec time).
// -----------------------------------------------------------------
static const char* get_go_path(void) {
    const char* env = getenv("GO_BIN");
    if (env && env[0]) return env;
    return "go";
}

// -----------------------------------------------------------------
// Find libaether_sandbox.so next to the running binary. Identical
// logic to contrib/host/aether — kept duplicated so each host is
// self-contained and can evolve independently.
// -----------------------------------------------------------------
static int find_preload_path(char* buf, int bufsize) {
    char exe[512];
    ssize_t len = 0;
#ifdef __linux__
    len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
#endif
    if (len <= 0) {
        snprintf(buf, bufsize, "./libaether_sandbox.so");
        return access(buf, F_OK) == 0;
    }
    exe[len] = '\0';

    char* slash = strrchr(exe, '/');
    if (slash) *(slash + 1) = '\0';

    snprintf(buf, bufsize, "%slibaether_sandbox.so", exe);
    if (access(buf, F_OK) == 0) return 1;

    if (slash) *slash = '\0';
    slash = strrchr(exe, '/');
    if (slash) *(slash + 1) = '\0';
    snprintf(buf, bufsize, "%sbuild/libaether_sandbox.so", exe);
    if (access(buf, F_OK) == 0) return 1;

    return 0;
}

// -----------------------------------------------------------------
// Serialize grant list to shared memory for the child's sandbox
// preload library to pick up. Format matches what
// libaether_sandbox_preload.c expects: "category:pattern\n" lines.
// -----------------------------------------------------------------
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
    snprintf(shm_name, 64, "/aether_host_go_%d", getpid());

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

// -----------------------------------------------------------------
// Core: fork + exec with sandbox preload.
// argv is NULL-terminated. program is argv[0]; the exec is execvp
// so PATH resolution works for "go" without an absolute path.
// -----------------------------------------------------------------
static int spawn_go_sandboxed(void* perms,
                              const char* program,
                              char* const argv[],
                              uint64_t map_token) {
    if (!program || !argv) return -1;

    // Preload library.
    char preload_path[1024];
    if (perms) {
        if (!find_preload_path(preload_path, sizeof(preload_path))) {
            fprintf(stderr,
                    "[go-host] cannot find libaether_sandbox.so\n");
            return -1;
        }
    }

    // Grants → shm.
    char* grants_shm = NULL;
    if (perms) {
        grants_shm = serialize_grants_to_shm(perms);
        if (!grants_shm) {
            fprintf(stderr,
                    "[go-host] cannot serialize grants to shm\n");
            return -1;
        }
    }

    // Shared map → shm.
    char* map_shm = NULL;
    if (map_token != 0) {
        extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
        aether_shared_map_freeze_inputs_by_token(map_token);
        map_shm = aether_shared_map_to_shm_by_token(map_token);
        if (!map_shm) {
            fprintf(stderr,
                    "[go-host] cannot serialize map to shm\n");
            if (grants_shm) {
                shm_unlink(grants_shm); free(grants_shm);
            }
            return -1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (grants_shm) { shm_unlink(grants_shm); free(grants_shm); }
        if (map_shm) {
            aether_shared_map_unlink_shm(map_shm); free(map_shm);
        }
        return -1;
    }

    if (pid == 0) {
        // Child.
        if (perms) {
            setenv("LD_PRELOAD", preload_path, 1);
            setenv("AETHER_SANDBOX_SHM", grants_shm, 1);
        }
        if (map_shm) setenv("AETHER_MAP_SHM", map_shm, 1);
        execvp(program, argv);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (map_shm && map_token != 0) {
        aether_shared_map_read_outputs_from_shm_by_token(map_token, map_shm);
    }
    if (grants_shm) { shm_unlink(grants_shm); free(grants_shm); }
    if (map_shm) {
        aether_shared_map_unlink_shm(map_shm); free(map_shm);
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// -----------------------------------------------------------------
// Public API
// -----------------------------------------------------------------

int go_run_sandboxed(void* perms, const char* binary_path) {
    if (!binary_path) return -1;
    char* argv[] = { (char*)binary_path, NULL };
    return spawn_go_sandboxed(perms, binary_path, argv, 0);
}

int go_run(const char* binary_path) {
    return go_run_sandboxed(NULL, binary_path);
}

int go_run_script_sandboxed(void* perms, const char* script_path) {
    if (!script_path) return -1;
    const char* go_bin = get_go_path();
    char* argv[] = { (char*)go_bin, "run", (char*)script_path, NULL };
    return spawn_go_sandboxed(perms, go_bin, argv, 0);
}

int go_run_script(const char* script_path) {
    return go_run_script_sandboxed(NULL, script_path);
}

int go_run_sandboxed_with_map(void* perms, const char* binary_path, uint64_t map_token) {
    if (!binary_path) return -1;
    char* argv[] = { (char*)binary_path, NULL };
    return spawn_go_sandboxed(perms, binary_path, argv, map_token);
}

int go_run_script_sandboxed_with_map(void* perms, const char* script_path, uint64_t map_token) {
    if (!script_path) return -1;
    const char* go_bin = get_go_path();
    char* argv[] = { (char*)go_bin, "run", (char*)script_path, NULL };
    return spawn_go_sandboxed(perms, go_bin, argv, map_token);
}

#else

// Non-POSIX platforms (Windows): stubs that fail with a clear message.
int go_run_sandboxed(void* perms, const char* binary_path) {
    (void)perms; (void)binary_path;
    fprintf(stderr, "error: contrib.host.go requires POSIX (fork/exec/shm_open)\n");
    return -1;
}
int go_run_script_sandboxed(void* perms, const char* script_path) {
    (void)perms; (void)script_path;
    return go_run_sandboxed(NULL, NULL);
}
int go_run(const char* binary_path) {
    (void)binary_path;
    return go_run_sandboxed(NULL, NULL);
}
int go_run_script(const char* script_path) {
    (void)script_path;
    return go_run_sandboxed(NULL, NULL);
}
int go_run_sandboxed_with_map(void* perms, const char* binary_path, uint64_t map_token) {
    (void)perms; (void)binary_path; (void)map_token;
    return go_run_sandboxed(NULL, NULL);
}
int go_run_script_sandboxed_with_map(void* perms, const char* script_path, uint64_t map_token) {
    (void)perms; (void)script_path; (void)map_token;
    return go_run_sandboxed(NULL, NULL);
}

#endif
