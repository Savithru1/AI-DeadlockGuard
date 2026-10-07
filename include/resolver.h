#ifndef RESOLVER_H
#define RESOLVER_H

/*
 * Victim selection for signal-based deadlock resolution.
 *
 * The victim is the hold-and-wait process (holds at least one resource and
 * is waiting for another) that started waiting most recently — the one
 * whose death loses the least work. If a cycle already exists, the victim
 * is chosen from the cycle; otherwise the kill is pre-emptive and stops the
 * cycle from ever closing.
 */

#include <sys/types.h>

#include "graph.h"

/* Returns the victim's thread node, or -1 if there is nothing to resolve.
 * *in_cycle is set to 1 if a cycle existed (victim taken from it). */
int resolver_pick_victim(const graph_t *g, int *in_cycle);

/* SIGKILL the process behind thread_node and drop its edges from the graph.
 * Returns the PID that was signalled, or -1. */
pid_t resolver_kill(graph_t *g, int thread_node);

#endif /* RESOLVER_H */
