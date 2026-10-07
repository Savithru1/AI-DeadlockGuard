/*
 * scenario.c — the dining-philosophers worker processes.
 *
 * Each philosopher is a real process created with fork(). The forks are
 * pthread mutexes placed in MAP_SHARED anonymous memory and initialised
 * PTHREAD_PROCESS_SHARED so every child locks the same objects.
 *
 * They are also PTHREAD_MUTEX_ROBUST: if the resolver SIGKILLs a
 * philosopher while it holds a fork, the next waiter's lock call returns
 * EOWNERDEAD, it marks the mutex consistent and carries on.
 *
 * Children are forked from a multi-threaded process, so they only use
 * async-signal-safe calls (no stdio, no malloc): everything they want to
 * report goes to the monitor through the FIFO.
 */

#include "scenario.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fifo_channel.h"
#include "python_bridge.h"

typedef struct {
    pthread_mutex_t forks[N_PHILOSOPHERS];
} shared_forks_t;

static shared_forks_t *shared = NULL;
static char fork_names[N_PHILOSOPHERS][16];

const char *scenario_left_fork(int i)  { return fork_names[i % N_PHILOSOPHERS]; }
const char *scenario_right_fork(int i) { return fork_names[(i + 1) % N_PHILOSOPHERS]; }

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
}

/* ── Child side ──────────────────────────────────────────────────────────── */

static void lock_fork(int fd, int run_id, int id, int which) {
    const char *name = fork_names[which];
    fifo_send_event(fd, EV_WAIT, run_id, id, name);
    if (pthread_mutex_lock(&shared->forks[which]) == EOWNERDEAD) {
        pthread_mutex_consistent(&shared->forks[which]);
        fifo_send_event(fd, EV_RECOVER, run_id, id, name);
    }
    fifo_send_event(fd, EV_HOLD, run_id, id, name);
}

static void unlock_fork(int fd, int run_id, int id, int which) {
    fifo_send_event(fd, EV_RELEASE, run_id, id, fork_names[which]);
    pthread_mutex_unlock(&shared->forks[which]);
}

static void philosopher(int run_id, int id) {
    int left  = id;
    int right = (id + 1) % N_PHILOSOPHERS;

    int fd = fifo_open_write();

    sleep_ms((long)STAGGER_MS * id);

    lock_fork(fd, run_id, id, left);
    sleep_ms(HOLD_MS);
    lock_fork(fd, run_id, id, right);     /* blocks while the neighbour eats */

    sleep_ms(EAT_MS);

    unlock_fork(fd, run_id, id, right);
    unlock_fork(fd, run_id, id, left);

    fifo_send_event(fd, EV_DONE, run_id, id, "");
    fifo_close_write(fd);
    _exit(0);
}

/* ── Parent side ─────────────────────────────────────────────────────────── */

int scenario_start(int run_id, pid_t pids[N_PHILOSOPHERS]) {
    for (int i = 0; i < N_PHILOSOPHERS; i++)
        snprintf(fork_names[i], sizeof(fork_names[i]), "fork_%d", i);

    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        shared = NULL;
        return -1;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    for (int i = 0; i < N_PHILOSOPHERS; i++)
        pthread_mutex_init(&shared->forks[i], &attr);
    pthread_mutexattr_destroy(&attr);

    for (int i = 0; i < N_PHILOSOPHERS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            for (int j = 0; j < i; j++) kill(pids[j], SIGKILL);
            return -1;
        }
        if (pid == 0) {
            /* Own process group: Ctrl+C in the menu must not reach us.
             * Unblock signals (the monitor thread runs with them blocked)
             * and drop our copy of the pipes to the Python coprocess. */
            setpgid(0, 0);
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, NULL);
            python_bridge_close_in_child();
            philosopher(run_id, i);
        }
        pids[i] = pid;
    }
    return 0;
}

void scenario_cleanup(void) {
    if (!shared) return;
    for (int i = 0; i < N_PHILOSOPHERS; i++)
        pthread_mutex_destroy(&shared->forks[i]);
    munmap(shared, sizeof(*shared));
    shared = NULL;
}
