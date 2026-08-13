#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/heap_guard.h"
#include "../test_support.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fs_read_text_rejects_over_cap_before_allocation(void) {
  static const char path[] = "workspace/tmp/fsutil_over_cap.txt";
  FcHeapStats before;
  FcHeapStats after;
  char *text = NULL;
  FILE *fp;
  int rc = 0;
  if (test_write_file(path, "123456") != 0)
    return expect(0, "write over-cap read fixture");
  fc_heap_snapshot(&before);
  rc |= expect(fs_read_text(path, &text, 5) == FS_READ_TOO_LARGE,
               "over-cap read returns FS_READ_TOO_LARGE");
  fc_heap_snapshot(&after);
  rc |= expect(text == NULL, "over-cap read leaves output null");
  rc |= expect(after.mallocs == before.mallocs,
               "over-cap read rejects before allocating payload storage");
  fp = fopen(path, "wb");
  if (!fp) return expect(0, "open sparse default-cap fixture");
  if (ftruncate(fileno(fp), (off_t)FS_READ_TEXT_DEFAULT_CAP + 1) != 0) {
    fclose(fp);
    return expect(0, "size sparse default-cap fixture");
  }
  if (fclose(fp) != 0)
    return expect(0, "create sparse default-cap fixture");
  fc_heap_snapshot(&before);
  rc |= expect(fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) ==
                   FS_READ_TOO_LARGE,
               "default 64 MiB hard ceiling rejects an oversized file");
  fc_heap_snapshot(&after);
  rc |= expect(text == NULL, "default-cap rejection leaves output null");
  rc |= expect(after.mallocs == before.mallocs,
               "default-cap rejection occurs before payload allocation");
  (void)unlink(path);
  return rc;
}

int fs_read_tail_starts_at_complete_record(void) {
  static const char path[] = "workspace/tmp/fsutil_tail.jsonl";
  char *text = NULL;
  int rc = 0;
  if (test_write_file(path, "first\nsecond\nthird\n") != 0)
    return expect(0, "write tail-window fixture");
  rc |= expect(fs_read_tail(path, &text, 10) == 0,
               "tail-window read succeeds");
  rc |= expect(text && strcmp(text, "third\n") == 0,
               "tail-window drops a partial first record");
  fc_xfree(text);
  (void)unlink(path);
  return rc;
}

int fs_write_text_atomic_replaces_and_cleans_temp(void) {
  static const char path[] = "workspace/tmp/atomic_state.json";
  static const char prefix[] = "atomic_state.json.tmp.";
  char *text = NULL;
  DIR *dir;
  struct dirent *ent;
  struct stat st;
  int found_temp = 0;
  int rc = 0;
  if (test_write_file(path, "before\n") != 0)
    return expect(0, "write atomic replacement fixture");
  rc |= expect(chmod(path, 0600) == 0, "set atomic fixture mode");
  rc |= expect(fs_write_text_atomic(path, "after\n") == 0,
               "atomic writer replaces existing state");
  rc |= expect(fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) == 0,
               "read atomically replaced state");
  rc |= expect(text && strcmp(text, "after\n") == 0,
               "atomic replacement exposes complete new content");
  rc |= expect(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600,
               "atomic replacement preserves existing file mode");
  fc_xfree(text);
  dir = opendir("workspace/tmp");
  if (!dir) return rc | expect(0, "open atomic fixture directory");
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, prefix, strlen(prefix)) == 0) {
      found_temp = 1;
      break;
    }
  }
  closedir(dir);
  rc |= expect(!found_temp, "atomic writer leaves no temporary file");
  (void)unlink(path);
  return rc;
}

/* #10: POSIX readdir leaves errno untouched at EOF, so stale errno from
 * work between entries must not read as a scan failure. The wrapper
 * clears errno before each real readdir call; NULL + errno==0 is always
 * clean EOF, and the injected-EIO path still reports loudly. */
int readdir_checked_clears_stale_errno_at_eof(void) {
  static const char dirpath[] = "workspace/tmp/readdir_errno";
  struct dirent *e;
  DIR *d;
  int rc = 0;
  int entries = 0;

  test_remove_path(dirpath);
  if (test_mkdir_p(dirpath) != 0) return expect(0, "mkdir fixture dir");
  if (test_write_file("workspace/tmp/readdir_errno/a.txt", "a") != 0 ||
      test_write_file("workspace/tmp/readdir_errno/b.txt", "b") != 0)
    return expect(0, "write fixture entries");

  d = opendir(dirpath);
  if (!d) return expect(0, "open fixture dir");
  fs_test_reset_readdir();
  for (;;) {
    errno = EINVAL; /* simulate stale errno from between-entry work */
    e = fs_readdir_checked(d);
    if (!e) break;
    entries++;
  }
  rc |= expect(errno == 0, "EOF with stale errno reads clean");
  rc |= expect(entries >= 2, "walk saw the fixture entries");
  closedir(d);

  d = opendir(dirpath);
  if (!d) return expect(0, "reopen fixture dir");
  fs_test_fail_readdir_after(0);
  errno = 0;
  e = fs_readdir_checked(d);
  rc |= expect(e == NULL && errno == EIO,
               "injected failure still reports EIO loudly");
  fs_test_reset_readdir();
  closedir(d);
  test_remove_path(dirpath);
  return rc;
}
