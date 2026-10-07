/*
 * dashboard.c — everything the program prints to the terminal.
 */

#include "dashboard.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "python_bridge.h"

#define BOX_WIDTH 66   /* visible columns between the two vertical borders */
#define RULE_WIDTH 78

static int color = 0;

/* ANSI styles (empty strings when colour is off) */
#define C_RESET   (color ? "\033[0m"  : "")
#define C_BOLD    (color ? "\033[1m"  : "")
#define C_DIM     (color ? "\033[2m"  : "")
#define C_RED     (color ? "\033[31m" : "")
#define C_GREEN   (color ? "\033[32m" : "")
#define C_YELLOW  (color ? "\033[33m" : "")
#define C_BLUE    (color ? "\033[34m" : "")
#define C_MAGENTA (color ? "\033[35m" : "")
#define C_CYAN    (color ? "\033[36m" : "")

void dash_init(void) {
    color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
}

static const char *philosopher_color(int i) {
    static const char *codes[] = { "\033[36m", "\033[33m", "\033[34m", "\033[35m", "\033[32m" };
    return color ? codes[i % 5] : "";
}

static void rule(const char *style, const char *ch) {
    printf("%s", style);
    for (int i = 0; i < RULE_WIDTH; i++) printf("%s", ch);
    printf("%s\n", C_RESET);
}

/* Visible width of a UTF-8 string, skipping ANSI escape sequences.
 * Continuation bytes (10xxxxxx) don't count, as in the reference. */
static int visible_len(const char *s) {
    int len = 0;
    while (*s) {
        if (*s == '\033') {
            while (*s && *s != 'm') s++;
            if (*s) s++;
            continue;
        }
        if (((unsigned char)*s & 0xC0) != 0x80) len++;
        s++;
    }
    return len;
}

/* Print text padded to width visible columns. */
static void pad_print(const char *text, int width) {
    int pad = width - visible_len(text);
    printf("%s%*s", text, pad > 0 ? pad : 0, "");
}

static void risk_bar(char *out, size_t len, double risk, double threshold) {
    const int width = 20;
    int filled = (int)(risk * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    const char *c = risk < 0.40 ? C_GREEN : risk < threshold ? C_YELLOW : C_RED;

    size_t n = (size_t)snprintf(out, len, "%s%s%.2f%s %s", C_BOLD, c, risk, C_RESET, c);
    for (int i = 0; i < filled && n < len; i++)
        n += (size_t)snprintf(out + n, len - n, "█");
    if (n < len) n += (size_t)snprintf(out + n, len - n, "%s%s", C_RESET, C_DIM);
    for (int i = filled; i < width && n < len; i++)
        n += (size_t)snprintf(out + n, len - n, "░");
    if (n < len) snprintf(out + n, len - n, "%s", C_RESET);
}

/* ── Menu ────────────────────────────────────────────────────────────────── */

void dash_print_menu(const engine_state_t *s, int web_port) {
    const char *ai_color = s->ai_status == AI_READY ? C_GREEN : s->ai_status == AI_LOADING ? C_YELLOW : C_RED;
    const char *ai_word  = s->ai_status == AI_READY ? "ready" : s->ai_status == AI_LOADING ? "loading" : "unavailable";

    printf("\n");
    printf("=============================================\n");
    printf("            AI DEADLOCK GUARD                \n");
    printf("=============================================\n");
    printf(" AI model : %s%s%s", ai_color, ai_word, C_RESET);
    if (s->ai_status == AI_READY) printf(" (Ensemble RF+SVM+XGB, threshold %.2f)", s->threshold);
    else                          printf(" — %s", s->ai_message);
    printf("\n Website  : http://localhost:%d  (start it with: make web)\n", web_port);
    printf("---------------------------------------------\n");
    printf("1. Run WITHOUT AI   (deadlock occurs)\n");
    printf("2. Run WITH AI      (AI predicts and prevents it)\n");
    printf("3. Compare both     (1 then 2, side by side)\n");
    printf("4. Live Monitor\n");
    printf("5. Last Results\n");
    printf("6. Event Log\n");
    printf("7. Exit\n");
    printf("\nEnter your choice: ");
    fflush(stdout);
}

/* ── Timeline events ─────────────────────────────────────────────────────── */

static void print_banner(const timeline_event_t *e) {
    int ai = e->value > 0.5;
    const char *c = ai ? C_GREEN : C_RED;
    printf("\n");
    rule(c, "━");
    printf("%s%s  RUN #%d · %s%s\n", c, C_BOLD, e->run_id, ai ? "WITH AI" : "WITHOUT AI", C_RESET);
    printf("%s  %s%s\n", C_DIM,
           ai ? "monitor + AI predictor (threshold 0.75) · resolver kills one victim on high risk"
              : "monitor observes only · AI predictor OFF · nobody intervenes",
           C_RESET);
    rule(c, "━");
}

void dash_print_event(const timeline_event_t *e) {
    if (e->kind == EVK_START) {
        print_banner(e);
        return;
    }

    char tag[16];
    const char *tag_color = C_DIM;
    const char *text = e->text;
    const char *text_color = "";
    char extra[160] = "";

    switch (e->kind) {
        case EVK_HOLD: case EVK_WAIT: case EVK_EAT: case EVK_DONE: case EVK_RECOVER:
            snprintf(tag, sizeof(tag), "P%d", e->worker);
            tag_color = philosopher_color(e->worker);
            if (text[0] == 'P' && strchr(text, ' ')) text = strchr(text, ' ') + 1;
            if (e->kind == EVK_DONE)    text_color = C_GREEN;
            if (e->kind == EVK_RECOVER) text_color = C_YELLOW;
            break;
        case EVK_DEADLOCK:
            snprintf(tag, sizeof(tag), "MONITOR");
            tag_color = C_RED;
            text_color = C_RED;
            break;
        case EVK_FROZEN:
            snprintf(tag, sizeof(tag), "FROZEN");
            tag_color = C_RED;
            snprintf(extra, sizeof(extra), "⏸  ");
            break;
        case EVK_RISK: {
            snprintf(tag, sizeof(tag), "AI");
            tag_color = C_BLUE;
            char bar[256];
            risk_bar(bar, sizeof(bar), e->value, 0.75);
            const char *rest = strstr(e->text, " · ");
            printf("  %s%+7.2fs%s  %s%s%-8s%s risk %s  %s%s%s\n", C_DIM, e->t, C_RESET,
                   tag_color, C_BOLD, tag, C_RESET, bar, C_DIM, rest ? rest + 4 : "", C_RESET);
            return;
        }
        case EVK_ALERT:
            snprintf(tag, sizeof(tag), "AI");
            tag_color = C_MAGENTA;
            text_color = C_MAGENTA;
            break;
        case EVK_KILL:
            snprintf(tag, sizeof(tag), "RESOLVER");
            tag_color = C_MAGENTA;
            text_color = C_MAGENTA;
            snprintf(extra, sizeof(extra), "⚡ ");
            break;
        case EVK_WATCHDOG:
            snprintf(tag, sizeof(tag), "WATCHDOG");
            tag_color = C_RED;
            text_color = C_RED;
            snprintf(extra, sizeof(extra), "✘ ");
            break;
        case EVK_RESULT:
            snprintf(tag, sizeof(tag), "RESULT");
            tag_color = strstr(e->text, "HUNG") ? C_RED : C_GREEN;
            text_color = tag_color;
            break;
        default:
            snprintf(tag, sizeof(tag), "INFO");
            text_color = C_DIM;
            break;
    }

    int bold = e->kind == EVK_DEADLOCK || e->kind == EVK_ALERT || e->kind == EVK_KILL ||
               e->kind == EVK_WATCHDOG || e->kind == EVK_RESULT || e->kind == EVK_DONE;
    printf("  %s%+7.2fs%s  %s%s%-8s%s %s%s%s%s%s\n", C_DIM, e->t, C_RESET,
           tag_color, C_BOLD, tag, C_RESET,
           text_color, bold ? C_BOLD : "", extra, text, C_RESET);
}

/* ── Comparison table ────────────────────────────────────────────────────── */

typedef struct {
    char text[64];
    const char *color;
} cell_t;

static cell_t cell_deadlock(const run_result_t *r) {
    cell_t c;
    if (r->deadlock) { snprintf(c.text, sizeof(c.text), "YES at +%.2fs", r->deadlock_at); c.color = C_RED; }
    else if (r->victims[0]) { snprintf(c.text, sizeof(c.text), "NO — prevented"); c.color = C_GREEN; }
    else { snprintf(c.text, sizeof(c.text), "NO"); c.color = C_GREEN; }
    return c;
}

static cell_t cell_ai(const run_result_t *r) {
    cell_t c;
    if (!r->ai_enabled) { snprintf(c.text, sizeof(c.text), "not running"); c.color = C_DIM; }
    else if (!r->ai_alerted) { snprintf(c.text, sizeof(c.text), "no alert (max %.2f)", r->max_risk); c.color = C_YELLOW; }
    else { snprintf(c.text, sizeof(c.text), "+%.2fs (risk %.2f)", r->alert_at, r->alert_risk); c.color = C_GREEN; }
    return c;
}

static cell_t cell_action(const run_result_t *r) {
    cell_t c;
    if (strcmp(r->intervention, "ai") == 0 || strcmp(r->intervention, "manual") == 0) {
        snprintf(c.text, sizeof(c.text), "%s killed %s", strcmp(r->intervention, "ai") == 0 ? "AI" : "manual:", r->victims);
        c.color = C_GREEN;
    } else if (strcmp(r->intervention, "watchdog") == 0) {
        snprintf(c.text, sizeof(c.text), "none (watchdog)");
        c.color = C_RED;
    } else {
        snprintf(c.text, sizeof(c.text), "none");
        c.color = C_DIM;
    }
    return c;
}

static cell_t cell_frozen(const run_result_t *r) {
    cell_t c;
    if (!r->deadlock) { snprintf(c.text, sizeof(c.text), "0.00s"); c.color = C_GREEN; }
    else if (r->hung) { snprintf(c.text, sizeof(c.text), "%.1fs → ∞ (forced stop)", r->frozen_for); c.color = C_RED; }
    else { snprintf(c.text, sizeof(c.text), "%.2fs", r->frozen_for); c.color = C_YELLOW; }
    return c;
}

static cell_t cell_finished(const run_result_t *r) {
    cell_t c;
    snprintf(c.text, sizeof(c.text), "%d / %d", r->finished, N_PHILOSOPHERS);
    c.color = r->finished ? C_GREEN : C_RED;
    return c;
}

static cell_t cell_killed(const run_result_t *r) {
    cell_t c;
    if (r->hung || r->timed_out) { snprintf(c.text, sizeof(c.text), "%d (by watchdog)", r->killed); c.color = C_RED; }
    else if (r->killed && !r->victims[0]) { snprintf(c.text, sizeof(c.text), "%d (externally)", r->killed); c.color = C_RED; }
    else if (r->killed) { snprintf(c.text, sizeof(c.text), "%d (victim)", r->killed); c.color = C_YELLOW; }
    else { snprintf(c.text, sizeof(c.text), "0"); c.color = C_GREEN; }
    return c;
}

static int forced_stop(const run_result_t *r) {
    return r->hung || r->timed_out;
}

static cell_t cell_time(const run_result_t *r) {
    cell_t c;
    snprintf(c.text, sizeof(c.text), "%.2fs %s", r->duration, forced_stop(r) ? "(forced stop)" : "(completed)");
    c.color = forced_stop(r) ? C_RED : C_GREEN;
    return c;
}

/* One word for how a run ended; shared by the table and the live box. */
static const char *outcome_word(const run_result_t *r) {
    if (r->hung)          return "HUNG ✘";
    if (r->timed_out)     return "FAILED (timed out) ✘";
    if (r->finished == 0) return "FAILED ✘";
    return "RECOVERED ✔";
}

static cell_t cell_outcome(const run_result_t *r) {
    cell_t c;
    snprintf(c.text, sizeof(c.text), "%s", outcome_word(r));
    c.color = r->finished && !forced_stop(r) ? C_GREEN : C_RED;
    return c;
}

void dash_print_comparison(const run_result_t *base, const run_result_t *ai) {
    const run_result_t *runs[2];
    const char *heads[2];
    int n = 0;
    if (base && base->valid) { runs[n] = base; heads[n++] = "WITHOUT AI"; }
    if (ai && ai->valid)     { runs[n] = ai;   heads[n++] = "WITH AI"; }

    printf("\n");
    rule(C_CYAN, "━");
    printf("%s%s  COMPARISON%s\n", C_CYAN, C_BOLD, C_RESET);
    printf("%s  same scenario, same monitor — the only difference is the AI predictor%s\n", C_DIM, C_RESET);
    rule(C_CYAN, "━");

    if (n == 0) {
        printf("  No completed runs yet — choose 1, 2 or 3 from the menu.\n");
        return;
    }

    const int w0 = 26, w = 30;
    printf("  %*s", w0, "");
    for (int i = 0; i < n; i++) {
        printf("%s", C_BOLD);
        pad_print(heads[i], w);
        printf("%s", C_RESET);
    }
    printf("\n  ");
    for (int i = 0; i < w0 - 2; i++) printf("─");
    printf("  ");
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < w - 2; i++) printf("─");
        printf("  ");
    }
    printf("\n");

    struct { const char *label; cell_t (*fn)(const run_result_t *); } rows[] = {
        { "Deadlock (cycle) formed", cell_deadlock },
        { "AI early warning",        cell_ai },
        { "Intervention",            cell_action },
        { "Time frozen in deadlock", cell_frozen },
        { "Philosophers finished",   cell_finished },
        { "Processes killed",        cell_killed },
        { "Run time",                cell_time },
        { "OUTCOME",                 cell_outcome },
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        int strong = strcmp(rows[r].label, "OUTCOME") == 0;
        printf("  %s", strong ? C_BOLD : "");
        pad_print(rows[r].label, w0);
        printf("%s", C_RESET);
        for (int i = 0; i < n; i++) {
            cell_t c = rows[r].fn(runs[i]);
            printf("%s%s", c.color, strong ? C_BOLD : "");
            pad_print(c.text, w);
            printf("%s", C_RESET);
        }
        printf("\n");
    }

    if (n == 2 && base->deadlock && ai->ai_alerted) {
        double lead = base->deadlock_at - ai->alert_at;
        printf("\n");
        if (lead > 0)
            printf("%s  ▶ The AI acted %.2fs before the point where the unguarded run deadlocked (+%.2fs),\n"
                   "    so the circular wait never closed and the other philosophers finished.%s\n",
                   C_GREEN, lead, base->deadlock_at, C_RESET);
        else
            printf("%s  ▶ The AI acted %.2fs after the cycle closed — it broke the deadlock instead of preventing it.%s\n",
                   C_YELLOW, -lead, C_RESET);
    }
    printf("\n");
}

/* ── Live boxed dashboard ────────────────────────────────────────────────── */

static void box_border(const char *left, const char *right) {
    printf("%s", left);
    for (int i = 0; i < BOX_WIDTH; i++) printf("─");
    printf("%s\n", right);
}

static void box_row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void box_row(const char *fmt, ...) {
    char content[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(content, sizeof(content), fmt, ap);
    va_end(ap);
    printf("│ ");
    pad_print(content, BOX_WIDTH - 1);
    printf("%s│\n", C_RESET);
}

/* Cut a plain (uncoloured) string to at most cols visible columns, adding
 * "..." on a UTF-8 boundary when it had to be shortened. */
static void fit_columns(char *s, size_t cap, int cols) {
    if (visible_len(s) <= cols) return;
    int seen = 0;
    char *p = s;
    while (*p && seen < cols - 3) {
        if (((unsigned char)*p & 0xC0) != 0x80) seen++;
        p++;
    }
    while (((unsigned char)*p & 0xC0) == 0x80) p++;
    snprintf(p, cap - (size_t)(p - s), "...");
}

static void box_title(const char *title) {
    int pad = BOX_WIDTH - visible_len(title);
    int left = pad / 2;
    printf("│%*s%s%s%s%*s│\n", left, "", C_BOLD, title, C_RESET, pad - left, "");
}

static const char *worker_color(worker_state_t st) {
    switch (st) {
        case W_WAITING:  return C_YELLOW;
        case W_EATING:   return C_CYAN;
        case W_DONE:     return C_GREEN;
        case W_KILLED_AI: case W_KILLED_MANUAL: case W_KILLED_WATCHDOG: return C_RED;
        default:         return "";
    }
}

static const char *result_word(const run_result_t *r) {
    if (!r->valid) return "—";
    return outcome_word(r);
}

static const char *result_color(const run_result_t *r) {
    if (!r->valid) return "";
    return r->finished && !forced_stop(r) ? C_GREEN : C_RED;
}

void dash_render_live(const engine_state_t *s, int web_port) {
    if (isatty(STDOUT_FILENO)) printf("\033[2J\033[H");

    static const char *phase_names[] = { "idle", "running", "finished" };

    box_border("┌", "┐");
    box_title("AI DEADLOCK GUARD · LIVE MONITOR");
    box_border("├", "┤");

    if (s->run_id == 0) {
        box_row("Run        none yet — start one from the menu or the website");
    } else {
        box_row("Run        #%d · %s · %s · %.2fs%s", s->run_id,
                s->ai_enabled ? "WITH AI" : "WITHOUT AI", phase_names[s->phase], s->elapsed,
                s->compare_stage ? " · comparison" : "");
    }
    box_row("AI model   %s%s%s", s->ai_status == AI_READY ? C_GREEN : C_YELLOW,
            s->ai_status == AI_READY ? "ready (threshold 0.75)" : s->ai_message, C_RESET);
    if (s->risk >= 0) {
        char bar[256];
        risk_bar(bar, sizeof(bar), s->risk, s->threshold);
        box_row("AI risk    %s", bar);
    } else {
        box_row("AI risk    %s— (AI not running in this run)%s", C_DIM, C_RESET);
    }
    box_row("Features   blocked=%d edges=%d growth=%.2f density=%.4f",
            s->features.blocked_count, s->features.edge_count,
            s->features.wait_time_growth, s->features.graph_density);

    if (s->deadlocked)
        box_row("Status     %s%s⚠ DEADLOCKED — processes frozen%s", C_RED, C_BOLD, C_RESET);
    else if (s->phase == PHASE_ACTIVE)
        box_row("Status     %s✓ running%s", C_GREEN, C_RESET);
    else if (s->current.valid)
        box_row("Status     %s%s%s", result_color(&s->current), result_word(&s->current), C_RESET);
    else
        box_row("Status     idle");

    box_border("├", "┤");
    box_row("%sPROCESSES%s", C_BOLD, C_RESET);
    for (int i = 0; i < N_PHILOSOPHERS && s->run_id; i++) {
        const worker_view_t *w = &s->workers[i];
        box_row(" %sP%d%s  pid %-6d %s%-15s%s holds %-13s %s%s",
                philosopher_color(i), i, C_RESET, (int)w->pid,
                worker_color(w->state), worker_state_name(w->state), C_RESET,
                w->holds[0] ? w->holds : "-",
                w->waiting_for[0] ? "wants " : "", w->waiting_for);
    }
    if (s->cycle[0]) {
        char cycle[sizeof(s->cycle) + 8];
        snprintf(cycle, sizeof(cycle), "Cycle: %s", s->cycle);
        fit_columns(cycle, sizeof(cycle), BOX_WIDTH - 1);
        box_row("%s%s%s", C_RED, cycle, C_RESET);
    }

    box_border("├", "┤");
    box_row("%sRECENT EVENTS%s", C_BOLD, C_RESET);
    unsigned long newest = s->timeline_seq;
    unsigned long from = newest > 7 ? newest - 7 + 1 : 1;
    for (unsigned long q = from; newest && q <= newest; q++) {
        const timeline_event_t *e = &s->timeline[q % MAX_TIMELINE];
        char line[sizeof(e->text)];
        snprintf(line, sizeof(line), "%s", e->text);
        fit_columns(line, sizeof(line), BOX_WIDTH - 10);
        box_row("%s%+7.2fs%s %s", C_DIM, e->t, C_RESET, line);
    }

    box_border("├", "┤");
    box_row("Last runs  without AI: %s%s%s   with AI: %s%s%s",
            result_color(&s->baseline), result_word(&s->baseline), C_RESET,
            result_color(&s->ai), result_word(&s->ai), C_RESET);
    box_border("└", "┘");
    printf("%s  Website: http://localhost:%d  ·  Ctrl+C returns to the menu%s\n", C_DIM, web_port, C_RESET);
    fflush(stdout);
}
