/*
 * fifo_channel.c — named-pipe transport between workers and the monitor.
 *
 * Why fixed-size structs over a FIFO?
 *   - No framing/parsing needed: one read() always yields one event.
 *   - Writes <= PIPE_BUF (4096 bytes) are atomic on Linux, so concurrent
 *     worker writes never interleave partial messages.
 */

#include "fifo_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Writer side ─────────────────────────────────────────────────────────── */

int fifo_open_write(void) {
    return open(FIFO_PATH, O_WRONLY);
}

int fifo_send_event(int fd, event_type_t type, int run_id, int worker,
                    const char *resource) {
    if (fd < 0) return -1;

    ipc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type   = type;
    ev.pid    = getpid();
    ev.run_id = run_id;
    ev.worker = worker;
    strncpy(ev.resource, resource, MAX_RESOURCE_NAME - 1);

    return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

void fifo_close_write(int fd) {
    if (fd >= 0) close(fd);
}

/* ── Reader side ─────────────────────────────────────────────────────────── */

int fifo_open_read(void) {
    if (mkfifo(FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo " FIFO_PATH);
        return -1;
    }

    /*
     * O_RDWR instead of O_RDONLY: the monitor itself counts as a writer, so
     * the FIFO never reports EOF between runs when every worker has exited,
     * and workers' O_WRONLY open never blocks. O_NONBLOCK lets the monitor
     * thread poll it alongside its other duties.
     */
    int fd = open(FIFO_PATH, O_RDWR | O_NONBLOCK);
    if (fd < 0) perror("open " FIFO_PATH);
    return fd;
}

int fifo_recv_event(int fd, ipc_event_t *ev) {
    ssize_t n = read(fd, ev, sizeof(*ev));
    if (n == (ssize_t)sizeof(*ev)) return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

void fifo_destroy(void) {
    unlink(FIFO_PATH);
}
