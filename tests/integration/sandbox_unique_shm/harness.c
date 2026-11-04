#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);
extern void list_free(void* list);

static pthread_barrier_t fork_barrier;
static char collision_name[128];

static void remove_collision(void) {
    if (collision_name[0]) shm_unlink(collision_name);
}

pid_t __wrap_aether_fork(void) {
    int rc = pthread_barrier_wait(&fork_barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        errno = rc;
        return -1;
    }
    return fork();
}

struct sandbox_call {
    void* grants;
    const char* script;
    const char* output;
    int rc;
};

static void* run_sandbox(void* raw) {
    struct sandbox_call* call = (struct sandbox_call*)raw;
    call->rc = aether_spawn_sandboxed(call->grants, call->script, call->output);
    return NULL;
}

static int write_script(const char* path) {
    static const char text[] =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$AETHER_SANDBOX_SHM\" > \"$1\"\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return -1;
    ssize_t written = write(fd, text, sizeof text - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (written != (ssize_t)(sizeof text - 1)) return -1;
    return chmod(path, 0755);
}

static int read_name(const char* path, char* name, size_t size) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(name, (int)size, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    name[strcspn(name, "\r\n")] = '\0';
    return name[0] ? 0 : -1;
}

static int expect_removed(const char* name) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0 && errno == ENOENT) return 0;
    if (fd >= 0) close(fd);
    fprintf(stderr, "FAIL shared memory remains after spawn: %s\n", name);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }

    char script[4096];
    char output_a[4096];
    char output_b[4096];
    snprintf(script, sizeof script, "%s/record-shm.sh", argv[1]);
    snprintf(output_a, sizeof output_a, "%s/a.txt", argv[1]);
    snprintf(output_b, sizeof output_b, "%s/b.txt", argv[1]);
    if (write_script(script) != 0) {
        perror("write script");
        return 1;
    }

    void* grants = list_new();
    if (!grants || !list_add_raw(grants, (void*)"*") ||
        !list_add_raw(grants, (void*)"*")) {
        fprintf(stderr, "FAIL cannot create grant list\n");
        return 1;
    }
    if (pthread_barrier_init(&fork_barrier, NULL, 2) != 0) {
        fprintf(stderr, "FAIL cannot create fork barrier\n");
        return 1;
    }

    snprintf(collision_name, sizeof collision_name,
             "/aether_sandbox_%ld_0", (long)getpid());
    int collision_fd = shm_open(collision_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (collision_fd < 0) {
        perror("create collision object");
        return 1;
    }
    close(collision_fd);
    atexit(remove_collision);

    struct sandbox_call a = { grants, script, output_a, -1 };
    struct sandbox_call b = { grants, script, output_b, -1 };
    pthread_t thread_a;
    pthread_t thread_b;
    if (pthread_create(&thread_a, NULL, run_sandbox, &a) != 0 ||
        pthread_create(&thread_b, NULL, run_sandbox, &b) != 0) {
        fprintf(stderr, "FAIL cannot create caller threads\n");
        return 1;
    }
    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);
    pthread_barrier_destroy(&fork_barrier);
    list_free(grants);

    if (a.rc != 0 || b.rc != 0) {
        fprintf(stderr, "FAIL concurrent spawns returned %d and %d\n", a.rc, b.rc);
        return 1;
    }

    char name_a[128];
    char name_b[128];
    if (read_name(output_a, name_a, sizeof name_a) != 0 ||
        read_name(output_b, name_b, sizeof name_b) != 0) {
        fprintf(stderr, "FAIL children did not record shared-memory names\n");
        return 1;
    }
    if (strcmp(name_a, name_b) == 0) {
        fprintf(stderr, "FAIL concurrent spawns shared one name: %s\n", name_a);
        return 1;
    }
    if (expect_removed(name_a) || expect_removed(name_b)) return 1;

    collision_fd = shm_open(collision_name, O_RDONLY, 0);
    if (collision_fd < 0) {
        fprintf(stderr, "FAIL spawn did not preserve an existing name: %s\n",
                collision_name);
        return 1;
    }
    close(collision_fd);

    printf("PASS: concurrent spawn_sandboxed calls use separate shared memory\n");
    return 0;
}
