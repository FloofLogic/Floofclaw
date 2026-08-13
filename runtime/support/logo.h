#ifndef FCLAW_SUPPORT_LOGO_H
#define FCLAW_SUPPORT_LOGO_H

#include <signal.h>
#include <stdio.h>

#define FCLAW_LOGO_ROWS 6
#define FCLAW_HEADER_ROWS 9

/* One-shot static print (uses color when stdout is a tty). */
void fclaw_logo_print(FILE *out);

/* Render one frame at an absolute terminal row/column. This is for
 * foreground terminal UIs that reserve a fixed header area. It does not
 * emit newlines and does not depend on the current cursor position. */
void fclaw_logo_print_frame_at(FILE *out, int top, int left, int tick);

/* Render a single animated frame at the given color tick.
 * If clear_lines, each row is preceded by an ANSI line-erase. */
void fclaw_logo_print_frame(FILE *out, int tick, int clear_lines);

/* Render an animation loop until *stop_flag becomes non-zero. Returns 0
 * on normal exit, -1 on bad arguments. Restores cursor visibility on
 * exit. Does nothing animated if stdout isn't a tty (prints once). */
int fclaw_logo_loop(FILE *out, volatile sig_atomic_t *stop_flag);

#endif
