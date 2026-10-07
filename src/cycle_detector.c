#include "cycle_detector.h"
#include <string.h>
#include <stdio.h>

#define WHITE 0
#define GRAY 1
#define BLACK 2

static int dfs_visit(const graph_t *g, int u, int *color, int *parent, cycle_result_t *result) {
    color[u] = GRAY;

    for (int v = 0; v < MAX_NODES; v++) {
        if (!g->active[v] || !g->adj[u][v]) continue;

        if (color[v] == GRAY) {
            // Cycle found
            result->length = 0;
            int curr = u;
            while (curr != v && curr != -1 && result->length < MAX_CYCLE_LEN) {
                result->path[result->length++] = curr;
                curr = parent[curr];
            }
            if (result->length < MAX_CYCLE_LEN) {
                result->path[result->length++] = v;
            }
            // Reverse path to show proper order
            for (int i = 0; i < result->length / 2; i++) {
                int temp = result->path[i];
                result->path[i] = result->path[result->length - 1 - i];
                result->path[result->length - 1 - i] = temp;
            }
            // Add the closing edge (back to the first node, v)
            if (result->length < MAX_CYCLE_LEN) {
                result->path[result->length++] = v;
            }
            return 1;
        }

        if (color[v] == WHITE) {
            parent[v] = u;
            if (dfs_visit(g, v, color, parent, result)) {
                return 1;
            }
        }
    }

    color[u] = BLACK;
    return 0;
}

int detect_cycle(const graph_t *g, cycle_result_t *result) {
    int color[MAX_NODES];
    int parent[MAX_NODES];

    for (int i = 0; i < MAX_NODES; i++) {
        color[i] = WHITE;
        parent[i] = -1;
    }

    for (int i = 0; i < MAX_NODES; i++) {
        if (g->active[i] && color[i] == WHITE) {
            if (dfs_visit(g, i, color, parent, result)) {
                return 1;
            }
        }
    }

    return 0;
}

void print_cycle(const graph_t *g, const cycle_result_t *result) {
    if (result->length == 0) return;
    printf("DEADLOCK DETECTED: ");
    for (int i = 0; i < result->length; i++) {
        printf("%s", g->names[result->path[i]]);
        if (i < result->length - 1) {
            printf(" -> ");
        }
    }
    printf("\n");
}
