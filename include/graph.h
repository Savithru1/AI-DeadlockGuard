#pragma once

#define MAX_NODES 64
// Convention: thread nodes = 0..31, resource nodes = 32..63
#define MAX_THREADS 32
#define MAX_RESOURCES 32
#define THREAD_NODE(id) (id)          // maps a thread index to a node index
#define RESOURCE_NODE(id) ((id) + MAX_THREADS)  // maps a resource index to a node index
#define IS_THREAD_NODE(n) ((n) < MAX_THREADS)
#define IS_RESOURCE_NODE(n) ((n) >= MAX_THREADS && (n) < MAX_NODES)

typedef struct {
    int adj[MAX_NODES][MAX_NODES];  // adjacency matrix: adj[i][j]=1 means edge i→j
    int active[MAX_NODES];          // whether this node is in use
    char names[MAX_NODES][32];      // human-readable name for each node
    int node_count;                 // for informational purposes
} graph_t;

void graph_init(graph_t *g);
void graph_set_name(graph_t *g, int node, const char *name);
void graph_add_edge(graph_t *g, int from, int to);
void graph_remove_edge(graph_t *g, int from, int to);
void graph_clear_node(graph_t *g, int node); // remove all edges involving this node
void graph_print(const graph_t *g);           // debug print
