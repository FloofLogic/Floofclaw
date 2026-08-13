# Common actions

`actions/common/` holds bundled actions shared by the shipped general-purpose
floops. The runtime discovers these directories recursively like every other
action area; `common` is a display grouping, not part of an action's ID.

Current actions:

- `gcal` — Google Calendar operations and account setup; see
  [`gcal/SETUP_GCAL.md`](gcal/SETUP_GCAL.md)
- `git_github` — bounded Git and GitHub CLI operations; see
  [`git_github/SETUP_GIT_GITHUB.md`](git_github/SETUP_GIT_GITHUB.md)
- `http_call` — bounded HTTP requests
- `manage_codex` — managed Codex worker lifecycle
- `manage_claude` — managed Claude Code worker lifecycle
- `manage_hermes` — managed Hermes worker lifecycle
- `web_read` — readable page extraction through the public
  [FloofLogic/pluck](https://github.com/FloofLogic/pluck) source

Operator setup belongs inside the action directory as `SETUP_<NAME>.md`.
Development clones initialize the action-owned public `pluck` submodule and
build it once:

```bash
git submodule update --init actions/common/web_read/_pluck
bash actions/common/web_read/build.sh
```

Public FloofClaw source releases flatten the pinned source, so their first
command is needed only in development clones.
