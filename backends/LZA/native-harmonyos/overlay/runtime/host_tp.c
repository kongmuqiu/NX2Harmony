/* Host thread slots, see host_tp.h. */
#include "host_tp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>

/* Linux / Android only: on Darwin the slots are pthread keys and loader.c handles them as it always has. */
#if defined(TOTK_OHOS)
#include <stdio.h>
/* HarmonyOS (host_tp.h): the slots are this library's own __thread array, addressed from the thread pointer. musl gives
   a thread created after the library was loaded the library's TLS block at a fixed TP offset; it is measured once on the
   runtime's first thread and checked on every thread that runs guest code. A thread that existed before the library
   was loaded (the ArkTS thread) has the block elsewhere and is refused. */
uint64_t runtime_slot_off[RUNTIME_NSLOTS];
pthread_key_t runtime_keys[8];   /* unused, as on Linux */
static __thread uint64_t ohos_slots[RUNTIME_NSLOTS] __attribute__((aligned(16)));
static intptr_t slots_off;

static intptr_t tp_offset_here(void) {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (!tp || (tp & 7)) runtime_fail("unusable TPIDR_EL0 thread pointer");
    return (intptr_t)((uintptr_t)&ohos_slots[0] - tp);
}

void runtime_slots_process_init(void) {
    slots_off = tp_offset_here();
    if (slots_off < 0 || slots_off & 7) runtime_fail("libtotk_ohos.so's TLS block is not above the thread pointer");
    for (unsigned i = 0; i < RUNTIME_NSLOTS; i++) runtime_slot_off[i] = (uint64_t)slots_off + i * 8;
    fprintf(stderr, "[totk] thread slots: libtotk_ohos.so TLS at TP%+ld\n", (long)slots_off);
}

void runtime_slots_thread_attach(void) {
    if (!slots_off) runtime_fail("thread slots used before runtime_slots_process_init");
    intptr_t here = tp_offset_here();
    if (here != slots_off) {
        fprintf(stderr, "[totk] thread slots at TP%+ld on this thread, TP%+ld on the first\n", (long)here, (long)slots_off);
        runtime_fail("this thread's TLS layout differs (created before libtotk_ohos.so was loaded?)");
    }
}

void runtime_slots_thread_detach(void) { memset(ohos_slots, 0, sizeof ohos_slots); }

void runtime_slot_set(unsigned i, void *value) {
    if (i >= RUNTIME_NSLOTS) runtime_fail("slot index");
    *(volatile uint64_t *)(runtime_host_slots() + runtime_slot_off[i]) = (uint64_t)(uintptr_t)value;
}
void *runtime_slot_get(unsigned i) {
    return i < RUNTIME_NSLOTS ? (void *)(uintptr_t)*(volatile uint64_t *)(runtime_host_slots() + runtime_slot_off[i]) : NULL;
}

#elif !defined(__APPLE__)

uint64_t runtime_slot_off[RUNTIME_NSLOTS];

/* Unused on Linux: the key page holds slot-block offsets (runtime_slot_off), not pthread keys. Kept so code shared
   with Darwin still links. */
pthread_key_t runtime_keys[8];

#if !defined(__BIONIC__)
/* glibc: TPIDR_EL0 points at the 16-byte TCB, followed by the executable's TLS block. An initialized local-exec word
   lands in .tdata, ahead of every zero-initialized __thread variable (.tbss); runtime_slots_thread_attach checks that
   it really sits at TP + 16. */
__attribute__((tls_model("local-exec"), used)) __thread uintptr_t runtime_tp_app_word = 1;
#endif

static pthread_key_t block_key;   /* this thread's slot block, freed at thread exit */

static uintptr_t *tp_word(void) {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (!tp || (tp & 7)) runtime_fail("unusable TPIDR_EL0 thread pointer");
    return (uintptr_t *)(tp + RUNTIME_SLOT_PTR_OFF);
}

static void block_free(void *p) {
    if (!p) return;
    uintptr_t *w = tp_word();
    if (*w == (uintptr_t)p) *w = 0;
    free(p);
}

void runtime_slots_process_init(void) {
    for (unsigned i = 0; i < RUNTIME_NSLOTS; i++) runtime_slot_off[i] = (uint64_t)i * 8;
    if (pthread_key_create(&block_key, block_free)) runtime_fail("slot block key");
}

void runtime_slots_thread_attach(void) {
    uintptr_t *w = tp_word();
#if defined(__BIONIC__)
    /* TLS_SLOT_APP is free for apps; nothing else in the process may be using it on a thread the runtime owns. */
    if (*w && *w != (uintptr_t)pthread_getspecific(block_key)) runtime_fail("bionic TLS_SLOT_APP already in use");
#else
    if (w != &runtime_tp_app_word) runtime_fail("glibc TLS layout: the slot-block word is not at TPIDR_EL0 + 16");
#endif
    if (pthread_getspecific(block_key)) return;
    uint64_t *block = calloc(RUNTIME_NSLOTS, sizeof *block);
    if (!block) runtime_fail("slot block allocation");
    if (pthread_setspecific(block_key, block)) runtime_fail("slot block ownership");
    *w = (uintptr_t)block;
}

void runtime_slots_thread_detach(void) {
    void *block = pthread_getspecific(block_key);
    pthread_setspecific(block_key, NULL);
    block_free(block);
}

void runtime_slot_set(unsigned i, void *value) {
    if (i >= RUNTIME_NSLOTS) runtime_fail("slot index");
    uint64_t *block = (uint64_t *)runtime_host_slots();
    if (!block) runtime_fail("thread slots used before runtime_slots_thread_attach");
    block[i] = (uint64_t)(uintptr_t)value;
}
void *runtime_slot_get(unsigned i) {
    uint64_t *block = (uint64_t *)runtime_host_slots();
    return block && i < RUNTIME_NSLOTS ? (void *)(uintptr_t)block[i] : NULL;
}

#endif
