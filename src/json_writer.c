/*
 * json_writer.c — engine state -> /tmp/deadlock_guard_state.json
 */

#include "json_writer.h"

#include <stdio.h>

#define JSON_TIMELINE_EVENTS 150

static void put_string(FILE *fp, const char *s) {
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else if (*p < 0x20) {
            fprintf(fp, "\\u%04x", *p);
        } else {
            fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

static void put_number_or_null(FILE *fp, int present, double v) {
    if (present) fprintf(fp, "%.3f", v);
    else         fputs("null", fp);
}

static void put_result(FILE *fp, const run_result_t *r) {
    if (!r->valid) {
        fputs("null", fp);
        return;
    }
    fprintf(fp, "{\"run_id\": %d, \"mode\": \"%s\", \"deadlock\": %s, \"deadlock_at\": ",
            r->run_id, r->ai_enabled ? "ai" : "baseline", r->deadlock ? "true" : "false");
    put_number_or_null(fp, r->deadlock, r->deadlock_at);
    fputs(", \"cycle\": ", fp);
    put_string(fp, r->cycle);
    fputs(", \"ai_alert_at\": ", fp);
    put_number_or_null(fp, r->ai_alerted, r->alert_at);
    fputs(", \"ai_alert_risk\": ", fp);
    put_number_or_null(fp, r->ai_alerted, r->alert_risk);
    fputs(", \"max_risk\": ", fp);
    put_number_or_null(fp, r->ai_enabled, r->max_risk);
    fputs(", \"intervention\": ", fp);
    put_string(fp, r->intervention);
    fputs(", \"victims\": ", fp);
    put_string(fp, r->victims);
    fputs(", \"resolved_at\": ", fp);
    put_number_or_null(fp, r->resolved_at >= 0, r->resolved_at);
    fprintf(fp, ", \"finished\": %d, \"killed\": %d, \"total\": %d, \"hung\": %s, "
                "\"timed_out\": %s, \"frozen_for\": %.3f, \"duration\": %.3f}",
            r->finished, r->killed, N_PHILOSOPHERS, r->hung ? "true" : "false",
            r->timed_out ? "true" : "false", r->frozen_for, r->duration);
}

int json_write_state(const char *path, const engine_state_t *s) {
    char tmp_path[280];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return -1;

    static const char *ai_names[] = { "loading", "ready", "unavailable" };
    static const char *phase_names[] = { "idle", "active", "done" };

    fprintf(fp, "{\n  \"timestamp\": %.3f,\n  \"backend_pid\": %d,\n", s->timestamp, (int)s->backend_pid);

    fprintf(fp, "  \"ai\": {\"status\": \"%s\", \"message\": ", ai_names[s->ai_status]);
    put_string(fp, s->ai_message);
    fputs(", \"interpreter\": ", fp);
    put_string(fp, s->ai_interpreter);
    fprintf(fp, ", \"threshold\": %.2f},\n", s->threshold);

    fprintf(fp, "  \"run\": {\"id\": %d, \"phase\": \"%s\", \"mode\": \"%s\", \"compare_stage\": %d, "
                "\"pending\": %s, \"origin\": \"%s\", \"elapsed\": %.3f, \"deadlocked\": %s, \"cycle\": ",
            s->run_id, phase_names[s->phase], s->ai_enabled ? "ai" : "baseline", s->compare_stage,
            s->pending_mode >= 0 ? "true" : "false", s->origin, s->elapsed,
            s->deadlocked ? "true" : "false");
    put_string(fp, s->cycle);
    fputs("},\n  \"risk\": ", fp);
    put_number_or_null(fp, s->risk >= 0, s->risk);

    const features_t *f = &s->features;
    fprintf(fp, ",\n  \"features\": {\"blocked_count\": %d, \"wait_time_growth\": %.4f, "
                "\"edge_count\": %d, \"graph_density\": %.6f},\n",
            f->blocked_count, f->wait_time_growth, f->edge_count, f->graph_density);

    fputs("  \"workers\": [", fp);
    for (int i = 0; i < N_PHILOSOPHERS; i++) {
        const worker_view_t *w = &s->workers[i];
        fprintf(fp, "%s\n    {\"id\": %d, \"name\": \"P%d\", \"pid\": %d, \"state\": \"%s\", \"holds\": ",
                i ? "," : "", i, i, (int)w->pid, worker_state_name(w->state));
        put_string(fp, w->holds);
        fputs(", \"waiting_for\": ", fp);
        put_string(fp, w->waiting_for);
        fputc('}', fp);
    }
    fputs("\n  ],\n", fp);

    fputs("  \"risk_history\": [", fp);
    for (int i = 0; i < s->risk_count; i++)
        fprintf(fp, "%s[%.2f, %.4f]", i ? ", " : "", s->risk_t[i], s->risk_v[i]);
    fputs("],\n", fp);

    fputs("  \"results\": {\"baseline\": ", fp);
    put_result(fp, &s->baseline);
    fputs(", \"ai\": ", fp);
    put_result(fp, &s->ai);
    fputs(", \"current\": ", fp);
    put_result(fp, &s->current);
    fputs("},\n", fp);

    fputs("  \"timeline\": [", fp);
    unsigned long newest = s->timeline_seq;
    unsigned long oldest = newest > MAX_TIMELINE ? newest - MAX_TIMELINE + 1 : 1;
    if (newest >= JSON_TIMELINE_EVENTS && newest - JSON_TIMELINE_EVENTS + 1 > oldest)
        oldest = newest - JSON_TIMELINE_EVENTS + 1;
    int first = 1;
    for (unsigned long q = oldest; newest && q <= newest; q++) {
        const timeline_event_t *e = &s->timeline[q % MAX_TIMELINE];
        fprintf(fp, "%s\n    {\"seq\": %lu, \"run\": %d, \"t\": %.2f, \"kind\": \"%s\", "
                    "\"worker\": %d, \"value\": %.3f, \"text\": ",
                first ? "" : ",", e->seq, e->run_id, e->t, event_kind_name(e->kind),
                e->worker, e->value);
        put_string(fp, e->text);
        fputc('}', fp);
        first = 0;
    }
    fputs("\n  ]\n}\n", fp);

    if (fclose(fp) != 0) return -1;
    return rename(tmp_path, path) == 0 ? 0 : -1;
}
