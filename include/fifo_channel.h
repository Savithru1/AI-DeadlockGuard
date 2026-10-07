#ifndef FIFO_CHANNEL_H
#define FIFO_CHANNEL_H

/*
 * Writer side (worker processes) and reader side (monitor thread) of the
 * named-pipe channel that carries ipc_event_t messages.
 */

#include "event_protocol.h"

/* ── Writer side (worker processes) ─────────────────────────────────────── */

/* Open the FIFO for writing. The monitor keeps a reader open at all times,
 * so this does not block. Returns fd, or -1 on error. */
int fifo_open_write(void);

/* Send one event. Returns 0 on success, -1 on error. */
int fifo_send_event(int fd, event_type_t type, int run_id, int worker,
                    const char *resource);

void fifo_close_write(int fd);

/* ── Reader side (monitor thread) ───────────────────────────────────────── */

/* Create the FIFO if needed and open it non-blocking for reading.
 * Returns fd, or -1 on error. */
int fifo_open_read(void);

/* Read one event. Returns 1 on success, 0 if no event is waiting,
 * -1 on error. */
int fifo_recv_event(int fd, ipc_event_t *ev);

/* Remove the FIFO from the filesystem. */
void fifo_destroy(void);

#endif /* FIFO_CHANNEL_H */
