#include "../test_support.h"

#include "../../runtime/bus/media_manifest.h"
#include "../../runtime/llm/media.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int fd_write_all(int fd, const void *bytes, size_t len) {
  const unsigned char *p = (const unsigned char *)bytes;
  size_t off = 0U;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static int spawn_one_response_server(const unsigned char *body,
                                     size_t body_len,
                                     size_t declared_len,
                                     pid_t *pid_out,
                                     unsigned *port_out) {
  struct sockaddr_in address;
  socklen_t address_len = sizeof(address);
  int listener;
  pid_t pid;
  if (!body || !pid_out || !port_out) return -1;
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(listener, 1) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
    close(listener);
    return -1;
  }
  *port_out = (unsigned)ntohs(address.sin_port);
  pid = fork();
  if (pid < 0) {
    close(listener);
    return -1;
  }
  if (pid == 0) {
    char request[2048];
    char header[512];
    int client;
    int header_len;
    alarm(10);
    client = accept(listener, NULL, NULL);
    if (client < 0) _exit(2);
    (void)read(client, request, sizeof(request));
    header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/png; charset=binary\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        declared_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
        fd_write_all(client, header, (size_t)header_len) != 0 ||
        fd_write_all(client, body, body_len) != 0) {
      close(client);
      close(listener);
      _exit(3);
    }
    close(client);
    close(listener);
    _exit(0);
  }
  close(listener);
  *pid_out = pid;
  return 0;
}

static int spawn_stalled_server(unsigned stall_ms,
                                pid_t *pid_out, unsigned *port_out,
                                int *accepted_fd_out) {
  struct sockaddr_in address;
  socklen_t address_len = sizeof(address);
  int accepted_pipe[2];
  int listener;
  pid_t pid;
  if (!pid_out || !port_out || !accepted_fd_out ||
      pipe(accepted_pipe) != 0)
    return -1;
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    close(accepted_pipe[0]);
    close(accepted_pipe[1]);
    return -1;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(listener, 1) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
    close(listener);
    close(accepted_pipe[0]);
    close(accepted_pipe[1]);
    return -1;
  }
  *port_out = (unsigned)ntohs(address.sin_port);
  pid = fork();
  if (pid < 0) {
    close(listener);
    close(accepted_pipe[0]);
    close(accepted_pipe[1]);
    return -1;
  }
  if (pid == 0) {
    char request[2048];
    char ready = '1';
    int client;
    close(accepted_pipe[0]);
    alarm(5);
    client = accept(listener, NULL, NULL);
    if (client < 0 ||
        fd_write_all(accepted_pipe[1], &ready, 1U) != 0)
      _exit(2);
    (void)read(client, request, sizeof(request));
    usleep((useconds_t)stall_ms * 1000U);
    close(client);
    close(listener);
    close(accepted_pipe[1]);
    _exit(0);
  }
  close(listener);
  close(accepted_pipe[1]);
  *pid_out = pid;
  *accepted_fd_out = accepted_pipe[0];
  return 0;
}

static int wait_server(pid_t pid) {
  int status;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int file_equals(const char *path,
                       const unsigned char *expected, size_t expected_len) {
  unsigned char actual[64];
  struct stat st;
  ssize_t n;
  int fd;
  if (expected_len > sizeof(actual) ||
      lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != getuid() || (st.st_mode & 077U) != 0U ||
      (size_t)st.st_size != expected_len)
    return 0;
  fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  do {
    n = read(fd, actual, sizeof(actual));
  } while (n < 0 && errno == EINTR);
  close(fd);
  return n == (ssize_t)expected_len &&
         memcmp(actual, expected, expected_len) == 0;
}

static int count_part_files(const char *dir_path) {
  DIR *dir = opendir(dir_path);
  struct dirent *entry;
  int count = 0;
  if (!dir) return -1;
  while ((entry = readdir(dir)) != NULL)
    if (strstr(entry->d_name, ".part.")) count++;
  closedir(dir);
  return count;
}

int llm_media_downloader_is_bounded_atomic_and_cacheable(void) {
#ifndef FCLAW_HAVE_LIBCURL
  return 0;
#else
  static const unsigned char complete[] = {'P', 'N', 'G', '!'};
  static const unsigned char truncated[] = {'P', 'N'};
  FcMediaDescriptor descriptor;
  FcMediaManifestRef ref;
  LlmMedia *media = NULL;
  size_t media_count = 0U;
  uint64_t media_bytes = 0U;
  pid_t server = -1;
  pid_t loader = -1;
  unsigned port = 0U;
  int accepted_fd = -1;
  char source_url[256];
  char manifest_path[256];
  char err[512];
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= test_mkdir_p("workspace/runs/run_001");
  rc |= test_mkdir_p("workspace/runs/run_001/media");
  rc |= test_write_file(
      "workspace/runs/run_001/media/orphan.bin.part.1.0", "stale");
  rc |= expect(chmod(
                   "workspace/runs/run_001/media/orphan.bin.part.1.0",
                   0600) == 0,
               "prepare owner-private stale partial");
  rc |= expect(setenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK", "1", 1) == 0,
               "enable explicit media loopback test scope");
  rc |= expect(spawn_one_response_server(
                   complete, sizeof(complete), sizeof(complete),
                   &server, &port) == 0,
               "start exact media fixture server");
  snprintf(source_url, sizeof(source_url),
           "http://127.0.0.1:%u/sample.png", port);
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.id = "fixture-1";
  descriptor.filename = "sample.png";
  descriptor.media_type = "image/png";
  descriptor.size_bytes = sizeof(complete);
  descriptor.source_url = source_url;
  rc |= expect(fc_media_manifest_write(&descriptor, 1U, &ref,
                                       err, sizeof(err)) == 0,
               err[0] ? err : "write loopback media manifest");
  snprintf(manifest_path, sizeof(manifest_path), "workspace/%s", ref.manifest);
  rc |= expect(llm_media_load("workspace/runs/run_001", manifest_path,
                              &media, &media_count, &media_bytes,
                              err, sizeof(err)) == 0,
               err[0] ? err : "download exact media fixture");
  rc |= expect(media && media_count == 1U &&
                   media_bytes == sizeof(complete),
               "loader returns one lightweight local descriptor");
  if (media) {
    rc |= expect(file_equals(media[0].path, complete, sizeof(complete)),
                 "downloaded file is exact, private, and regular");
  }
  rc |= expect(count_part_files("workspace/runs/run_001/media") == 0,
               "loader prunes stale partials before downloading");
  rc |= expect(wait_server(server) == 0, "fixture server exits cleanly");
  server = -1;
  llm_media_dispose(media);
  media = NULL;

  /* No server is listening now. A second load can only succeed by validating
   * and reusing the deterministic run-local cache. */
  rc |= expect(llm_media_load("workspace/runs/run_001", manifest_path,
                              &media, &media_count, &media_bytes,
                              err, sizeof(err)) == 0,
               "verified run-local media is reused without redownload");
  llm_media_dispose(media);
  media = NULL;

  rc |= test_mkdir_p("workspace/runs/run_002");
  rc |= expect(spawn_one_response_server(
                   truncated, sizeof(truncated), sizeof(complete),
                   &server, &port) == 0,
               "start truncated media fixture server");
  snprintf(source_url, sizeof(source_url),
           "http://127.0.0.1:%u/truncated.png", port);
  descriptor.id = "fixture-2";
  descriptor.source_url = source_url;
  rc |= expect(fc_media_manifest_write(&descriptor, 1U, &ref,
                                       err, sizeof(err)) == 0,
               "write truncated media manifest");
  snprintf(manifest_path, sizeof(manifest_path), "workspace/%s", ref.manifest);
  rc |= expect(llm_media_load("workspace/runs/run_002", manifest_path,
                              &media, &media_count, &media_bytes,
                              err, sizeof(err)) != 0,
               "truncated transfer is rejected");
  rc |= expect_substr(err, "download failed",
                      "truncated transfer reports a bounded reason");
  rc |= expect(wait_server(server) == 0, "truncated server exits cleanly");
  server = -1;
  rc |= expect(count_part_files("workspace/runs/run_002/media") == 0,
               "failed download removes every partial file");

  rc |= test_mkdir_p("workspace/runs/run_003");
  rc |= expect(spawn_stalled_server(300U, &server, &port,
                                    &accepted_fd) == 0,
               "start stalled media fixture server");
  snprintf(source_url, sizeof(source_url),
           "http://127.0.0.1:%u/stalled.png", port);
  descriptor.id = "fixture-3";
  descriptor.source_url = source_url;
  rc |= expect(fc_media_manifest_write(&descriptor, 1U, &ref,
                                       err, sizeof(err)) == 0,
               "write stalled media manifest");
  snprintf(manifest_path, sizeof(manifest_path), "workspace/%s", ref.manifest);
  rc |= expect(setenv("FCLAW_MEDIA_TEST_DOWNLOAD_BUDGET_MS",
                      "100", 1) == 0,
               "set a short hermetic aggregate download budget");
  rc |= expect(llm_media_load("workspace/runs/run_003", manifest_path,
                              &media, &media_count, &media_bytes,
                              err, sizeof(err)) != 0,
               "aggregate media deadline stops a stalled transfer");
  rc |= expect(count_part_files("workspace/runs/run_003/media") == 0,
               "deadline failure removes its partial file");
  if (accepted_fd >= 0) close(accepted_fd);
  accepted_fd = -1;
  rc |= expect(wait_server(server) == 0, "stalled server exits cleanly");
  server = -1;
  (void)unsetenv("FCLAW_MEDIA_TEST_DOWNLOAD_BUDGET_MS");

  rc |= test_mkdir_p("workspace/runs/run_004");
  rc |= expect(spawn_stalled_server(300U, &server, &port,
                                    &accepted_fd) == 0,
               "start SIGTERM cleanup fixture server");
  snprintf(source_url, sizeof(source_url),
           "http://127.0.0.1:%u/terminated.png", port);
  descriptor.id = "fixture-4";
  descriptor.source_url = source_url;
  rc |= expect(fc_media_manifest_write(&descriptor, 1U, &ref,
                                       err, sizeof(err)) == 0,
               "write SIGTERM cleanup media manifest");
  snprintf(manifest_path, sizeof(manifest_path), "workspace/%s", ref.manifest);
  loader = fork();
  if (loader == 0) {
    LlmMedia *child_media = NULL;
    size_t child_count = 0U;
    uint64_t child_bytes = 0U;
    char child_err[256];
    int child_rc = llm_media_load(
        "workspace/runs/run_004", manifest_path,
        &child_media, &child_count, &child_bytes,
        child_err, sizeof(child_err));
    llm_media_dispose(child_media);
    _exit(child_rc == 0 ? 0 : 1);
  }
  rc |= expect(loader > 0, "fork media loader for SIGTERM cleanup");
  if (loader > 0 && accepted_fd >= 0) {
    char ready;
    rc |= expect(read(accepted_fd, &ready, 1U) == 1,
                 "loader reached the stalled transfer");
    rc |= expect(kill(loader, SIGTERM) == 0,
                 "terminate loader while a partial file is active");
    {
      int status = 0;
      pid_t waited;
      do {
        waited = waitpid(loader, &status, 0);
      } while (waited < 0 && errno == EINTR);
      rc |= expect(waited == loader && WIFEXITED(status) &&
                       WEXITSTATUS(status) == 143,
                   "media SIGTERM handler exits after cleanup");
    }
    loader = -1;
  } else if (loader <= 0 && server > 0) {
    (void)kill(server, SIGKILL);
  }
  if (accepted_fd >= 0) close(accepted_fd);
  accepted_fd = -1;
  rc |= expect(wait_server(server) == 0,
               "SIGTERM cleanup server exits cleanly");
  server = -1;
  rc |= expect(count_part_files("workspace/runs/run_004/media") == 0,
               "SIGTERM cleanup leaves no partial file");

  llm_media_dispose(media);
  if (loader > 0) {
    (void)kill(loader, SIGKILL);
    (void)waitpid(loader, NULL, 0);
  }
  if (accepted_fd >= 0) close(accepted_fd);
  if (server > 0) {
    (void)kill(server, SIGKILL);
    (void)waitpid(server, NULL, 0);
  }
  unsetenv("FCLAW_MEDIA_TEST_DOWNLOAD_BUDGET_MS");
  unsetenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK");
  return rc;
#endif
}
