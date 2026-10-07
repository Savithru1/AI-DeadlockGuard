#include "graph.h"
#include <string.h>
#include <stdio.h>

void graph_init(graph_t *g) {
    memset(g, 0, sizeof(graph_t));
}

void graph_set_name(graph_t *g, int node, const char *name) {
    if (node >= 0 && node < MAX_NODES) {
        strncpy(g->names[node], name, sizeof(g->names[node]) - 1);
        g->names[node][sizeof(g->names[node]) - 1] = '\0';
        if (!g->active[node]) {
            g->active[node] = 1;
            g->node_count++;
        }
    }
}

void graph_add_edge(graph_t *g, int from, int to) {
    if (from >= 0 && from < MAX_NODES && to >= 0 && to < MAX_NODES) {
        g->adj[from][to] = 1;
        if (!g->active[from]) {
            g->active[from] = 1;
            g->node_count++;
        }
        if (!g->active[to]) {
            g->active[to] = 1;
            g->node_count++;
        }
    }
}

void graph_remove_edge(graph_t *g, int from, int to) {
    if (from >= 0 && from < MAX_NODES && to >= 0 && to < MAX_NODES) {
        g->adj[from][to] = 0;
    }
}

void graph_clear_node(graph_t *g, int node) {
    if (node >= 0 && node < MAX_NODES) {
        for (int i = 0; i < MAX_NODES; i++) {
            g->adj[node][i] = 0;
            g->adj[i][node] = 0;
        }
    }
}

void graph_print(const graph_t *g) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!g->active[i]) continue;
        for (int j = 0; j < MAX_NODES; j++) {
            if (g->adj[i][j]) {
                printf("%s -> %s\n", g->names[i], g->names[j]);
            }
        }
    }
}
