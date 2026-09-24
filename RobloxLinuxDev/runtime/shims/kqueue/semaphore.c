// Mach semaphores that stay inside this process. Darling turns every
// semaphore_* call into a darlingserver RPC (~50 us round trip, far more
// under load), and libdispatch, FMOD's mixer and WebRTC signal them
// constantly. Semaphores created here live on Linux futexes instead. Names
// this table did not create keep the original RPC implementation.
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>

// Names never collide with XNU's (index << 8 | generation): XNU indexes stay
// far below bit 22. Layout: tag | generation(10 bits) | slot(12 bits) | 0.
enum { tag = 0x40000000, slot_count = 4096, generation_mask = 0x3ff };

struct local_semaphore {
    int count;          // >= 0 available permits; < 0: -(ungranted waiters)
    unsigned wakeups;   // futex word: grants not yet consumed by a waiter
    unsigned users;     // threads inside an operation; a slot is never reused under them
    unsigned generation;
    int dead;
    int released;
};
static struct local_semaphore table[slot_count];
static unsigned free_slots[slot_count];
static unsigned free_count, next_unused;
static int free_lock;

static long linux_futex(unsigned *word, int op, unsigned value, const struct timespec *timeout) {
    long result;
    register long r10 __asm__("r10") = (long)timeout;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(202L), "D"(word), "S"((long)op), "d"((long)value), "r"(r10)
        : "rcx", "r11", "memory", "cc");
    return result;
}
enum { futex_wait_private = 128, futex_wake_private = 129, linux_eintr = 4, linux_etimedout = 110 };

static void lock_free(void) { while (__atomic_exchange_n(&free_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void unlock_free(void) { __atomic_store_n(&free_lock, 0, __ATOMIC_RELEASE); }

static struct local_semaphore *lookup(semaphore_t name, unsigned *generation) {
    if ((name & 0xc00000ff) != tag) return NULL;
    struct local_semaphore *s = &table[(name >> 8) & (slot_count - 1)];
    *generation = (name >> 20) & generation_mask;
    return s;
}
static int current(struct local_semaphore *s, unsigned generation) {
    return !__atomic_load_n(&s->dead, __ATOMIC_ACQUIRE) &&
        (__atomic_load_n(&s->generation, __ATOMIC_ACQUIRE) & generation_mask) == generation;
}
// A destroyed slot returns to the free list once, after its last user left.
static void release_if_idle(struct local_semaphore *s) {
    int expected = 0;
    if (!__atomic_load_n(&s->dead, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->users, __ATOMIC_ACQUIRE) ||
        !__atomic_compare_exchange_n(&s->released, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    lock_free();
    free_slots[free_count++] = (unsigned)(s - table);
    unlock_free();
}
// Enter before checking the generation: release waits for every user, so an
// operation that passed the check can never touch a reused slot.
static int enter(struct local_semaphore *s, unsigned generation) {
    __atomic_add_fetch(&s->users, 1, __ATOMIC_ACQ_REL);
    return current(s, generation);
}
static void leave(struct local_semaphore *s) {
    if (!__atomic_sub_fetch(&s->users, 1, __ATOMIC_ACQ_REL)) release_if_idle(s);
}

#define ORIGINAL(name, type) \
    static __typeof__(type) original; __typeof__(type) call = __atomic_load_n(&original, __ATOMIC_ACQUIRE); \
    if (!call) { call = (__typeof__(type))dlsym(RTLD_NEXT, name); __atomic_store_n(&original, call, __ATOMIC_RELEASE); }

kern_return_t semaphore_create(task_t task, semaphore_t *semaphore, int policy, int value) {
    if (task == mach_task_self() && semaphore && value >= 0) {
        unsigned slot = slot_count;
        lock_free();
        if (free_count) slot = free_slots[--free_count];
        else if (next_unused < slot_count) slot = next_unused++;
        unlock_free();
        if (slot < slot_count) {
            struct local_semaphore *s = &table[slot];
            __atomic_store_n(&s->count, value, __ATOMIC_RELAXED);
            __atomic_store_n(&s->wakeups, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&s->released, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&s->dead, 0, __ATOMIC_RELEASE);
            *semaphore = tag | ((__atomic_load_n(&s->generation, __ATOMIC_RELAXED) & generation_mask) << 20) | slot << 8;
            return KERN_SUCCESS;
        }
    }
    // ponytail: 4096 live semaphores, then the RPC path; grow the table if
    // a workload ever holds more at once.
    ORIGINAL("semaphore_create", kern_return_t (*)(task_t, semaphore_t *, int, int));
    return call(task, semaphore, policy, value);
}

kern_return_t semaphore_destroy(task_t task, semaphore_t semaphore) {
    unsigned generation;
    struct local_semaphore *s = lookup(semaphore, &generation);
    if (!s) {
        ORIGINAL("semaphore_destroy", kern_return_t (*)(task_t, semaphore_t));
        return call(task, semaphore);
    }
    if (task != mach_task_self()) return KERN_INVALID_ARGUMENT;
    // The generation bump makes the name stale; waiters return KERN_TERMINATED.
    lock_free();
    int alive = current(s, generation);
    if (alive) {
        // Keep the slot until the final wake; a stale caller can otherwise
        // recycle it as soon as dead is set and wake a replacement semaphore.
        __atomic_add_fetch(&s->users, 1, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&s->generation, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s->dead, 1, __ATOMIC_RELEASE);
    }
    unlock_free();
    if (!alive) return KERN_INVALID_ARGUMENT;
    __atomic_add_fetch(&s->wakeups, 1, __ATOMIC_RELEASE);
    linux_futex(&s->wakeups, futex_wake_private, INT_MAX, NULL);
    leave(s);
    return KERN_SUCCESS;
}

static void grant(struct local_semaphore *s, unsigned count) {
    __atomic_add_fetch(&s->wakeups, count, __ATOMIC_RELEASE);
    linux_futex(&s->wakeups, futex_wake_private, count, NULL);
}

kern_return_t semaphore_signal(semaphore_t semaphore) {
    unsigned generation;
    struct local_semaphore *s = lookup(semaphore, &generation);
    if (!s) {
        ORIGINAL("semaphore_signal", kern_return_t (*)(semaphore_t));
        return call(semaphore);
    }
    if (!enter(s, generation)) { leave(s); return KERN_INVALID_ARGUMENT; }
    if (__atomic_fetch_add(&s->count, 1, __ATOMIC_ACQ_REL) < 0) grant(s, 1);
    leave(s);
    return KERN_SUCCESS;
}

kern_return_t semaphore_signal_all(semaphore_t semaphore) {
    unsigned generation;
    struct local_semaphore *s = lookup(semaphore, &generation);
    if (!s) {
        ORIGINAL("semaphore_signal_all", kern_return_t (*)(semaphore_t));
        return call(semaphore);
    }
    if (!enter(s, generation)) { leave(s); return KERN_INVALID_ARGUMENT; }
    // XNU: release every current waiter; with none waiting the count is kept.
    int count = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    while (count < 0)
        if (__atomic_compare_exchange_n(&s->count, &count, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            grant(s, (unsigned)-count);
            break;
        }
    leave(s);
    return KERN_SUCCESS;
}

kern_return_t semaphore_signal_thread(semaphore_t semaphore, thread_t thread) {
    unsigned generation;
    struct local_semaphore *s = lookup(semaphore, &generation);
    if (!s) {
        ORIGINAL("semaphore_signal_thread", kern_return_t (*)(semaphore_t, thread_t));
        return call(semaphore, thread);
    }
    if (thread != MACH_PORT_NULL) return KERN_FAILURE; // no directed wakeup, as before
    if (!enter(s, generation)) { leave(s); return KERN_INVALID_ARGUMENT; }
    kern_return_t result = KERN_NOT_WAITING;
    int count = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    while (count < 0)
        if (__atomic_compare_exchange_n(&s->count, &count, count + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            grant(s, 1);
            result = KERN_SUCCESS;
            break;
        }
    leave(s);
    return result;
}

// Darling's Darwin CLOCK_MONOTONIC derives from kern.boottime and can step;
// mach_absolute_time is the Linux monotonic clock in nanoseconds (timebase 1/1).
static uint64_t monotonic_ns(void) { return mach_absolute_time(); }

// timeout_ns < 0 waits forever.
static kern_return_t wait_local(struct local_semaphore *s, unsigned generation, int64_t timeout_ns) {
    if (!enter(s, generation)) { leave(s); return KERN_INVALID_ARGUMENT; }
    int count = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    while (count > 0)
        if (__atomic_compare_exchange_n(&s->count, &count, count - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            leave(s); return KERN_SUCCESS;
        }
    if (!timeout_ns) { leave(s); return KERN_OPERATION_TIMED_OUT; }
    // Queue as a waiter; a racing signal may already have covered us.
    if (__atomic_sub_fetch(&s->count, 1, __ATOMIC_ACQ_REL) >= 0) { leave(s); return KERN_SUCCESS; }
    uint64_t deadline = timeout_ns > 0 ? monotonic_ns() + (uint64_t)timeout_ns : 0;
    int withdrawn = 0;
    for (;;) {
        if (!current(s, generation)) { leave(s); return KERN_TERMINATED; }
        unsigned wakeups = __atomic_load_n(&s->wakeups, __ATOMIC_ACQUIRE);
        while (wakeups)
            if (__atomic_compare_exchange_n(&s->wakeups, &wakeups, wakeups - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                leave(s); return KERN_SUCCESS;
            }
        struct timespec remaining, *timeout = NULL;
        if (deadline && !withdrawn) {
            uint64_t now = monotonic_ns();
            if (now >= deadline) {
                // Withdraw while we are still counted as waiting. If a signal
                // already counted us, its grant is on the way: wait for it.
                int count = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
                while (count < 0)
                    if (__atomic_compare_exchange_n(&s->count, &count, count + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                        leave(s); return KERN_OPERATION_TIMED_OUT;
                    }
                withdrawn = 1;
                continue;
            }
            remaining.tv_sec = (time_t)((deadline - now) / 1000000000ull);
            remaining.tv_nsec = (long)((deadline - now) % 1000000000ull);
            timeout = &remaining;
        }
        linux_futex(&s->wakeups, futex_wait_private, 0, timeout); // EAGAIN/EINTR/timeout recheck
    }
}

kern_return_t semaphore_wait(semaphore_t semaphore) {
    unsigned generation;
    struct local_semaphore *s = lookup(semaphore, &generation);
    if (!s) {
        ORIGINAL("semaphore_wait", kern_return_t (*)(semaphore_t));
        return call(semaphore);
    }
    return wait_local(s, generation, -1);
}

kern_return_t semaphore_timedwait(semaphore_t semaphore, mach_timespec_t wait_time) {
    unsigned generation;
    struct local_semaphore *s = lookup(semaphore, &generation);
    if (!s) {
        ORIGINAL("semaphore_timedwait", kern_return_t (*)(semaphore_t, mach_timespec_t));
        return call(semaphore, wait_time);
    }
    if (wait_time.tv_nsec < 0 || wait_time.tv_nsec >= 1000000000) return KERN_INVALID_VALUE;
    return wait_local(s, generation, (int64_t)wait_time.tv_sec * 1000000000 + wait_time.tv_nsec); // tv_sec is 32-bit
}

// Not atomic with the wait, unlike XNU; only ordering hints depend on that.
kern_return_t semaphore_wait_signal(semaphore_t wait_semaphore, semaphore_t signal_semaphore) {
    unsigned generation;
    if (!lookup(wait_semaphore, &generation) && !lookup(signal_semaphore, &generation)) {
        ORIGINAL("semaphore_wait_signal", kern_return_t (*)(semaphore_t, semaphore_t));
        return call(wait_semaphore, signal_semaphore);
    }
    kern_return_t result = semaphore_signal(signal_semaphore);
    return result == KERN_SUCCESS ? semaphore_wait(wait_semaphore) : result;
}

kern_return_t semaphore_timedwait_signal(semaphore_t wait_semaphore, semaphore_t signal_semaphore, mach_timespec_t wait_time) {
    unsigned generation;
    if (!lookup(wait_semaphore, &generation) && !lookup(signal_semaphore, &generation)) {
        ORIGINAL("semaphore_timedwait_signal", kern_return_t (*)(semaphore_t, semaphore_t, mach_timespec_t));
        return call(wait_semaphore, signal_semaphore, wait_time);
    }
    kern_return_t result = semaphore_signal(signal_semaphore);
    return result == KERN_SUCCESS ? semaphore_timedwait(wait_semaphore, wait_time) : result;
}

// libc's nanosleep() and libpthread's custom-stack join wait on semaphores
// they created above, through __semwait_signal: Darling's implementation
// calls the kernel traps internally, which cannot see local names, so every
// sleep would return at once. Serve local semaphores here with XNU's
// semantics: signal mutex_sem, then wait on cond_sem (relative or absolute
// realtime timeout). Returns 0 or -1 with errno, like the syscall wrapper.
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
typedef int (*semwait_fn)(int, int, int, int, int64_t, int32_t);
static int semwait_signal(semwait_fn original, int cond, int mutex, int timeout, int relative, int64_t sec, int32_t nsec) {
    unsigned cond_generation, mutex_generation;
    struct local_semaphore *s = lookup((semaphore_t)cond, &cond_generation);
    if (mutex && (s || lookup((semaphore_t)mutex, &mutex_generation))) {
        // Local on either side: signal here, so the rest is a plain wait.
        if (semaphore_signal((semaphore_t)mutex) != KERN_SUCCESS) { errno = EINVAL; return -1; }
        mutex = 0;
    }
    if (!s) return original(cond, mutex, timeout, relative, sec, nsec);
    int64_t wait_ns = -1;
    if (timeout) {
        if (sec < 0 || nsec < 0 || nsec >= 1000000000) { errno = EINVAL; return -1; }
        // Beyond ~30 years is forever; also keeps the arithmetic from overflowing.
        wait_ns = sec > 1000000000 ? -1 : sec * 1000000000 + nsec;
        if (!relative && wait_ns >= 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            wait_ns -= (int64_t)now.tv_sec * 1000000000 + (int64_t)now.tv_usec * 1000;
            if (wait_ns < 0) wait_ns = 0;
        }
    }
    kern_return_t result = wait_local(s, cond_generation, wait_ns);
    if (result == KERN_SUCCESS) return 0;
    errno = result == KERN_OPERATION_TIMED_OUT ? ETIMEDOUT : EINVAL;
    return -1;
}
int __semwait_signal(int cond, int mutex, int timeout, int relative, int64_t sec, int32_t nsec) {
    pthread_testcancel(); // cancellation point, like the original
    ORIGINAL("__semwait_signal", semwait_fn);
    return semwait_signal(call, cond, mutex, timeout, relative, sec, nsec);
}
int __semwait_signal_nocancel(int cond, int mutex, int timeout, int relative, int64_t sec, int32_t nsec) {
    ORIGINAL("__semwait_signal_nocancel", semwait_fn);
    return semwait_signal(call, cond, mutex, timeout, relative, sec, nsec);
}

// A thread exiting from a custom stack hands its join semaphore to the
// kernel, which signals it as the thread terminates. Darling signals first
// and then exits; do the same for local semaphores, then terminate without one.
// A joiner may already have freed this thread's pthread_t (and with it the
// TSD dlsym relies on), so the original is resolved at load, never here.
typedef int (*terminate_fn)(void *, size_t, mach_port_t, semaphore_t);
static terminate_fn original_terminate;
__attribute__((constructor)) static void resolve_terminate(void) {
    original_terminate = (terminate_fn)dlsym(RTLD_NEXT, "__bsdthread_terminate");
}
int __bsdthread_terminate(void *stack, size_t size, mach_port_t thread, semaphore_t join) {
    unsigned generation;
    if (join && lookup(join, &generation)) {
        semaphore_signal(join);
        join = MACH_PORT_NULL;
    }
    return original_terminate(stack, size, thread, join);
}
