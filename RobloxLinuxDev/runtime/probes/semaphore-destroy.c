// Replay a stale caller and slot allocation after destroy drops the table lock.
// Wrapping the atomic store inserts that interleaving into the production code.
#include <assert.h>
#include <stdio.h>
static void after_store(void *address);
#define __atomic_store_n(p, v, order) do { \
    __atomic_store_n(p, v, order); after_store((void *)(p)); \
} while (0)
#include "../shims/kqueue/semaphore.c"
#undef __atomic_store_n

static semaphore_t dying, replacement;
static int replay;
static void after_store(void *address) {
    if (address != &free_lock || !replay) return;
    replay = 0;
    assert(semaphore_signal(dying) == KERN_INVALID_ARGUMENT);
    assert(semaphore_create(mach_task_self(), &replacement, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
}

int main(void) {
    assert(semaphore_create(mach_task_self(), &dying, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
    replay = 1;
    assert(semaphore_destroy(mach_task_self(), dying) == KERN_SUCCESS);
    assert(!replay && replacement);
    // A late destroy wake must never become a permit in the replacement.
    mach_timespec_t timeout = {0, 1000000};
    kern_return_t result = semaphore_timedwait(replacement, timeout);
    if (result != KERN_OPERATION_TIMED_OUT) {
        fprintf(stderr, "FAIL replacement woke without a signal: %d\n", result);
        return 1;
    }
    assert(semaphore_destroy(mach_task_self(), replacement) == KERN_SUCCESS);
    puts("PASS semaphore destruction cannot wake a recycled slot");
    return 0;
}
