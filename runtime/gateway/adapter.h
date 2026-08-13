#ifndef FCLAW_ADAPTER_H
#define FCLAW_ADAPTER_H

#include <stddef.h>

#include "reactor.h"

/* A channel adapter is a self-contained source directory under adapters/.
 *
 * To add one:
 *   1. create adapters/<name>/
 *   2. export a `const FcAdapter fc_adapter_<name>`
 *   3. build FloofClaw
 *
 * That is the entire contract. There is no registration list: the build
 * discovers the directories under adapters/ and generates the extern
 * declarations and the fc_adapters[] array from their names. Nothing is
 * dynamically
 * loaded — an adapter is statically linked into the same single binary, and
 * a directory that fails to export its descriptor fails the link naming the
 * missing symbol.
 *
 * FcAdapter describes how an adapter is *integrated*: how the gateway builds
 * and tears one down. FcReactorModule (reactor.h) describes how the resulting
 * thing *participates* in the reactor loop, and holds the real callbacks. The
 * two stay separate on purpose; this struct deliberately does not wrap them.
 */

/* Forward-declared so an adapter descriptor costs no kernel header. */
struct RtScheduler;

typedef struct {
  /* Also the config key: configuration is read from channels.<name> in
   * config/floofclaw_config.json. */
  const char *name;

  /* Build the adapter's reactor module.
   *
   * Returns NULL when the adapter is disabled or unconfigured — that is the
   * normal "nothing to register" path and is not an error. Sets
   * *startup_error non-zero when the adapter is enabled but cannot start,
   * which aborts gateway startup. An adapter needing a build dependency the
   * binary lacks (OpenSSL, libcurl) reports that itself here rather than
   * disappearing silently.
   *
   * `sched` is the live scheduler. Adapters that do not need it ignore it;
   * one uniform signature is what lets the generated registry need nothing
   * from a directory but its name. */
  FcReactorModule *(*create)(struct RtScheduler *sched, int *startup_error);

  void (*destroy)(FcReactorModule *module);
} FcAdapter;

/* Generated at build time from the adapters/ directory listing, sorted by
 * name. Never hand-edited; see the ADAPTER_REGISTRY rule in the Makefile. */
extern const FcAdapter *const fc_adapters[];
extern const size_t fc_adapter_count;

#endif
