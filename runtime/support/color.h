#ifndef FCLAW_SUPPORT_COLOR_H
#define FCLAW_SUPPORT_COLOR_H

#define COLOR_RESET "\x1b[0m"

#define COLOR_PALETTE_01 "\x1b[38;5;229m"
#define COLOR_PALETTE_02 "\x1b[38;5;228m"
#define COLOR_PALETTE_03 "\x1b[38;5;227m"
#define COLOR_PALETTE_04 "\x1b[38;5;221m"
#define COLOR_PALETTE_05 "\x1b[38;5;215m"
#define COLOR_PALETTE_06 "\x1b[38;5;214m"
#define COLOR_PALETTE_07 "\x1b[38;5;179m"
#define COLOR_PALETTE_08 "\x1b[38;5;144m"
#define COLOR_PALETTE_09 "\x1b[38;5;110m"
#define COLOR_PALETTE_10 "\x1b[38;5;75m"
#define COLOR_PALETTE_11 "\x1b[38;5;81m"
#define COLOR_PALETTE_12 "\x1b[38;5;87m"
#define COLOR_PALETTE_13 "\x1b[38;5;86m"
#define COLOR_PALETTE_14 "\x1b[38;5;80m"
#define COLOR_PALETTE_15 "\x1b[38;5;74m"
#define COLOR_PALETTE_16 "\x1b[38;5;68m"
#define COLOR_PALETTE_17 "\x1b[38;5;62m"
#define COLOR_PALETTE_18 "\x1b[38;5;61m"
#define COLOR_PALETTE_19 "\x1b[38;5;97m"
#define COLOR_PALETTE_20 "\x1b[38;5;133m"
#define COLOR_PALETTE_21 "\x1b[38;5;169m"
#define COLOR_PALETTE_22 "\x1b[38;5;175m"
#define COLOR_PALETTE_23 "\x1b[38;5;177m"
#define COLOR_PALETTE_24 "\x1b[38;5;183m"

#define COLOR_YELLOW         COLOR_PALETTE_01
#define COLOR_BLUE_BRIGHT    COLOR_PALETTE_10
#define COLOR_ORANGE         COLOR_PALETTE_06
#define COLOR_CYAN           COLOR_PALETTE_12
#define COLOR_MAGENTA        COLOR_PALETTE_22
#define COLOR_WHITE          "\x1b[97m"
#define COLOR_BRIGHT_BLACK   "\x1b[90m"
#define COLOR_GREEN_BRIGHT   "\x1b[92m"
#define COLOR_RED_BRIGHT     "\x1b[91m"
#define COLOR_GREEN          "\x1b[32m"
#define COLOR_RED            "\x1b[31m"
#define COLOR_SECTION_HEADER COLOR_PALETTE_15

const char *color_or_default(int enabled, const char *on, const char *off);

#endif
