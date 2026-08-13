#include "color.h"

const char *color_or_default(int enabled, const char *on, const char *off) {
  return enabled ? on : (off ? off : "");
}
