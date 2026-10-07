#ifndef SCENARIO_H
#define SCENARIO_H

/*
 * Dining-philosophers workload: N_PHILOSOPHERS forked processes sharing
 * N_PHILOSOPHERS forks (robust, process-shared mutexes in mmap'd memory).
 *
 * Philosopher i sits down at i * STAGGER_MS, picks up its LEFT fork, holds
 * it for HOLD_MS, then reaches for its RIGHT fork. Because HOLD_MS >
 * STAGGER_MS the right neighbour already holds that fork, so philosophers
 * block one after another — the wait-for chain grows a link at a time —
 * until the last one reaches for fork_0 and closes the cycle.
 */

#include <sys/types.h>

#define N_PHILOSOPHERS 5
#define STAGGER_MS     500
#define HOLD_MS        600
#define EAT_MS         300

/* fork() the philosophers for this run. pids[] receives their PIDs.
 * Must be called after the monitor has opened the FIFO.
 * Returns 0 on success, -1 on failure (any started children are killed). */
int scenario_start(int run_id, pid_t pids[N_PHILOSOPHERS]);

/* Release the shared fork memory once every philosopher has been reaped. */
void scenario_cleanup(void);

/* Name of philosopher i's left / right fork, e.g. "fork_3". */
const char *scenario_left_fork(int i);
const char *scenario_right_fork(int i);

#endif /* SCENARIO_H */
