#ifndef FEATURE_EXTRACTOR_H
#define FEATURE_EXTRACTOR_H

/*
 * The four live graph metrics the AI model was trained on, plus the raw
 * wait-edge count needed to compute the next growth rate.
 */

#include "graph.h"

typedef struct {
    int    blocked_count;     /* processes with at least one WAIT edge          */
    double wait_time_growth;  /* new WAIT edges per second (smoothed by caller) */
    int    edge_count;        /* WAIT + HOLD edges in the graph                 */
    double graph_density;     /* edge_count / (MAX_NODES * (MAX_NODES - 1))     */
    int    wait_edges;        /* WAIT edges only — pass back as wait_edges_prev */
} features_t;

/* Snapshot the graph. wait_edges_prev/elapsed_seconds come from the
 * previous call and give the instantaneous growth rate. */
features_t extract_features(const graph_t *g, int wait_edges_prev,
                            double elapsed_seconds);

#endif /* FEATURE_EXTRACTOR_H */
