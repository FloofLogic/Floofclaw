#include "floop.h"

#include "support/fsutil.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

int rt_floop_list_available(char *out, size_t out_len) {
  DIR *dir;
  struct dirent *entry;
  size_t used = 0;
  int count = 0;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  dir = opendir("floops");
  if (!dir) return -1;
  while ((entry = readdir(dir)) != NULL) {
    char path[512];
    int n;
    if (entry->d_name[0] == '.') continue;
    n = snprintf(path, sizeof(path), "floops/%s/loop.json", entry->d_name);
    if (n < 0 || (size_t)n >= sizeof(path) || !fs_file_exists(path)) continue;
    n = snprintf(out + used, out_len - used, "%s%s",
                 count ? ", " : "", entry->d_name);
    if (n < 0 || (size_t)n >= out_len - used) {
      closedir(dir);
      out[0] = '\0';
      return -1;
    }
    used += (size_t)n;
    count++;
  }
  closedir(dir);
  return count > 0 ? 0 : -1;
}

int rt_floop_loop_path(const char *name, char *out, size_t out_len) {
  char path[512];
  const char *suffix;
  if (!name || !*name || !out || out_len == 0) return -1;
  snprintf(path, sizeof(path), "floops/%s/loop.json", name);
  if (fs_file_exists(path)) {
    snprintf(out, out_len, "%s", path);
    return 0;
  }
  suffix = strstr(name, ".json");
  if (suffix && suffix[5] == '\0')
    snprintf(path, sizeof(path), "loops/%s", name);
  else
    snprintf(path, sizeof(path), "loops/%s.json", name);
  if (fs_file_exists(path)) {
    snprintf(out, out_len, "%s", path);
    return 0;
  }
  return -1;
}

int rt_floop_agent_dir(const char *floop_or_loop_name, const char *agent_id,
                       char *out, size_t out_len) {
  char path[512];
  if (!floop_or_loop_name || !*floop_or_loop_name || !agent_id || !*agent_id ||
      !out || out_len == 0) return -1;
  snprintf(path, sizeof(path), "floops/%s/agents/%s", floop_or_loop_name, agent_id);
  if (fs_dir_exists(path)) {
    snprintf(out, out_len, "%s", path);
    return 0;
  }
  snprintf(path, sizeof(path), "agents/%s", agent_id);
  if (fs_dir_exists(path)) {
    snprintf(out, out_len, "%s", path);
    return 0;
  }
  return -1;
}
