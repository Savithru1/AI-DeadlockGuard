#ifndef ENGINE_H
#define ENGINE_H

/*
 * The monitor engine: a background thread that runs for the whole life of
 * the program, independent of the menu (same split as the reference
 * project's monitor_loop).
 *
 * Each run, it forks the philosopher processes, reads their lock events
 * from the FIFO into the wait-for graph, detects cycles, extracts the live
 * features, asks the AI model for a risk score (WITH-AI runs only), kills a
 * victim when the risk crosses the threshold, and supervises the run with
 * a watchdog. Every 100 ms it writes a JSON snapshot for the Flask website.
 *
 * The menu (main thread) and the website (via a command file) only ever
 * *request* runs; the engine performs them. Everything the engine does is
 * recorded as timeline events, which the terminal and the website render.
 */

#include <stddef.h>
#include <sys/types.h>

#include "event_protocol.h"
#include "feature_extractor.h"
#include "scenario.h"

#define AI_THRESHOLD     0.75   /* risk that triggers resolution            */
#define AI_COOLDOWN_SEC  1.5    /* let the system unwind before acting again */
#define HANG_LIMIT_SEC   6.0    /* watchdog: give up on a deadlocked run     */
#define RUN_TIMEOUT_SEC  30.0   /* hard limit per run                        */
#define COMPARE_GAP_SEC  2.0    /* pause between the two runs of a compare   */

#define STATE_JSON_PATH  "/tmp/deadlock_guard_state.json"
#define WEB_COMMAND_PATH "/tmp/deadlock_guard.cmd"
#define EVENTS_LOG       "logs/events.log"
#define FEATURES_LOG     "logs/features.log"
#define RESULTS_LOG      "logs/results.csv"

#define MAX_TIMELINE     256
#define MAX_RISK_POINTS  400

typedef enum { MODE_BASELINE = 0, MODE_AI = 1, MODE_COMPARE = 2 } run_mode_t;
typedef enum { PHASE_IDLE = 0, PHASE_ACTIVE = 1, PHASE_DONE = 2 } run_phase_t;

typedef enum {
    W_STARTING, W_HOLDING, W_WAITING, W_EATING, W_DONE,
    W_KILLED_AI, W_KILLED_MANUAL, W_KILLED_WATCHDOG
} worker_state_t;

typedef struct {
    pid_t          pid;
    worker_state_t state;
    int            reaped;
    char           holds[2 * MAX_RESOURCE_NAME + 2];   /* "fork_2 fork_3" */
    char           waiting_for[MAX_RESOURCE_NAME];
} worker_view_t;

typedef enum {
    EVK_INFO, EVK_HOLD, EVK_WAIT, EVK_EAT, EVK_DONE, EVK_RECOVER,
    EVK_DEADLOCK, EVK_FROZEN, EVK_RISK, EVK_ALERT, EVK_KILL,
    EVK_WATCHDOG, EVK_RESULT, EVK_START
} event_kind_t;

typedef struct {
    unsigned long seq;
    int           run_id;
    double        t;          /* seconds since the run started */
    event_kind_t  kind;
    int           worker;     /* philosopher index, -1 if none */
    double        value;      /* risk (EVK_RISK/ALERT), AI on/off (EVK_START) */
    char          text[176];
} timeline_event_t;

typedef struct {
    int    valid;
    int    run_id;
    int    ai_enabled;
    int    deadlock;           /* a wait-for cycle formed               */
    double deadlock_at;
    char   cycle[176];
    int    ai_alerted;
    double alert_at;
    double alert_risk;
    double max_risk;
    char   intervention[16];   /* none | ai | manual | watchdog         */
    char   victims[48];
    double resolved_at;        /* first kill that broke/prevented it, -1 */
    int    finished;           /* philosophers that completed           */
    int    killed;
    int    hung;               /* deadlock lasted until the watchdog    */
    int    timed_out;          /* no deadlock, but the run hit the time
                                  limit and the watchdog killed the rest */
    double frozen_for;         /* seconds spent deadlocked              */
    double duration;
} run_result_t;

typedef struct {
    double         timestamp;          /* wall clock of this snapshot   */
    pid_t          backend_pid;

    int            ai_status;          /* ai_status_t                   */
    char           ai_message[128];
    char           ai_interpreter[128];
    double         threshold;

    run_phase_t    phase;
    int            run_id;
    int            ai_enabled;
    int            compare_stage;      /* 0 none, 1 baseline, 2 AI run  */
    int            pending_mode;       /* queued request, -1 if none    */
    char           origin[8];          /* "menu" or "web"               */
    double         elapsed;
    int            deadlocked;         /* a cycle exists right now      */
    char           cycle[176];
    double         risk;               /* -1 when the AI is not running */
    features_t     features;
    worker_view_t  workers[N_PHILOSOPHERS];

    run_result_t   current;
    run_result_t   baseline;           /* last completed WITHOUT-AI run */
    run_result_t   ai;                 /* last completed WITH-AI run    */

    int            risk_count;
    float          risk_t[MAX_RISK_POINTS];
    float          risk_v[MAX_RISK_POINTS];

    timeline_event_t timeline[MAX_TIMELINE];   /* ring buffer           */
    unsigned long    timeline_seq;             /* newest seq, 0 = none  */
} engine_state_t;

/* Start / stop the background thread (also opens the FIFO). */
int  engine_start(void);
void engine_stop(void);

/* Queue a run. Returns 0 if accepted; otherwise why explains. */
int  engine_request_run(run_mode_t mode, const char *origin, char *why, size_t whylen);

/* Ask the engine to kill one victim now (manual override). */
int  engine_request_resolve(char *why, size_t whylen);

/* Copy the full state (thread-safe). */
void engine_get_state(engine_state_t *out);

/* Copy timeline events with seq > after, oldest first. Returns count. */
int  engine_events_since(unsigned long after, timeline_event_t *out, int max);

/* 1 while a run is active or queued. */
int  engine_busy(void);

const char *worker_state_name(worker_state_t s);
const char *event_kind_name(event_kind_t k);

#endif /* ENGINE_H */
