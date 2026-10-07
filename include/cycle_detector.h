#pragma once
#include "graph.h"

#define MAX_CYCLE_LEN MAX_NODES

typedef struct {
    int path[MAX_CYCLE_LEN];  // node indices in the cycle
    int length;               // number of nodes in the cycle
} cycle_result_t;

// Returns 1 if a cycle is found, 0 otherwise.
// If found, fills result with the cycle path.
int detect_cycle(const graph_t *g, cycle_result_t *result);

// Print a human-readable cycle description using node names from the graph.
void print_cycle(const graph_t *g, const cycle_result_t *result);
