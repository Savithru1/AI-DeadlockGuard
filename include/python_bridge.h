#ifndef PYTHON_BRIDGE_H
#define PYTHON_BRIDGE_H

/*
 * C <-> Python bridge for the AI deadlock-risk model.
 *
 * Starts `python3 ai_prediction.py --for-c` once as a coprocess connected
 * by two pipes (fork + dup2 + exec). Loading scikit-learn/xgboost and the
 * model takes a second or two, far too slow to repeat per prediction when a
 * deadlock forms in under three seconds, so the process stays alive:
 *
 *   C -> Python : "blocked,growth,edges,density\n"
 *   Python -> C : "risk\n"            (probability 0.0 - 1.0)
 *
 * Python prints "READY" once the model has loaded, or "ERROR <reason>" and
 * exits if it cannot load it. Its stderr goes to logs/predictor.log, and
 * it ignores SIGINT so Ctrl+C in the menu leaves the model running.
 *
 * Interpreter: $DLG_PYTHON if set, else ~/.venvs/deadlock-guard/bin/python3
 * if it exists (created by `make setup`), else python3.
 */

#include <stddef.h>

#include "feature_extractor.h"

typedef enum {
    AI_LOADING = 0,
    AI_READY   = 1,
    AI_FAILED  = 2
} ai_status_t;

/* Launch the coprocess. Call before any other thread exists.
 * Returns 0 if it was started (it may still fail while loading). */
int python_bridge_start(void);

/* Non-blocking: check for READY / ERROR / exit. msg gets a short status. */
ai_status_t python_bridge_poll(char *msg, size_t msglen);

/* Score one feature vector. Returns 0 and sets *risk, or -1 on failure. */
int python_bridge_predict(const features_t *f, double *risk);

/* Close the pipes and stop the coprocess. */
void python_bridge_stop(void);

/* Called in forked worker children: close our copies of the pipe fds. */
void python_bridge_close_in_child(void);

/* The interpreter actually used. */
const char *python_bridge_interpreter(void);

#endif /* PYTHON_BRIDGE_H */
