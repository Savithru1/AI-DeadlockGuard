/*
 * python_bridge.c — persistent coprocess running ai_prediction.py --for-c.
 */

#include "python_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PREDICTOR_SCRIPT  "ai_prediction.py"
#define PREDICTOR_LOG     "logs/predictor.log"
#define DEFAULT_VENV_PY   "/.venvs/deadlock-guard/bin/python3"
#define PREDICT_TIMEOUT_MS 2000

static pid_t       py_pid = -1;
static int         to_py = -1;      /* we write features here  */
static int         from_py = -1;    /* we read replies here    */
static ai_status_t status = AI_FAILED;
static char        status_msg[128] = "not started";
static char        interpreter[512] = "python3";

static char   rbuf[512];            /* partial-line buffer for from_py */
static size_t rlen = 0;

const char *python_bridge_interpreter(void) { return interpreter; }

static void pick_interpreter(void) {
    const char *env = getenv("DLG_PYTHON");
    if (env && *env) {
        snprintf(interpreter, sizeof(interpreter), "%s", env);
        return;
    }
    const char *home = getenv("HOME");
    if (home) {
        char venv[512];
        snprintf(venv, sizeof(venv), "%s%s", home, DEFAULT_VENV_PY);
        if (access(venv, X_OK) == 0) {
            snprintf(interpreter, sizeof(interpreter), "%s", venv);
            return;
        }
    }
    snprintf(interpreter, sizeof(interpreter), "python3");
}

static void set_status(ai_status_t s, const char *msg) {
    status = s;
    snprintf(status_msg, sizeof(status_msg), "%.*s", (int)sizeof(status_msg) - 1, msg);
}

int python_bridge_start(void) {
    pick_interpreter();

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        set_status(AI_FAILED, "pipe() failed");
        return -1;
    }

    py_pid = fork();
    if (py_pid < 0) {
        set_status(AI_FAILED, "fork() failed");
        return -1;
    }

    if (py_pid == 0) {
        /* Child: stdin <- in_pipe, stdout -> out_pipe, stderr -> log file.
         * Ignore SIGINT so Ctrl+C in the menu doesn't kill the model: an
         * ignored disposition survives exec and Python keeps it. */
        signal(SIGINT, SIG_IGN);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int log = open(PREDICTOR_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0) dup2(log, STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        if (log >= 0) close(log);

        execlp(interpreter, interpreter, PREDICTOR_SCRIPT, "--for-c", (char *)NULL);
        printf("ERROR could not run %s\n", interpreter);
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    to_py = in_pipe[1];
    from_py = out_pipe[0];
    fcntl(from_py, F_SETFL, fcntl(from_py, F_GETFL) | O_NONBLOCK);
    fcntl(to_py, F_SETFD, FD_CLOEXEC);
    fcntl(from_py, F_SETFD, FD_CLOEXEC);

    set_status(AI_LOADING, "loading model…");
    return 0;
}

/* Pull whatever bytes are available into rbuf; return a complete line
 * (without '\n') in out, or 0 if none yet. -1 on EOF/error. */
static int read_line(char *out, size_t outlen) {
    for (;;) {
        char *nl = memchr(rbuf, '\n', rlen);
        if (nl) {
            size_t n = (size_t)(nl - rbuf);
            size_t copy = n < outlen - 1 ? n : outlen - 1;
            memcpy(out, rbuf, copy);
            out[copy] = '\0';
            memmove(rbuf, nl + 1, rlen - n - 1);
            rlen -= n + 1;
            return 1;
        }
        if (rlen == sizeof(rbuf)) rlen = 0;      /* overlong junk line */

        ssize_t got = read(from_py, rbuf + rlen, sizeof(rbuf) - rlen);
        if (got > 0) { rlen += (size_t)got; continue; }
        if (got == 0) return -1;
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
}

static void mark_dead(const char *why) {
    char msg[128];
    int st;
    if (py_pid > 0 && waitpid(py_pid, &st, WNOHANG) == py_pid) py_pid = -1;
    snprintf(msg, sizeof(msg), "%s (see " PREDICTOR_LOG ")", why);
    set_status(AI_FAILED, msg);
}

ai_status_t python_bridge_poll(char *msg, size_t msglen) {
    if (status == AI_LOADING) {
        char line[256];
        int rc;
        while ((rc = read_line(line, sizeof(line))) == 1) {
            if (strcmp(line, "READY") == 0) {
                set_status(AI_READY, "model loaded");
                break;
            }
            if (strncmp(line, "ERROR", 5) == 0) {
                set_status(AI_FAILED, line[5] ? line + 6 : "predictor error");
                break;
            }
        }
        if (rc < 0 && status == AI_LOADING) mark_dead("predictor exited while loading");
    }
    if (msg && msglen) snprintf(msg, msglen, "%s", status_msg);
    return status;
}

int python_bridge_predict(const features_t *f, double *risk) {
    if (status != AI_READY) return -1;

    char req[128];
    int n = snprintf(req, sizeof(req), "%d,%.6f,%d,%.8f\n",
                     f->blocked_count, f->wait_time_growth,
                     f->edge_count, f->graph_density);
    if (write(to_py, req, (size_t)n) != n) {
        mark_dead("predictor pipe closed");
        return -1;
    }

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    char line[128];
    for (;;) {
        int rc = read_line(line, sizeof(line));
        if (rc == 1) {
            char *end;
            double v = strtod(line, &end);
            if (end == line) continue;           /* not a number: skip */
            *risk = v;
            return 0;
        }
        if (rc < 0) {
            mark_dead("predictor exited");
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long waited = (now.tv_sec - start.tv_sec) * 1000
                    + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (waited >= PREDICT_TIMEOUT_MS) {
            mark_dead("predictor timed out");
            return -1;
        }
        struct pollfd p = { from_py, POLLIN, 0 };
        poll(&p, 1, (int)(PREDICT_TIMEOUT_MS - waited));
    }
}

void python_bridge_stop(void) {
    if (to_py >= 0)   { close(to_py);   to_py = -1; }    /* Python sees EOF */
    if (from_py >= 0) { close(from_py); from_py = -1; }
    if (py_pid > 0) {
        for (int i = 0; i < 20; i++) {                    /* up to ~1s */
            if (waitpid(py_pid, NULL, WNOHANG) == py_pid) { py_pid = -1; break; }
            usleep(50000);
        }
        if (py_pid > 0) {
            kill(py_pid, SIGKILL);
            waitpid(py_pid, NULL, 0);
            py_pid = -1;
        }
    }
    set_status(AI_FAILED, "stopped");
}

void python_bridge_close_in_child(void) {
    if (to_py >= 0)   close(to_py);
    if (from_py >= 0) close(from_py);
}
