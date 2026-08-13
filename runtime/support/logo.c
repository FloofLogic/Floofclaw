#include "logo.h"
#include "color.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *g_logo_art[] = {
    "⠀⠀⠀⠔⠢⠀⡄⠢⡀⠀⠀",
    "⡠⢄⡰⠀⠀⣦⠁⠀⡇⡠⢄",
    "⢇⠀⠱⠣⡴⠥⢦⡔⡎⠀⢸",
    "⠈⠒⡸⠎⠀⠀⠀⠱⢕⠐⠁",
    "⠀⠠⡀⠀⠀⠀⠀⠀⢀⠆⠀",
    "⠀⠀⠑⠠⠔⠒⠢⠤⠊⠀⠀",
    NULL,
};

static const char *g_logo_text[] = {
    "▄▖▜     ▐▘  ▜      ",
    "▙▖▐ ▛▌▛▌▜▘▛▘▐ ▀▌▌▌▌",
    "▌ ▐▖▙▌▙▌▐ ▙▖▐▖█▌▚▚▘",
    NULL,
};

static const char *g_logo_tag = "tiny code. sharp claws.";

static const char *g_palette[] = {
    COLOR_PALETTE_01, COLOR_PALETTE_02, COLOR_PALETTE_03, COLOR_PALETTE_04,
    COLOR_PALETTE_05, COLOR_PALETTE_06, COLOR_PALETTE_07, COLOR_PALETTE_08,
    COLOR_PALETTE_09, COLOR_PALETTE_10, COLOR_PALETTE_11, COLOR_PALETTE_12,
    COLOR_PALETTE_13, COLOR_PALETTE_14, COLOR_PALETTE_15, COLOR_PALETTE_16,
    COLOR_PALETTE_17, COLOR_PALETTE_18, COLOR_PALETTE_19, COLOR_PALETTE_20,
    COLOR_PALETTE_21, COLOR_PALETTE_22, COLOR_PALETTE_23, COLOR_PALETTE_24,
};

static int use_color(FILE *out) {
  return out && fileno(out) >= 0 && isatty(fileno(out));
}

static void prepare_locale(void) {
  static int prepared = 0;
  if (!prepared) {
    (void)setlocale(LC_CTYPE, "");
    prepared = 1;
  }
}

static int utf8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xe0) == 0xc0) return 2;
  if ((c & 0xf0) == 0xe0) return 3;
  if ((c & 0xf8) == 0xf0) return 4;
  return 1;
}

static void write_colored(FILE *out, const char *s, int width, int tick,
                          int color_spaces, int colored) {
  const unsigned char *p = (const unsigned char *)s;
  int i = 0;
  int palette_len = (int)(sizeof(g_palette) / sizeof(g_palette[0]));
  if (!out || !p || width <= 0) return;
  while (*p) {
    int n = utf8_len(*p);
    if (colored && (color_spaces || *p != ' ')) {
      int idx = width > 1
                    ? (((i * (palette_len - 1)) / (width - 1)) + tick) % palette_len
                    : tick % palette_len;
      fputs(g_palette[idx], out);
      (void)fwrite(p, 1U, (size_t)n, out);
      fputs(COLOR_RESET, out);
    } else {
      (void)fwrite(p, 1U, (size_t)n, out);
    }
    p += n;
    ++i;
  }
}

static void draw_frame_at(FILE *out, int top, int left, int tick, int colored) {
  int row;
  if (!out) return;
  if (top <= 0) top = 1;
  if (left <= 0) left = 1;
  prepare_locale();
  for (row = 0; g_logo_art[row]; ++row) {
    fprintf(out, "\033[%d;%dH\033[2K", top + row, left);
    write_colored(out, g_logo_art[row], 16, tick, 1, colored);
    if (row < 3) {
      fputs("  ", out);
      write_colored(out, g_logo_text[row], 19, tick, 0, colored);
    } else if (row == 4) {
      fputs("   ", out);
      write_colored(out, g_logo_tag, 26, tick, 1, colored);
    }
    fputs("\033[0m", out);
  }
}

static void draw_frame(FILE *out, int tick, int colored, int clear_lines) {
  int row;
  if (!out) return;
  prepare_locale();
  for (row = 0; g_logo_art[row]; ++row) {
    if (clear_lines) fputs("\r\x1b[2K", out);
    write_colored(out, g_logo_art[row], 16, tick, 1, colored);
    if (row < 3) {
      fputs("  ", out);
      write_colored(out, g_logo_text[row], 19, tick, 0, colored);
    } else if (row == 4) {
      fputs("   ", out);
      write_colored(out, g_logo_tag, 26, tick, 1, colored);
    }
    fputc('\n', out);
  }
}

void fclaw_logo_print_frame_at(FILE *out, int top, int left, int tick) {
  if (!out) return;
  draw_frame_at(out, top, left, tick, use_color(out));
}

void fclaw_logo_print(FILE *out) {
  if (!out) return;
  draw_frame(out, 0, use_color(out), 0);
  fflush(out);
}

void fclaw_logo_print_frame(FILE *out, int tick, int clear_lines) {
  if (!out) return;
  draw_frame(out, tick, use_color(out), clear_lines);
}

int fclaw_logo_loop(FILE *out, volatile sig_atomic_t *stop_flag) {
  int tick = 0;
  if (!out) return -1;
  if (!use_color(out)) {
    fclaw_logo_print(out);
    return 0;
  }
  fputs("\x1b[?25l", out); /* hide cursor */
  draw_frame(out, tick++, 1, 0);
  fflush(out);
  while (!stop_flag || !*stop_flag) {
    struct timespec ts = {0, 60 * 1000000L};
    nanosleep(&ts, NULL);
    if (stop_flag && *stop_flag) break;
    fprintf(out, "\r\x1b[%dA", FCLAW_LOGO_ROWS);
    draw_frame(out, tick++, 1, 1);
    fflush(out);
  }
  fputs("\x1b[?25h", out); /* show cursor */
  fflush(out);
  return 0;
}
