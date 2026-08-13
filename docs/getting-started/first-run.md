# First Run

**Preconditions:** run from the repository root after `make -j4`. The committed
default is the mock-backed `hello` floop, so no account, API key, gateway, or
network access is needed.

Missing-configuration and precondition errors include an inline `fix:`. Other
failures identify the operation that failed, so begin with the first error.

FloofClaw is agent-first at the command boundary: omitting an output flag is
the same as `-a` and returns compact JSON/JSONL. This walkthrough adds `-h`
where a person is expected to read the terminal. See the
[CLI output contract](../reference/cli.md#output-contract).

`./bin/fclaw setup -h` is an optional printed checklist. It does not generate,
rewrite, or validate configuration; the committed configuration is already
ready for the mock-backed path below.

## One-shot and gateway mode

`fclaw run -h --text` is the simplest terminal path: it submits one event,
temporarily hosts the runtime owner when no gateway is running, prints the
reply, and exits. Interactive `fclaw -i -h` is a client of the long-running
gateway and therefore needs `fclaw gateway start -h` first. Both paths use the
same scheduler, floop, bus, and event log.

## 1. Build and make one mock-backed run

```bash
make -j4
./bin/fclaw run -h --text "hello"
```

On an empty workspace a typical result begins like this:

```text
run_id: run_001
result: Hello! FloofClaw is running locally with the built-in mock model.
```

The runtime generates the run ID from the workspace. `run_001` is only the
empty-workspace example; use the ID printed by your command below:

Inspect the durable record created by that run:

```bash
RUN_ID=run_001  # replace with the generated ID printed above
ls "workspace/runs/$RUN_ID/"
./bin/fclaw replay -h "workspace/runs/$RUN_ID/event_log.jsonl"
./bin/fclaw view -h "$RUN_ID"
./bin/fclaw view -h -f -d "$RUN_ID"
./bin/fclaw view -a "$RUN_ID"
```

The directory contains `event_log.jsonl`, materialized `state.json`, durable
`runstate.json`, `trace.jsonl`, and agent/action artifacts. Runs that call an
LLM also contain per-call request and response artifacts under
`provider_calls/`. Replay reconstructs materialized state from the event log;
`view -h` shows the event timeline and timing summary, `-h -f -d` adds trace,
provider artifacts, state, and failure detail, and default/`-a` emits
event/trace JSONL.

## 2. Run the gateway and interactive client

Start the default `hello` floop as a background gateway:

```bash
./bin/fclaw gateway start -h
./bin/fclaw gateway status -h
```

In the same terminal, start the interactive client:

```bash
./bin/fclaw -i -h
```

Type `hello`, press Return, then press Ctrl-C to leave the client. Stop the
gateway after trying the bus without the interactive client:

```bash
./bin/fclaw bus publish -h --channel cli --text "hello via bus"
sleep 1
./bin/fclaw bus log -h -n 20
tail -n 5 workspace/logs/deliveries.jsonl
./bin/fclaw gateway stop -h
```

`bus log` shows the accepted inbound envelope; `deliveries.jsonl` shows the
matching outbound reply.

For attached logs instead of a daemon, use `./bin/fclaw gateway run -h`; stop it
with Ctrl-C.

## What you verified

- a clean build produces the runtime binary;
- the committed `hello` default returns a sensible reply without a key or
  network;
- one-shot mode creates a replayable event log and state snapshot;
- gateway mode owns the same runtime for interactive and bus clients.

Next, choose a product loop in [Choosing a floop](floops.md), then consult the
provider and channel guides linked from the repository README.
