/* aether_context_freertos.c: the task-context port for ESP-IDF's FreeRTOS
 * (libaether.h, "Task execution contexts"; docs/design/
 * task-execution-contexts.md has the contract).
 *
 * A task's context is kept in one of its FreeRTOS thread-local-storage
 * pointer slots, AETHER_FREERTOS_TLS_SLOT (1 by default: ESP-IDF keeps slot
 * 0 for pthreads), so a task that never enters Aether pays that slot and
 * nothing on its stack. The slot's deletion callback ends the context of a
 * task deleted while it still has one. While another task holds a runtime
 * lock, a waiting task sleeps a tick rather than spin, so a holder of lower
 * priority runs. The project needs CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_
 * POINTERS of at least 2 (its default is 1) and
 * CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS (its default is y) to install the
 * port; without them it builds, and the install returns -1 saying so.
 *
 * The host calls aether_freertos_contexts_install() once, on the task that
 * owns the actors (app_main's, usually), before it starts any other task
 * that enters Aether, and aether_context_attach() / aether_context_detach()
 * on each such task. Compiled only for ESP-IDF builds of the cooperative
 * runtime; elsewhere this file is empty. */

#if defined(ESP_PLATFORM) && defined(AETHER_NO_THREADING)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libaether.h"

#ifndef AETHER_FREERTOS_TLS_SLOT
#define AETHER_FREERTOS_TLS_SLOT 1
#endif
#if AETHER_FREERTOS_TLS_SLOT < 1
#error "AETHER_FREERTOS_TLS_SLOT: ESP-IDF keeps thread-local pointer slot 0 for pthreads"
#endif

/* A project that does not configure the slot still builds: this file is in
 * every cooperative build's source list, and a single-task program (the
 * default sdkconfig, ESP-IDF's examples, the esp32 lane's projects) never
 * installs a port. Only the install refuses, and says what to set. It was a
 * #error, which stopped every ESP-IDF build of the runtime that had not
 * raised the slot count, contexts or not. */
#if CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS <= AETHER_FREERTOS_TLS_SLOT || \
    !CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS
#include <stdio.h>

int aether_freertos_contexts_install(void) {
    fprintf(stderr,
            "aether: task contexts need FreeRTOS thread-local pointer slot %d and its deletion "
            "callback: set CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS to at least %d and "
            "CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS=y\n",
            AETHER_FREERTOS_TLS_SLOT, AETHER_FREERTOS_TLS_SLOT + 1);
    return -1;
}

#else

static AetherExecContext* freertos_current(void) {
    return (AetherExecContext*)pvTaskGetThreadLocalStoragePointer(NULL, AETHER_FREERTOS_TLS_SLOT);
}

/* FreeRTOS calls it when a task with the slot set is deleted, from the
 * deleting task or, for a task that deleted itself, the idle task. */
static void freertos_task_deleted(int slot, void* ctx) {
    (void)slot;
    aether_context_task_ended((AetherExecContext*)ctx);
}

static int freertos_set_current(AetherExecContext* ctx) {
    vTaskSetThreadLocalStoragePointerAndDelCallback(NULL, AETHER_FREERTOS_TLS_SLOT, ctx,
                                                    ctx ? freertos_task_deleted : NULL);
    return 0;
}

static void freertos_relax(void) {
    vTaskDelay(1);
}

static const AetherContextPort k_freertos_port = {
    freertos_current,
    freertos_set_current,
    freertos_relax,
};

int aether_freertos_contexts_install(void) {
    return aether_context_port_install(&k_freertos_port);
}
#endif /* the slot and its deletion callback configured */

#else
/* ISO C wants a translation unit to declare something. */
typedef int aether_context_freertos_is_empty_here;
#endif
