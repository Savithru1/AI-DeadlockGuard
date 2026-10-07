/*
 * ipc_reader.c — FIFO events -> wait-for graph.
 *
 * The only file that knows both the IPC protocol and the graph API.
 * Edge convention (resource allocation graph):
 *   thread -> resource   thread is waiting for the resource
 *   resource -> thread   resource is held by the thread
 */

#include "ipc_reader.h"

#include <stdio.h>
#include <string.h>

/* ── PID -> thread-node table ────────────────────────────────────────────── */
static pid_t         pid_table[MAX_THREADS];
static unsigned long wait_seq[MAX_THREADS];
static unsigned long wait_seq_counter = 0;
static int           pid_count = 0;

static int pid_to_node(graph_t *g, pid_t pid) {
    for (int i = 0; i < pid_count; i++) {
        if (pid_table[i] == pid) return THREAD_NODE(i);
    }
    if (pid_count >= MAX_THREADS) return -1;

    int idx = pid_count++;
    pid_table[idx] = pid;
    char name[32];
    snprintf(name, sizeof(name), "pid_%d", (int)pid);
    graph_set_name(g, THREAD_NODE(idx), name);
    return THREAD_NODE(idx);
}

/* ── Resource-name -> resource-node table ────────────────────────────────── */
static char res_table[MAX_RESOURCES][MAX_RESOURCE_NAME];
static int  res_count = 0;

static int resource_to_node(graph_t *g, const char *name) {
    for (int i = 0; i < res_count; i++) {
        if (strncmp(res_table[i], name, MAX_RESOURCE_NAME) == 0)
            return RESOURCE_NODE(i);
    }
    if (res_count >= MAX_RESOURCES) return -1;

    int idx = res_count++;
    snprintf(res_table[idx], MAX_RESOURCE_NAME, "%.*s", MAX_RESOURCE_NAME - 1, name);
    graph_set_name(g, RESOURCE_NODE(idx), name);
    return RESOURCE_NODE(idx);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ipc_reader_reset(void) {
    pid_count = 0;
    res_count = 0;
    wait_seq_counter = 0;
    memset(pid_table, 0, sizeof(pid_table));
    memset(wait_seq,  0, sizeof(wait_seq));
    memset(res_table, 0, sizeof(res_table));
}

int ipc_reader_apply(graph_t *g, const ipc_event_t *ev) {
    int t_node = pid_to_node(g, ev->pid);
    if (t_node < 0) return -1;

    if (ev->type == EV_DONE) {
        graph_clear_node(g, t_node);
        return t_node;
    }
    if (ev->type == EV_RECOVER) return t_node;   /* followed by EV_HOLD */

    int r_node = resource_to_node(g, ev->resource);
    if (r_node < 0) return -1;

    switch (ev->type) {
        case EV_WAIT:
            graph_add_edge(g, t_node, r_node);
            wait_seq[t_node] = ++wait_seq_counter;
            break;
        case EV_HOLD:
            /* A dead previous owner may still have its hold edge; ownership
             * is exclusive, so drop every other hold on this resource. */
            for (int t = 0; t < MAX_THREADS; t++) g->adj[r_node][t] = 0;
            graph_remove_edge(g, t_node, r_node);
            graph_add_edge(g, r_node, t_node);
            break;
        case EV_RELEASE:
            graph_remove_edge(g, r_node, t_node);
            break;
        default:
            return -1;
    }
    return t_node;
}

pid_t ipc_reader_get_pid(int thread_node) {
    if (thread_node < 0 || thread_node >= pid_count) return -1;
    return pid_table[thread_node];
}

unsigned long ipc_reader_wait_seq(int thread_node) {
    if (thread_node < 0 || thread_node >= pid_count) return 0;
    return wait_seq[thread_node];
}
