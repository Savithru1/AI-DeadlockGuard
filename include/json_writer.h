#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include "engine.h"

/* Writes the engine state as one JSON document for the Flask website.
 * Writes to <path>.tmp first and renames it into place, so a reader never
 * sees a half-written file. Returns 0 on success. */
int json_write_state(const char *path, const engine_state_t *s);

#endif /* JSON_WRITER_H */
