#ifndef FCLAW_TEST_SUPPORT_H
#define FCLAW_TEST_SUPPORT_H

#include <stddef.h>

int expect(int cond, const char *msg);
int expect_not(int cond, const char *msg);
int expect_substr(const char *haystack, const char *needle, const char *msg);
int expect_no_substr(const char *haystack, const char *needle, const char *msg);
int expect_file_exists(const char *path);
int expect_file_not_exists(const char *path);

int test_reset_workspace(void);
int test_config_enable_all(char **saved);
void test_config_restore(char *saved);
int test_mkdir_p(const char *path);
int test_write_file(const char *path, const char *text);
int test_read_file(const char *path, char **out);
int test_agent_output_path(const char *run_id, const char *phase,
                           const char *suffix, char *out, size_t out_len);
int test_read_agent_output(const char *run_id, const char *phase,
                           const char *suffix, char **out);
int test_agent_output_contains(const char *run_id, const char *phase,
                               const char *suffix, const char *needle);
int expect_agent_output_exists(const char *run_id, const char *phase,
                               const char *suffix);
int expect_agent_output_not_exists(const char *run_id, const char *phase,
                                   const char *suffix);
long test_file_size(const char *path);
int test_file_contains(const char *path, const char *needle);
int test_copy_fixture_dir(const char *src, const char *dst);
int test_copy_fixture_file(const char *src, const char *dst);
int test_remove_path(const char *path);
int test_install_agent_fixture(const char *id);
int test_install_action_fixture(const char *id);
int test_install_loop_fixture(const char *file_name);
int test_cleanup_fixtures(void);
int test_run_with_text(const char *loop, const char *text, char *run_id, size_t run_id_len, char **out);
int test_latest_run_id(char *out, size_t out_len);
int test_event_log_path(const char *run_id, char *out, size_t out_len);
int test_count_event_type(const char *run_id, const char *type);
int test_count_substr(const char *text, const char *needle);
int test_extract_run_id(const char *text, char *out, size_t out_len);
int test_read_run_event_log(const char *run_id, char **out);
int test_stat_mtime_size(const char *path, long *mtime_out, long *size_out);
int test_gateway_fast_loop(int events, char **out_log);

#endif
