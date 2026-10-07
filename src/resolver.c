/*
 * resolver.c — pick a deadlock victim and kill it with SIGKILL.
 *
 * The workers' mutexes are robust (PTHREAD_MUTEX_ROBUST), so when the
 * victim dies holding a fork, the next waiter's pthread_mutex_lock()
 * returns EOWNERDEAD and it can take the fork over — killing one process
 * genuinely unblocks the rest instead of leaving the lock stuck forever.
 */

#include "resolver.h"

#include <errno.h>
#include <signal.h>

#include "cycle_detector.h"
#include "ipc_reader.h"

static int is_waiting(const graph_t *g, int t) {
    for (int r = MAX_THREADS; r < MAX_NODES; r++)
        if (g->adj[t][r]) return 1;
    return 0;
}

static int is_holding(const graph_t *g, int t) {
    for (int r = MAX_THREADS; r < MAX_NODES; r++)
        if (g->adj[r][t]) return 1;
    return 0;
}

int resolver_pick_victim(const graph_t *g, int *in_cycle) {
    cycle_result_t cycle;
    int member[MAX_THREADS] = {0};
    int have_cycle = detect_cycle(g, &cycle);

    if (have_cycle) {
        for (int i = 0; i < cycle.length; i++)
            if (IS_THREAD_NODE(cycle.path[i])) member[cycle.path[i]] = 1;
    }
    if (in_cycle) *in_cycle = have_cycle;

    int victim = -1;
    unsigned long best_seq = 0;
    for (int t = 0; t < MAX_THREADS; t++) {
        if (!g->active[t]) continue;
        if (have_cycle && !member[t]) continue;
        if (!is_waiting(g, t) || !is_holding(g, t)) continue;

        unsigned long seq = ipc_reader_wait_seq(t);
        if (victim < 0 || seq > best_seq) {
            victim = t;
            best_seq = seq;
        }
    }
    return victim;
}

pid_t resolver_kill(graph_t *g, int thread_node) {
    pid_t pid = ipc_reader_get_pid(thread_node);
    if (pid <= 0) return -1;

    if (kill(pid, SIGKILL) < 0 && errno != ESRCH) return -1;
    graph_clear_node(g, thread_node);
    return pid;
}
