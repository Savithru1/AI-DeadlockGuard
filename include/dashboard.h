#ifndef DASHBOARD_H
#define DASHBOARD_H

/*
 * Terminal output for the WSL side. Colours are used only when stdout is a
 * real terminal (and NO_COLOR is unset), so redirected output stays a
 * readable plain log — the same rule the reference project's dashboard
 * uses for clearing the screen.
 */

#include "engine.h"

void dash_init(void);

/* Main menu, with a status header (AI model, website URL). */
void dash_print_menu(const engine_state_t *s, int web_port);

/* One timeline event as a coloured line (banners for EVK_START). */
void dash_print_event(const timeline_event_t *e);

/* Side-by-side WITHOUT AI vs WITH AI table (either may be missing). */
void dash_print_comparison(const run_result_t *base, const run_result_t *ai);

/* Boxed live dashboard; clears the screen first when on a terminal. */
void dash_render_live(const engine_state_t *s, int web_port);

#endif /* DASHBOARD_H */
