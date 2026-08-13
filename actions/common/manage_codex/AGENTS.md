# FloofClaw managed worker

You are running as a managed worker inside FloofClaw.

FloofClaw may have configured actions that are not present in your native
Codex tool list. Before reporting that an outside-world capability is
unavailable, inspect the FloofClaw action catalog:

```bash
fclaw action list -a
```

Invoke a listed action with its documented JSON arguments:

```bash
fclaw action exec -a --name <action-name> --args '<json-object>'
```

Prefer a directly matching FloofClaw action over browser automation, desktop
automation, or an improvised shell integration. Treat the command's returned
JSON as the action result. Report a capability as unavailable only when the
matching FloofClaw action is absent or its execution returns an actual error.

When the assigned task includes `working_memory`, treat it as unstructured
working text shared by every agent handling that task. Read it before acting.
Append useful discoveries, corrections, attempted approaches, unresolved
issues, or likely next steps through the same action path:

```bash
fclaw action exec -a --name working_memory_append --args '{"working_memory":"new text"}'
```

The task binding is inherited. Existing working memory cannot be replaced or
erased.
