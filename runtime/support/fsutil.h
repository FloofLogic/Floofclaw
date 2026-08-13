#ifndef FCLAW_SUPPORT_FSUTIL_H
#define FCLAW_SUPPORT_FSUTIL_H

#include <stddef.h>
#include <dirent.h>

#define FS_READ_TEXT_DEFAULT_CAP (64U * 1024U * 1024U)
#define FS_READ_TOO_LARGE (-2)

int fs_mkdir_p(const char *path);
int fs_file_exists(const char *path);
int fs_dir_exists(const char *path);
int fs_write_text(const char *path, const char *text);
int fs_write_text_atomic(const char *path, const char *text);
int fs_append_text(const char *path, const char *text);
int fs_append_text_sync(const char *path, const char *text);
/* JSONL newlines are commit markers. If `path` ends in an incomplete
 * record, truncate only that final record and fsync the repair. */
int fs_truncate_incomplete_line(const char *path);
void fs_test_fail_next_append_sync(void);
unsigned long fs_test_append_sync_attempts(void);
void fs_test_reset_append_sync(void);
struct dirent *fs_readdir_checked(DIR *dir);
void fs_test_fail_readdir_after(long successful_calls);
void fs_test_reset_readdir(void);
int fs_read_text(const char *path, char **out_text, size_t max_bytes);
int fs_read_tail(const char *path, char **out_text, size_t max_bytes);
int fs_join(char *out, size_t out_len, const char *left, const char *right);
void fs_trim_newline(char *text);

#endif
