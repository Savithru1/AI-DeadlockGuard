/*
 * engine.c — the background monitor thread (see engine.h).
 *
 * Threading model: the engine thread is the only writer of the shared
 * state S. It takes `lock` whenever it modifies S; other threads (the menu)
 * only read S through engine_get_state() / engine_events_since(), which
 * take the same lock. The wait-for graph and timers are private to the
 * engine thread and need no locking.
 */

#include "engine.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cycle_detector.h"
#include "fifo_channel.h"
#include "graph.h"
#include "ipc_reader.h"
#include "json_writer.h"
#include "python_bridge.h"
#include "resolver.h"

#define TICK_US            10000   /* engine loop period             */
#define FEATURE_PERIOD     0.05    /* feature extraction cadence (s) */
#define PREDICT_PERIOD     0.10    /* AI prediction cadence (s)      */
#define SNAPSHOT_PERIOD    0.10    /* JSON snapshot cadence (s)      */
#define GROWTH_TAU_SEC     1.0     /* smoothing for wait growth      */
#define RISK_LOG_STEP      0.10    /* log risk when it moves this much */

/* ── Shared state ────────────────────────────────────────────────────────── */
static engine_state_t  S;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static double          pending_at = 0.0;       /* guarded by lock */
static int             resolve_requested = 0;  /* guarded by lock */

static pthread_t       thread;
static volatile int    running = 0;

/* ── Engine-thread private ───────────────────────────────────────────────── */
static graph_t g;
static int     fifo_fd = -1;
static FILE   *events_log = NULL;
static FILE   *features_log = NULL;
static int     next_run_id = 1;

static double  t0;                 /* monotonic time the run started */
static double  growth_ema;
static int     prev_wait_edges;
static double  last_feature, last_predict, last_snapshot;
static double  last_ai_action;
static double  last_risk_logged;
static double  deadlock_mono;      /* monotonic time the cycle closed */
static double  next_frozen_tick;
static int     watchdog_fired;
static int     waiting_for_model_noted;

/* ── Small helpers ───────────────────────────────────────────────────────── */

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double wall_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double run_time(void) {
    return S.phase == PHASE_ACTIVE ? mono_now() - t0 : S.elapsed;
}

const char *worker_state_name(worker_state_t s) {
    switch (s) {
        case W_STARTING:        return "starting";
        case W_HOLDING:         return "holding";
        case W_WAITING:         return "waiting";
        case W_EATING:          return "eating";
        case W_DONE:            return "done";
        case W_KILLED_AI:       return "killed_ai";
        case W_KILLED_MANUAL:   return "killed_manual";
        case W_KILLED_WATCHDOG: return "killed_watchdog";
    }
    return "?";
}

const char *event_kind_name(event_kind_t k) {
    static const char *names[] = {
        "info", "hold", "wait", "eat", "done", "recover", "deadlock",
        "frozen", "risk", "alert", "kill", "watchdog", "result", "start"
    };
    return (k >= 0 && k <= EVK_START) ? names[k] : "info";
}

static int is_killed(worker_state_t s) {
    return s == W_KILLED_AI || s == W_KILLED_MANUAL || s == W_KILLED_WATCHDOG;
}

static int worker_by_pid(pid_t pid) {
    for (int i = 0; i < N_PHILOSOPHERS; i++)
        if (S.workers[i].pid == pid) return i;
    return -1;
}

/* Append one event to the timeline ring and to logs/events.log. */
static void emit(event_kind_t kind, int worker, double value, const char *fmt, ...) {
    timeline_event_t ev;
    memset(&ev, 0, sizeof(ev));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);
    ev.kind = kind;
    ev.worker = worker;
    ev.value = value;
    ev.t = run_time();

    pthread_mutex_lock(&lock);
    ev.run_id = S.run_id;
    ev.seq = ++S.timeline_seq;
    S.timeline[ev.seq % MAX_TIMELINE] = ev;
    pthread_mutex_unlock(&lock);

    if (events_log) {
        time_t now = time(NULL);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));
        fprintf(events_log, "%s run=%d t=%+7.2fs %-8s %s\n",
                ts, ev.run_id, ev.t, event_kind_name(kind), ev.text);
    }
}

/* "P3" for a graph thread node, the resource name otherwise. */
static const char *node_label(int node, char *buf, size_t len) {
    if (IS_THREAD_NODE(node)) {
        int w = worker_by_pid(ipc_reader_get_pid(node));
        if (w >= 0) {
            snprintf(buf, len, "P%d", w);
            return buf;
        }
    }
    return g.names[node];
}

static void describe_cycle(const cycle_result_t *c, char *out, size_t len) {
    out[0] = '\0';
    for (int i = 0; i < c->length; i++) {
        char buf[16];
        size_t used = strlen(out);
        snprintf(out + used, len - used, "%s%s", i ? " → " : "",
                 node_label(c->path[i], buf, sizeof(buf)));
    }
}

/* Summarise what the kernel says the live philosophers are doing. */
static void kernel_summary(char *out, size_t len) {
    int alive = 0, asleep = 0;
    char wchan[48] = "?";
    for (int i = 0; i < N_PHILOSOPHERS; i++) {
        worker_view_t *w = &S.workers[i];
        if (w->reaped || w->pid <= 0) continue;

        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/%d/stat", (int)w->pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        char *paren = strrchr(buf, ')');
        if (!paren || paren[1] == '\0') continue;
        char state = paren[2];
        if (state == 'Z') continue;
        alive++;

        snprintf(path, sizeof(path), "/proc/%d/wchan", (int)w->pid);
        char wc[48] = "";
        f = fopen(path, "r");
        if (f) {
            size_t m = fread(wc, 1, sizeof(wc) - 1, f);
            wc[m] = '\0';
            fclose(f);
        }
        if (state == 'S' && strstr(wc, "futex")) {
            asleep++;
            snprintf(wchan, sizeof(wchan), "%s", wc);
        }
    }
    snprintf(out, len, "%d/%d processes asleep in the kernel (%s)", asleep, alive, wchan);
}

/* ── Run lifecycle ───────────────────────────────────────────────────────── */

static void start_run(int ai, int compare_stage, const char *origin) {
    graph_init(&g);
    ipc_reader_reset();
    ipc_event_t stale;
    while (fifo_recv_event(fifo_fd, &stale) == 1) { }

    double now = mono_now();
    growth_ema = 0.0;
    prev_wait_edges = 0;
    last_feature = last_predict = now;
    last_ai_action = -1e9;
    last_risk_logged = -1.0;
    watchdog_fired = 0;
    waiting_for_model_noted = 0;

    pthread_mutex_lock(&lock);
    S.run_id = next_run_id++;
    S.ai_enabled = ai;
    S.compare_stage = compare_stage;
    S.pending_mode = -1;
    snprintf(S.origin, sizeof(S.origin), "%s", origin);
    S.deadlocked = 0;
    S.cycle[0] = '\0';
    S.risk = ai ? 0.0 : -1.0;
    memset(&S.features, 0, sizeof(S.features));
    S.risk_count = 0;
    memset(S.workers, 0, sizeof(S.workers));
    memset(&S.current, 0, sizeof(S.current));
    S.current.valid = 1;
    S.current.run_id = S.run_id;
    S.current.ai_enabled = ai;
    S.current.resolved_at = -1.0;
    snprintf(S.current.intervention, sizeof(S.current.intervention), "none");
    S.elapsed = 0.0;
    S.phase = PHASE_ACTIVE;
    pthread_mutex_unlock(&lock);
    t0 = mono_now();

    emit(EVK_START, -1, ai, "RUN #%d started — %s · %d philosopher processes",
         S.run_id, ai ? "WITH AI (risk threshold 0.75)" : "WITHOUT AI (monitor observes only)",
         N_PHILOSOPHERS);

    pid_t pids[N_PHILOSOPHERS];
    if (scenario_start(S.run_id, pids) != 0) {
        emit(EVK_INFO, -1, 0, "could not fork the philosopher processes");
        pthread_mutex_lock(&lock);
        S.phase = PHASE_DONE;
        S.compare_stage = 0;
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < N_PHILOSOPHERS; i++) S.workers[i].pid = pids[i];
    pthread_mutex_unlock(&lock);

    struct stat st;
    int fresh = stat(FEATURES_LOG, &st) != 0;
    features_log = fopen(FEATURES_LOG, "a");
    if (features_log && fresh)
        fprintf(features_log, "run,t,blocked_count,wait_time_growth,edge_count,graph_density,risk\n");
}

static void append_result_csv(const run_result_t *r) {
    struct stat st;
    int fresh = stat(RESULTS_LOG, &st) != 0;
    FILE *f = fopen(RESULTS_LOG, "a");
    if (!f) return;
    if (fresh)
        fprintf(f, "timestamp,run,mode,deadlock,deadlock_at,ai_alert_at,intervention,"
                   "victims,finished,killed,hung,frozen_for,duration,timed_out\n");
    fprintf(f, "%ld,%d,%s,%d,%.2f,%.2f,%s,%s,%d,%d,%d,%.2f,%.2f,%d\n",
            (long)time(NULL), r->run_id, r->ai_enabled ? "ai" : "baseline",
            r->deadlock, r->deadlock ? r->deadlock_at : -1.0,
            r->ai_alerted ? r->alert_at : -1.0, r->intervention,
            r->victims[0] ? r->victims : "-", r->finished, r->killed,
            r->hung, r->frozen_for, r->duration, r->timed_out);
    fclose(f);
}

static void finish_run(double now) {
    run_result_t *r = &S.current;
    int finished = 0;
    for (int i = 0; i < N_PHILOSOPHERS; i++)
        if (S.workers[i].state == W_DONE) finished++;

    pthread_mutex_lock(&lock);
    S.elapsed = now - t0;
    r->duration = S.elapsed;
    r->finished = finished;
    r->killed = N_PHILOSOPHERS - finished;
    if (S.deadlocked) {                       /* still stuck at the very end */
        r->frozen_for = now - deadlock_mono;
        S.deadlocked = 0;
    }
    if (r->ai_enabled) S.ai = *r;
    else               S.baseline = *r;
    pthread_mutex_unlock(&lock);

    const char *mode = r->ai_enabled ? "WITH AI" : "WITHOUT AI";
    if (r->hung)
        emit(EVK_RESULT, -1, 0, "RUN #%d (%s) HUNG — deadlocked for %.1fs until the watchdog killed all %d philosophers",
             r->run_id, mode, r->frozen_for, r->killed);
    else if (r->timed_out)
        emit(EVK_RESULT, -1, 0, "RUN #%d (%s) FAILED — timed out after %.0fs without finishing · %d/%d finished, %d killed by the watchdog",
             r->run_id, mode, RUN_TIMEOUT_SEC, r->finished, N_PHILOSOPHERS, r->killed);
    else if (r->deadlock)
        emit(EVK_RESULT, -1, 0, "RUN #%d (%s) RECOVERED — deadlock broken after %.2fs · %d/%d finished, %d killed",
             r->run_id, mode, r->frozen_for, r->finished, N_PHILOSOPHERS, r->killed);
    else if (r->victims[0])
        emit(EVK_RESULT, -1, 0, "RUN #%d (%s) COMPLETE — deadlock prevented · %d/%d finished, %d killed (%s)",
             r->run_id, mode, r->finished, N_PHILOSOPHERS, r->killed, r->victims);
    else if (r->killed)
        emit(EVK_RESULT, -1, 0, "RUN #%d (%s) ENDED — %d/%d finished, %d killed from outside the program",
             r->run_id, mode, r->finished, N_PHILOSOPHERS, r->killed);
    else
        emit(EVK_RESULT, -1, 0, "RUN #%d (%s) COMPLETE — no deadlock · %d/%d finished",
             r->run_id, mode, r->finished, N_PHILOSOPHERS);

    append_result_csv(r);
    scenario_cleanup();
    if (features_log) { fclose(features_log); features_log = NULL; }

    /* Emit every event before the state goes idle: the terminal stops
     * following as soon as it sees the engine idle. */
    if (S.compare_stage == 1)
        emit(EVK_INFO, -1, 0, "comparison: starting the WITH-AI run in %.0fs …", COMPARE_GAP_SEC);
    else if (S.compare_stage == 2)
        emit(EVK_INFO, -1, 0, "comparison complete");

    pthread_mutex_lock(&lock);
    if (S.compare_stage == 1) {
        S.pending_mode = MODE_AI;
        pending_at = now + COMPARE_GAP_SEC;
    } else {
        S.compare_stage = 0;
    }
    S.phase = PHASE_DONE;
    pthread_mutex_unlock(&lock);
}

/* ── Resolution ──────────────────────────────────────────────────────────── */

static void on_deadlock(const cycle_result_t *c, double now) {
    char desc[sizeof(S.cycle)];
    describe_cycle(c, desc, sizeof(desc));

    pthread_mutex_lock(&lock);
    S.deadlocked = 1;
    snprintf(S.cycle, sizeof(S.cycle), "%s", desc);
    if (!S.current.deadlock) {
        S.current.deadlock = 1;
        S.current.deadlock_at = now - t0;
        snprintf(S.current.cycle, sizeof(S.current.cycle), "%s", desc);
    }
    pthread_mutex_unlock(&lock);

    deadlock_mono = now;
    next_frozen_tick = 1.0;
    emit(EVK_DEADLOCK, -1, 0, "DEADLOCK — wait-for cycle closed: %s", desc);
}

/* Kill one victim chosen by the resolver. who = "ai" or "manual". */
static int resolve_one(worker_state_t as, const char *who, double now) {
    int in_cycle = 0;
    int node = resolver_pick_victim(&g, &in_cycle);
    if (node < 0) {
        emit(EVK_INFO, -1, 0, "%s resolve: no hold-and-wait process — nothing to kill", who);
        return 0;
    }
    pid_t pid = resolver_kill(&g, node);
    int w = worker_by_pid(pid);
    if (pid <= 0 || w < 0) return 0;

    pthread_mutex_lock(&lock);
    S.workers[w].state = as;
    S.workers[w].holds[0] = '\0';
    S.workers[w].waiting_for[0] = '\0';
    run_result_t *r = &S.current;
    size_t used = strlen(r->victims);
    snprintf(r->victims + used, sizeof(r->victims) - used, "%sP%d", used ? " " : "", w);
    if (strcmp(r->intervention, "none") == 0)
        snprintf(r->intervention, sizeof(r->intervention), "%s", who);
    if (r->resolved_at < 0) r->resolved_at = now - t0;
    pthread_mutex_unlock(&lock);

    emit(EVK_KILL, w, 0, "%sSIGKILL → P%d (pid %d) — %s",
         strcmp(who, "manual") == 0 ? "manual override: " : "", w, (int)pid,
         in_cycle ? "breaking the deadlock cycle" : "pre-emptive: youngest hold-and-wait process");

    cycle_result_t c;
    if (S.deadlocked && !detect_cycle(&g, &c)) {
        pthread_mutex_lock(&lock);
        S.deadlocked = 0;
        S.current.frozen_for = now - deadlock_mono;
        pthread_mutex_unlock(&lock);
        emit(EVK_INFO, -1, 0, "cycle broken after %.2fs frozen", S.current.frozen_for);
    }
    return 1;
}

static void watchdog_kill(const char *reason, double now) {
    int n = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < N_PHILOSOPHERS; i++) {
        worker_view_t *w = &S.workers[i];
        if (w->reaped || w->pid <= 0 || w->state == W_DONE || is_killed(w->state)) continue;
        kill(w->pid, SIGKILL);
        w->state = W_KILLED_WATCHDOG;
        n++;
    }
    if (S.deadlocked) {
        S.current.hung = 1;
        S.current.frozen_for = now - deadlock_mono;
        S.deadlocked = 0;
    } else {
        S.current.timed_out = 1;
    }
    if (strcmp(S.current.intervention, "none") == 0)
        snprintf(S.current.intervention, sizeof(S.current.intervention), "watchdog");
    pthread_mutex_unlock(&lock);
    watchdog_fired = 1;

    emit(EVK_WATCHDOG, -1, 0, "watchdog: %s — killing %d philosophers (otherwise they hang forever)",
         reason, n);
}

/* ── Per-tick work during a run ──────────────────────────────────────────── */

static void handle_event(const ipc_event_t *ev, double now) {
    if (ev->run_id != S.run_id || ev->worker < 0 || ev->worker >= N_PHILOSOPHERS) return;
    int w = ev->worker;
    worker_view_t *wv = &S.workers[w];
    if (is_killed(wv->state)) return;           /* stale event from a victim */

    ipc_reader_apply(&g, ev);
    int is_right = strcmp(ev->resource, scenario_right_fork(w)) == 0;

    switch (ev->type) {
        case EV_WAIT:
            if (is_right) {
                pthread_mutex_lock(&lock);
                wv->state = W_WAITING;
                snprintf(wv->waiting_for, sizeof(wv->waiting_for), "%.*s", MAX_RESOURCE_NAME - 1, ev->resource);
                pthread_mutex_unlock(&lock);
                emit(EVK_WAIT, w, 0, "P%d reaches for %s (right) …", w, ev->resource);
            }
            cycle_result_t c;
            if (!S.deadlocked && detect_cycle(&g, &c)) on_deadlock(&c, now);
            break;

        case EV_HOLD:
            pthread_mutex_lock(&lock);
            if (is_right) {
                wv->state = W_EATING;
                wv->waiting_for[0] = '\0';
                snprintf(wv->holds, sizeof(wv->holds), "%.*s %.*s", MAX_RESOURCE_NAME - 1,
                         scenario_left_fork(w), MAX_RESOURCE_NAME - 1, ev->resource);
            } else {
                wv->state = W_HOLDING;
                snprintf(wv->holds, sizeof(wv->holds), "%.*s", MAX_RESOURCE_NAME - 1, ev->resource);
            }
            pthread_mutex_unlock(&lock);
            if (is_right) emit(EVK_EAT, w, 0, "P%d has both forks — eating", w);
            else          emit(EVK_HOLD, w, 0, "P%d picks up %s (left)", w, ev->resource);
            break;

        case EV_RECOVER:
            emit(EVK_RECOVER, w, 0, "P%d takes over %s from the killed process", w, ev->resource);
            break;

        case EV_DONE:
            pthread_mutex_lock(&lock);
            wv->state = W_DONE;
            wv->holds[0] = '\0';
            pthread_mutex_unlock(&lock);
            emit(EVK_DONE, w, 0, "P%d finished ✔", w);
            break;

        default:
            break;
    }
}

static void drain_fifo(double now) {
    ipc_event_t ev;
    while (fifo_recv_event(fifo_fd, &ev) == 1) handle_event(&ev, now);
}

static int reap_workers(void) {
    int remaining = 0;
    for (int i = 0; i < N_PHILOSOPHERS; i++) {
        worker_view_t *w = &S.workers[i];
        if (w->reaped || w->pid <= 0) continue;
        int st;
        if (waitpid(w->pid, &st, WNOHANG) == w->pid) {
            pthread_mutex_lock(&lock);
            w->reaped = 1;
            if (WIFSIGNALED(st) && !is_killed(w->state)) w->state = W_KILLED_MANUAL;
            pthread_mutex_unlock(&lock);
        } else {
            remaining++;
        }
    }
    return remaining;
}

static void update_features(double now) {
    double dt = now - last_feature;
    features_t f = extract_features(&g, prev_wait_edges, dt);

    /* Smooth the instantaneous rate (EMA, tau = 1s) and clamp at 0: the
     * model was trained on non-negative, slowly varying growth values. */
    double alpha = dt / GROWTH_TAU_SEC;
    if (alpha > 1.0) alpha = 1.0;
    growth_ema += alpha * (f.wait_time_growth - growth_ema);
    f.wait_time_growth = growth_ema > 0.0 ? growth_ema : 0.0;
    prev_wait_edges = f.wait_edges;
    last_feature = now;

    pthread_mutex_lock(&lock);
    S.features = f;
    pthread_mutex_unlock(&lock);
}

static void ai_step(double now) {
    last_predict = now;
    double risk;
    if (python_bridge_predict(&S.features, &risk) != 0) {
        pthread_mutex_lock(&lock);
        S.risk = -1.0;
        S.ai_enabled = 0;
        pthread_mutex_unlock(&lock);
        emit(EVK_INFO, -1, 0, "AI predictor stopped responding — run continues without AI");
        return;
    }
    double t = mono_now() - t0;      /* after the (blocking) model call */

    pthread_mutex_lock(&lock);
    S.risk = risk;
    if (S.risk_count < MAX_RISK_POINTS) {
        S.risk_t[S.risk_count] = (float)t;
        S.risk_v[S.risk_count] = (float)risk;
        S.risk_count++;
    }
    if (risk > S.current.max_risk) S.current.max_risk = risk;
    pthread_mutex_unlock(&lock);

    const features_t *f = &S.features;
    int crossed = last_risk_logged >= 0 && ((risk >= S.threshold) != (last_risk_logged >= S.threshold));
    if (last_risk_logged < 0 || fabs(risk - last_risk_logged) >= RISK_LOG_STEP || crossed) {
        last_risk_logged = risk;
        emit(EVK_RISK, -1, risk, "AI risk %.2f · blocked=%d edges=%d growth=%.2f",
             risk, f->blocked_count, f->edge_count, f->wait_time_growth);
    }

    if (risk >= S.threshold && now - last_ai_action >= AI_COOLDOWN_SEC) {
        last_ai_action = now;
        pthread_mutex_lock(&lock);
        if (!S.current.ai_alerted) {
            S.current.ai_alerted = 1;
            S.current.alert_at = t;
            S.current.alert_risk = risk;
        }
        pthread_mutex_unlock(&lock);
        emit(EVK_ALERT, -1, risk, "AI risk %.2f ≥ %.2f → requesting resolution", risk, S.threshold);
        resolve_one(W_KILLED_AI, "ai", now);
    }
}

static void log_features(double now) {
    if (!features_log) return;
    const features_t *f = &S.features;
    if (S.ai_enabled && S.risk >= 0)
        fprintf(features_log, "%d,%.3f,%d,%.4f,%d,%.6f,%.4f\n", S.run_id, now - t0,
                f->blocked_count, f->wait_time_growth, f->edge_count, f->graph_density, S.risk);
    else
        fprintf(features_log, "%d,%.3f,%d,%.4f,%d,%.6f,\n", S.run_id, now - t0,
                f->blocked_count, f->wait_time_growth, f->edge_count, f->graph_density);
}

static void supervise(double now) {
    if (watchdog_fired) return;
    if (S.deadlocked) {
        double frozen = now - deadlock_mono;
        if (frozen >= next_frozen_tick) {
            char summary[96];
            kernel_summary(summary, sizeof(summary));
            int done = 0;
            for (int i = 0; i < N_PHILOSOPHERS; i++) done += S.workers[i].state == W_DONE;
            emit(EVK_FROZEN, -1, 0, "frozen %.0fs · %s · %d/%d finished",
                 next_frozen_tick, summary, done, N_PHILOSOPHERS);
            next_frozen_tick += 1.0;
        }
        if (frozen >= HANG_LIMIT_SEC) {
            char why[96];
            snprintf(why, sizeof(why), "no progress for %.0fs after the cycle closed", HANG_LIMIT_SEC);
            watchdog_kill(why, now);
        }
    } else if (now - t0 >= RUN_TIMEOUT_SEC) {
        watchdog_kill("run exceeded the time limit", now);
    }
}

/* ── Requests (menu + website) ───────────────────────────────────────────── */

int engine_request_run(run_mode_t mode, const char *origin, char *why, size_t whylen) {
    int rc = 0;
    pthread_mutex_lock(&lock);
    if (S.phase == PHASE_ACTIVE || S.pending_mode >= 0 || S.compare_stage) {
        snprintf(why, whylen, "a run is already in progress");
        rc = -1;
    } else if (mode != MODE_BASELINE && S.ai_status == AI_FAILED) {
        snprintf(why, whylen, "AI model unavailable: %s", S.ai_message);
        rc = -1;
    } else {
        S.pending_mode = mode;
        snprintf(S.origin, sizeof(S.origin), "%s", origin);
        pending_at = 0.0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int engine_request_resolve(char *why, size_t whylen) {
    int rc = 0;
    pthread_mutex_lock(&lock);
    if (S.phase != PHASE_ACTIVE) {
        snprintf(why, whylen, "no run in progress");
        rc = -1;
    } else {
        resolve_requested = 1;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

static void poll_web_command(void) {
    FILE *f = fopen(WEB_COMMAND_PATH, "r");
    if (!f) return;
    char line[64] = "";
    if (!fgets(line, sizeof(line), f)) line[0] = '\0';
    fclose(f);
    unlink(WEB_COMMAND_PATH);
    line[strcspn(line, "\r\n")] = '\0';

    char why[160];
    if (strncmp(line, "run ", 4) == 0) {
        const char *m = line + 4;
        run_mode_t mode = strcmp(m, "ai") == 0 ? MODE_AI
                        : strcmp(m, "compare") == 0 ? MODE_COMPARE : MODE_BASELINE;
        if (engine_request_run(mode, "web", why, sizeof(why)) != 0)
            emit(EVK_INFO, -1, 0, "website request ignored: %s", why);
    } else if (strcmp(line, "resolve") == 0) {
        if (engine_request_resolve(why, sizeof(why)) != 0)
            emit(EVK_INFO, -1, 0, "website resolve ignored: %s", why);
    }
}

static void start_pending_run(double now) {
    pthread_mutex_lock(&lock);
    int mode = S.pending_mode;
    double at = pending_at;
    char origin[8];
    snprintf(origin, sizeof(origin), "%s", S.origin);
    pthread_mutex_unlock(&lock);

    if (mode < 0 || S.phase == PHASE_ACTIVE || now < at) return;

    if (mode == MODE_AI) {
        if (S.ai_status == AI_LOADING) {
            if (!waiting_for_model_noted) {
                waiting_for_model_noted = 1;
                emit(EVK_INFO, -1, 0, "waiting for the AI model to finish loading …");
            }
            return;
        }
        if (S.ai_status == AI_FAILED) {
            emit(EVK_INFO, -1, 0, "cannot start the WITH-AI run: %s", S.ai_message);
            pthread_mutex_lock(&lock);
            S.pending_mode = -1;
            S.compare_stage = 0;
            pthread_mutex_unlock(&lock);
            return;
        }
    }

    if (mode == MODE_COMPARE)      start_run(0, 1, origin);
    else if (mode == MODE_AI)      start_run(1, S.compare_stage ? 2 : 0, origin);
    else                           start_run(0, 0, origin);
}

/* ── Thread body ─────────────────────────────────────────────────────────── */

static void *engine_main(void *arg) {
    (void)arg;
    last_snapshot = 0.0;

    while (running) {
        double now = mono_now();

        char msg[128];
        ai_status_t st = python_bridge_poll(msg, sizeof(msg));
        pthread_mutex_lock(&lock);
        S.ai_status = st;
        snprintf(S.ai_message, sizeof(S.ai_message), "%s", msg);
        pthread_mutex_unlock(&lock);

        poll_web_command();
        start_pending_run(now);

        if (S.phase == PHASE_ACTIVE) {
            drain_fifo(now);

            pthread_mutex_lock(&lock);
            int want_resolve = resolve_requested;
            resolve_requested = 0;
            pthread_mutex_unlock(&lock);
            if (want_resolve) resolve_one(W_KILLED_MANUAL, "manual", now);

            if (now - last_feature >= FEATURE_PERIOD) update_features(now);
            if (S.ai_enabled && now - last_predict >= PREDICT_PERIOD) {
                ai_step(now);
                log_features(now);
            } else if (!S.ai_enabled && now - last_predict >= PREDICT_PERIOD) {
                last_predict = now;
                log_features(now);
            }
            supervise(now);

            if (reap_workers() == 0) {
                drain_fifo(now);             /* DONE events racing the exit */
                finish_run(now);
            } else {
                pthread_mutex_lock(&lock);
                S.elapsed = now - t0;
                pthread_mutex_unlock(&lock);
            }
        }

        if (now - last_snapshot >= SNAPSHOT_PERIOD) {
            last_snapshot = now;
            S.timestamp = wall_now();
            json_write_state(STATE_JSON_PATH, &S);
        }

        usleep(TICK_US);
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int engine_start(void) {
    fifo_fd = fifo_open_read();
    if (fifo_fd < 0) return -1;

    events_log = fopen(EVENTS_LOG, "a");
    if (events_log) setvbuf(events_log, NULL, _IOLBF, 0);

    memset(&S, 0, sizeof(S));
    S.backend_pid = getpid();
    S.threshold = AI_THRESHOLD;
    S.pending_mode = -1;
    S.risk = -1.0;
    S.phase = PHASE_IDLE;
    S.ai_status = python_bridge_poll(S.ai_message, sizeof(S.ai_message));
    snprintf(S.ai_interpreter, sizeof(S.ai_interpreter), "%s", python_bridge_interpreter());
    unlink(WEB_COMMAND_PATH);

    /* The engine thread must not receive SIGINT/SIGTERM — those belong to
     * the menu. Block them while creating it; it inherits the mask. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    running = 1;
    int rc = pthread_create(&thread, NULL, engine_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        running = 0;
        return -1;
    }
    return 0;
}

void engine_stop(void) {
    if (!running) return;
    running = 0;
    pthread_join(thread, NULL);

    if (S.phase == PHASE_ACTIVE) {
        for (int i = 0; i < N_PHILOSOPHERS; i++) {
            worker_view_t *w = &S.workers[i];
            if (w->pid > 0 && !w->reaped) {
                kill(w->pid, SIGKILL);
                waitpid(w->pid, NULL, 0);
            }
        }
        scenario_cleanup();
        S.phase = PHASE_IDLE;
    }
    if (features_log) fclose(features_log);
    if (events_log) fclose(events_log);
    close(fifo_fd);
    fifo_destroy();
    unlink(WEB_COMMAND_PATH);
    unlink(STATE_JSON_PATH);
}

void engine_get_state(engine_state_t *out) {
    pthread_mutex_lock(&lock);
    *out = S;
    pthread_mutex_unlock(&lock);
}

int engine_events_since(unsigned long after, timeline_event_t *out, int max) {
    pthread_mutex_lock(&lock);
    unsigned long newest = S.timeline_seq;
    unsigned long oldest = newest > MAX_TIMELINE ? newest - MAX_TIMELINE + 1 : 1;
    unsigned long from = after + 1 > oldest ? after + 1 : oldest;
    int n = 0;
    for (unsigned long s = from; s <= newest && n < max; s++)
        out[n++] = S.timeline[s % MAX_TIMELINE];
    pthread_mutex_unlock(&lock);
    return n;
}

int engine_busy(void) {
    pthread_mutex_lock(&lock);
    int busy = S.phase == PHASE_ACTIVE || S.pending_mode >= 0 || S.compare_stage;
    pthread_mutex_unlock(&lock);
    return busy;
}
