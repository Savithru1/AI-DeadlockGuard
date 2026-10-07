#ifndef IPC_READER_H
#define IPC_READER_H

/*
 * Translates ipc_event_t messages into wait-for-graph operations.
 * PIDs map to thread nodes and resource names to resource nodes on first
 * sight; call ipc_reader_reset() before each run.
 */

#include "event_protocol.h"
#include "graph.h"

void ipc_reader_reset(void);

/* Apply one event to the graph. Returns the thread node it touched, or -1. */
int ipc_reader_apply(graph_t *g, const ipc_event_t *ev);

/* Real PID for a thread node, or -1. */
pid_t ipc_reader_get_pid(int thread_node);

/* Sequence number of the node's most recent WAIT (larger = more recent,
 * 0 = never waited). Used to pick the youngest waiter as victim. */
unsigned long ipc_reader_wait_seq(int thread_node);

#endif /* IPC_READER_H */
