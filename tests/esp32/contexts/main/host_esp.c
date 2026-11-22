/* Two FreeRTOS tasks entering one cooperative Aether runtime, each with a
 * task context of its own (docs/design/task-execution-contexts.md), on an
 * ESP32 under ESP-IDF: the device form of
 * tests/integration/coop_task_contexts/host.c.
 *
 * app_main installs the FreeRTOS port, so its task keeps the built-in
 * context and owns the actors (it asks one). Task I, under a 16 KiB
 * account, is driven round after round past its allowance, past a deadline,
 * into a cancel from a native task that never enters Aether, and into a
 * panic. Task W, at the same priority on the same core (FreeRTOS time
 * slicing interleaves them), makes ordinary calls under its own account
 * meanwhile: each must return its value with AETHER_OK, leave nothing
 * charged to W's account, see W's identity, and a try in it must catch its
 * own panic. Then a task attaches and detaches 200 times, 50 tasks end
 * attached (their contexts go with the FreeRTOS deletion callback), and the
 * free heap must be back where it was. The native task's stack high-water
 * mark and the image's thread-local segment say what the other tasks pay:
 * nothing of Aether's is on their stacks.
 *
 * Prints RESULT lines on the console, then returns from app_main. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "libaether.h"

extern int32_t aether_grow(int32_t n);
extern int32_t aether_spin(int32_t n);
extern int32_t aether_boom(int32_t n);
extern int32_t aether_caught(int32_t n);
extern int32_t aether_ask_echo(int32_t n);
extern const void* aether_who(void);
extern void aether_ctxlib_string_release(const void* s);
extern const char* aether_string_data(const void* s);
extern size_t aether_context_size(void);   /* runtime-internal: reported, not used */
extern int aether_set_caller(const char* identity, const char** keys, const char** vals, size_t n,
                             int64_t deadline_ms);

#define ROUNDS 20
#define PRIO 5

static volatile int g_i_done = 0;
static volatile int g_i_bad = 0;
static volatile int g_w_bad = 0;
static volatile int g_w_rounds = 0;
static volatile int g_cancel_ready = 0;
static volatile int g_native_done = 0;
static AetherExecToken g_cancel_token;
static SemaphoreHandle_t g_done;
static char g_first_bad[160];

static void note_bad(const char* who, const char* what, int got) {
    if (!g_first_bad[0])
        snprintf(g_first_bad, sizeof g_first_bad, "%s %s: got %d (%s)", who, what, got, aether_last_outcome_text());
}

static int identity_is(const char* want) {
    const void* s = aether_who();
    int ok = aether_last_outcome() == AETHER_OK && s && strcmp(aether_string_data(s), want) == 0;
    if (s) aether_ctxlib_string_release(s);
    return ok;
}

static void interrupted_task(void* arg) {
    (void)arg;
    if (aether_context_attach() != 0) { g_i_bad = 1000; xSemaphoreGive(g_done); vTaskDelete(NULL); }
    AetherExecLimits lim = { 16 * 1024, 0, AETHER_DEADLINE_ELAPSED, NULL };
    AetherExecAccount* acct = aether_exec_account_new(&lim);
    if (!acct || aether_exec_use(acct, NULL) != 0) g_i_bad += 1000;
    (void)aether_set_caller("I", NULL, NULL, 0, 0);
    for (int r = 0; r < ROUNDS; r++) {
        while (g_w_rounds <= r) vTaskDelay(1);   /* W made a round more since the last */
        int v = aether_grow(100000), o = aether_last_outcome();
        if (o != AETHER_MEMORY_CAP || v != 0) { g_i_bad++; note_bad("I", "quota", o); }
        if (aether_exec_account_used(acct) != 0) { g_i_bad++; note_bad("I", "leftover", (int)aether_exec_account_used(acct)); }
        aether_set_call_deadline(20);
        (void)aether_spin(2000000000);
        o = aether_last_outcome();
        aether_set_call_deadline(0);
        if (o != AETHER_DEADLINE) { g_i_bad++; note_bad("I", "deadline", o); }
        g_cancel_token = aether_exec_token();
        g_cancel_ready = 1;
        (void)aether_spin(2000000000);
        o = aether_last_outcome();
        if (o != AETHER_CANCELLED) { g_i_bad++; note_bad("I", "cancel", o); }
        (void)aether_boom(1);
        o = aether_last_outcome();
        if (o != AETHER_PANICKED || strcmp(aether_last_outcome_text(), "boom") != 0) { g_i_bad++; note_bad("I", "panic", o); }
        if (!identity_is("I")) { g_i_bad++; note_bad("I", "identity", 0); }
        printf("I round %d done, W at %d\n", r, g_w_rounds);
        vTaskDelay(1);
    }
    aether_exec_use(NULL, NULL);
    aether_exec_account_free(acct);
    if (aether_context_detach() != 0) g_i_bad++;
    g_i_done = 1;
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

static void worker_task(void* arg) {
    (void)arg;
    if (aether_context_attach() != 0) { g_w_bad = 1000; xSemaphoreGive(g_done); vTaskDelete(NULL); }
    AetherExecLimits lim = { 0, 0, AETHER_DEADLINE_ELAPSED, NULL };
    AetherExecAccount* acct = aether_exec_account_new(&lim);
    if (!acct || aether_exec_use(acct, NULL) != 0) g_w_bad += 1000;
    (void)aether_set_caller("W", NULL, NULL, 0, 0);
    while (!g_i_done) {
        int v = aether_grow(64), o = aether_last_outcome();
        if (o != AETHER_OK || v != 1024) { g_w_bad++; note_bad("W", "grow", o); }
        if (aether_exec_account_used(acct) != 0) { g_w_bad++; note_bad("W", "leftover", (int)aether_exec_account_used(acct)); }
        v = aether_caught(1);
        o = aether_last_outcome();
        if (o != AETHER_OK || v != 7) { g_w_bad++; note_bad("W", "caught", o); }
        v = aether_spin(20000);
        o = aether_last_outcome();
        if (o != AETHER_OK || v != 99980000) { g_w_bad++; note_bad("W", "spin", o); }
        if (!identity_is("W")) { g_w_bad++; note_bad("W", "identity", 0); }
        g_w_rounds++;
        vTaskDelay(1);   /* lets the idle task run */
    }
    if (aether_exec_account_tripped(acct) != 0) { g_w_bad++; note_bad("W", "tripped", aether_exec_account_tripped(acct)); }
    aether_exec_use(NULL, NULL);
    aether_exec_account_free(acct);
    if (aether_context_detach() != 0) g_w_bad++;
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* A native task beside the Aether tasks, as a network task would be: it
 * never enters Aether (it cancels a call through its token, which needs no
 * context) and is given a small stack. */
static UBaseType_t g_native_free_words;
static void native_task(void* arg) {
    (void)arg;
    while (!g_i_done) {
        if (g_cancel_ready) {
            g_cancel_ready = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            (void)aether_exec_cancel(g_cancel_token);
        }
        vTaskDelay(1);
    }
    g_native_free_words = uxTaskGetStackHighWaterMark(NULL);
    g_native_done = 1;
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

static volatile int g_churn_bad = 0;
static void churn_task(void* arg) {
    (void)arg;
    for (int k = 0; k < 200; k++) {
        if (aether_context_attach() != 0) g_churn_bad++;
        (void)aether_exec_token();
        (void)aether_set_caller("churn", NULL, NULL, 0, 0);
        if (aether_caught(1) != 7) g_churn_bad++;
        if (aether_boom(1) != 0 || aether_last_outcome() != AETHER_PANICKED) g_churn_bad++;
        if (aether_context_detach() != 0) g_churn_bad++;
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

static void ends_attached(void* arg) {
    (void)arg;
    (void)aether_context_attach();
    (void)aether_set_caller("gone", NULL, NULL, 0, 0);
    (void)aether_exec_token();
    (void)aether_caught(1);
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);   /* attached: the deletion callback ends the context */
}

void app_main(void) {
    g_done = xSemaphoreCreateCounting(8, 0);
    if (aether_freertos_contexts_install() != 0) {
        printf("RESULT install: refused\n");
        return;
    }
    printf("RESULT actors: the owner's ask %d\n", (int)aether_ask_echo(41));
    printf("RESULT context: %u bytes a task\n", (unsigned)aether_context_size());

    xTaskCreate(native_task, "native", 2048, NULL, PRIO, NULL);
    xTaskCreate(worker_task, "ae_w", 8192, NULL, PRIO, NULL);
    xTaskCreate(interrupted_task, "ae_i", 8192, NULL, PRIO, NULL);
    for (int i = 0; i < 3; i++) xSemaphoreTake(g_done, portMAX_DELAY);
    printf("RESULT two tasks: I %d rounds, %d wrong; W %s rounds, %d wrong\n", ROUNDS, g_i_bad,
           g_w_rounds > 0 ? "some" : "no", g_w_bad);
    if (g_first_bad[0]) printf("RESULT first wrong: %s\n", g_first_bad);
    printf("RESULT native task: %u of 2048 stack bytes never used\n",
           (unsigned)(g_native_free_words * sizeof(StackType_t)));

    /* Settle, then the baseline the churn must come back to. */
    vTaskDelay(pdMS_TO_TICKS(50));
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    xTaskCreate(churn_task, "ae_churn", 8192, NULL, PRIO, NULL);
    xSemaphoreTake(g_done, portMAX_DELAY);
    for (int k = 0; k < 50; k++) {
        xTaskCreate(ends_attached, "ae_end", 8192, NULL, PRIO, NULL);
        xSemaphoreTake(g_done, portMAX_DELAY);
        vTaskDelay(2);   /* the idle task frees what a self-deleted task leaves */
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    size_t after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    printf("RESULT churn: 200 attach/detach, 50 ended attached, %d wrong, heap %s (%d bytes)\n", g_churn_bad,
           after >= before ? "back" : "lower", (int)after - (int)before);
}
