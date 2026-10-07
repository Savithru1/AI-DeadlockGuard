#ifndef EVENT_PROTOCOL_H
#define EVENT_PROTOCOL_H

/*
 * Wire format for events sent from the worker (philosopher) processes to
 * the monitor thread over a named pipe (FIFO). Every message is a
 * fixed-size 64-byte struct, so one read() always yields exactly one event
 * and concurrent writes from several workers never interleave (writes of
 * <= PIPE_BUF bytes to a pipe are atomic on Linux).
 *
 * Event flow for one lock:
 *   EV_WAIT    worker is about to block on a mutex
 *   EV_HOLD    worker now owns the mutex
 *   EV_RELEASE worker unlocked it
 *   EV_RECOVER previous owner was killed; worker took the robust mutex over
 *   EV_DONE    worker finished and is exiting
 */

#include <stdint.h>
#include <sys/types.h>

#define FIFO_PATH         "/tmp/deadlock_guard.fifo"
#define MAX_RESOURCE_NAME 32

typedef enum {
    EV_WAIT    = 1,
    EV_HOLD    = 2,
    EV_RELEASE = 3,
    EV_RECOVER = 4,
    EV_DONE    = 99
} event_type_t;

typedef struct {
    int32_t type;                        /* event_type_t                     */
    int32_t pid;                         /* sending process                  */
    int32_t run_id;                      /* which run sent it (drops stale)  */
    int32_t worker;                      /* philosopher index                */
    char    resource[MAX_RESOURCE_NAME]; /* lock name, e.g. "fork_0"         */
    uint8_t _pad[64 - 4 * sizeof(int32_t) - MAX_RESOURCE_NAME];
} ipc_event_t;

_Static_assert(sizeof(ipc_event_t) == 64, "ipc_event_t must stay 64 bytes");

#endif /* EVENT_PROTOCOL_H */
