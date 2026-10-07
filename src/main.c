/*
 * main.c — AI Deadlock Guard: menu-driven front end (runs in WSL / Linux).
 *
 * Two threads:
 *   - the engine thread (engine.c) runs for the program's whole lifetime:
 *     it performs runs, watches the wait-for graph, consults the AI model
 *     and writes the JSON snapshot the Flask website reads;
 *   - the main thread (this file) runs the menu and prints to the terminal.
 *
 * While waiting at the menu the main thread polls stdin with select(), so
 * a run started from the website is noticed and streamed here as well —
 * the same output is available in the terminal and in the browser.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dashboard.h"
#include "engine.h"
#include "python_bridge.h"

#define DEFAULT_WEB_PORT 8088
#define MENU_REDRAW      100     /* read_choice(): website run was shown */
#define INSTANCE_LOCK_PATH "/tmp/deadlock_guard.lock"

/* Ctrl+C at the menu exits; inside a view it returns to the menu. */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_interrupt = 0;

static void handle_sigint(int sig)  { (void)sig; g_interrupt = 1; }
static void handle_sigterm(int sig) { (void)sig; g_running = 0; }

static int           web_port = DEFAULT_WEB_PORT;
static unsigned long seen_seq = 0;          /* timeline events already printed */
static int           last_followed_run = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Run from the executable's directory so logs/ and ai_prediction.py resolve
 * no matter where the program was started from. */
static void chdir_to_exe_dir(void) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return;
    *slash = '\0';
    if (chdir(path) != 0) perror("chdir");
}

/* Only one backend may run: two would share the FIFO and steal each
 * other's lock events, and both would write the same state file. The
 * flock() is released automatically when the process exits. */
static int acquire_instance_lock(void) {
    int fd = open(INSTANCE_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return 0;                      /* can't tell: don't block startup */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char pid[32] = "?";
        ssize_t n = read(fd, pid, sizeof(pid) - 1);
        if (n > 0) pid[strcspn(pid, "\n")] = '\0';
        fprintf(stderr, "[AI Deadlock Guard] another instance is already running (pid %s).\n"
                        "Use that terminal, or stop it first (menu option 7).\n", pid);
        close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) == 0) dprintf(fd, "%d\n", (int)getpid());
    return 0;                                  /* keep fd open: holds the lock */
}

static engine_state_t *state_copy(void) {
    static engine_state_t s;          /* ~60 KB: keep it off the stack */
    engine_get_state(&s);
    return &s;
}

static void mark_all_seen(void) {
    seen_seq = state_copy()->timeline_seq;
}

/* Print timeline events until the engine is idle again (or Ctrl+C). */
static void follow_runs(void) {
    timeline_event_t ev[64];
    g_interrupt = 0;

    while (g_running && !g_interrupt) {
        int n = engine_events_since(seen_seq, ev, 64);
        for (int i = 0; i < n; i++) {
            dash_print_event(&ev[i]);
            seen_seq = ev[i].seq;
        }
        fflush(stdout);
        if (n == 0) {
            if (!engine_busy()) {
                /* Engine went idle: print anything emitted just before. */
                n = engine_events_since(seen_seq, ev, 64);
                for (int i = 0; i < n; i++) {
                    dash_print_event(&ev[i]);
                    seen_seq = ev[i].seq;
                }
                break;
            }
            usleep(50000);
        }
    }

    last_followed_run = state_copy()->run_id;
    if (g_interrupt) {
        g_interrupt = 0;
        printf("\n  (stopped following — the run continues in the background;"
               " watch it with Live Monitor or on the website)\n");
    }
}

static void show_comparison(void) {
    engine_state_t *s = state_copy();
    dash_print_comparison(&s->baseline, &s->ai);
}

/* True if the website has started (or queued) a run we have not shown. */
static int website_run_waiting(void) {
    engine_state_t *s = state_copy();
    if (strcmp(s->origin, "web") != 0) return 0;
    if (s->pending_mode >= 0) return 1;
    return s->phase == PHASE_ACTIVE && s->run_id != last_followed_run;
}

/*
 * Wait for a menu choice. Returns the number entered, 0 for invalid input,
 * -1 to exit (EOF, Ctrl+C, SIGTERM) or MENU_REDRAW after showing a run that
 * was started from the website.
 */
static int read_choice(void) {
    char buf[64];
    for (;;) {
        if (!g_running || g_interrupt) return -1;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { 0, 200000 };
        int rc = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) {
            if (website_run_waiting()) {
                printf("\n\n  [website] a run was started from the browser — following it here"
                       " (Ctrl+C to stop following)\n");
                follow_runs();
                show_comparison();
                return MENU_REDRAW;
            }
            continue;
        }

        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n <= 0) return -1;
        buf[n] = '\0';
        char *end;
        long v = strtol(buf, &end, 10);
        return end == buf ? 0 : (int)v;
    }
}

/* ── Menu actions ────────────────────────────────────────────────────────── */

static void start_and_follow(run_mode_t mode) {
    char why[160];
    mark_all_seen();
    if (engine_request_run(mode, "menu", why, sizeof(why)) != 0) {
        printf("\n  Cannot start: %s\n", why);
        return;
    }
    follow_runs();
    show_comparison();
}

static void live_monitor(void) {
    g_interrupt = 0;
    while (g_running && !g_interrupt) {
        dash_render_live(state_copy(), web_port);
        usleep(250000);
    }
    g_interrupt = 0;
    mark_all_seen();
}

static void show_event_log(void) {
    static timeline_event_t ev[MAX_TIMELINE];
    int n = engine_events_since(0, ev, MAX_TIMELINE);
    int from = n > 60 ? n - 60 : 0;

    printf("\nEvent log (last %d events — full history in " EVENTS_LOG ")\n", n - from);
    printf("--------------------------------------------------------------\n");
    if (n == 0) printf("  No events yet.\n");
    int run = -1;
    for (int i = from; i < n; i++) {
        if (ev[i].run_id != run && ev[i].kind != EVK_START) {
            run = ev[i].run_id;
            printf("  ── run #%d ──\n", run);
        }
        if (ev[i].kind == EVK_START) run = ev[i].run_id;
        dash_print_event(&ev[i]);
    }
    mark_all_seen();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    chdir_to_exe_dir();
    if (acquire_instance_lock() != 0) return 1;
    mkdir("logs", 0755);

    const char *port_env = getenv("DLG_WEB_PORT");
    if (port_env && atoi(port_env) > 0) web_port = atoi(port_env);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_sigint;            /* no SA_RESTART: interrupt select() */
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);                 /* a dead predictor must not kill us */

    dash_init();

    /* Fork the Python coprocess before any thread exists. */
    python_bridge_start();

    if (engine_start() != 0) {
        fprintf(stderr, "[AI Deadlock Guard] could not start the monitor engine\n");
        python_bridge_stop();
        return 1;
    }

    printf("\n[AI Deadlock Guard] backend running (pid %d). Loading the AI model in the background…\n",
           (int)getpid());

    while (g_running) {
        dash_print_menu(state_copy(), web_port);

        int choice = read_choice();
        if (choice == -1) break;
        if (choice == MENU_REDRAW) continue;

        switch (choice) {
            case 1: start_and_follow(MODE_BASELINE); break;
            case 2: start_and_follow(MODE_AI);       break;
            case 3: start_and_follow(MODE_COMPARE);  break;
            case 4: live_monitor();                  break;
            case 5: show_comparison();               break;
            case 6: show_event_log();                break;
            case 7: g_running = 0;                   break;
            default: printf("\nInvalid choice.\n");  break;
        }
    }

    printf("\nExiting AI Deadlock Guard…\n");
    engine_stop();
    python_bridge_stop();
    printf("[AI Deadlock Guard] Exited cleanly.\n");
    return 0;
}
