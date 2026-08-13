# Git and GitHub setup

The `git_github` action runs the normal `git` and GitHub CLI (`gh`) binaries
as the same operating-system account that runs the FloofClaw gateway. It does
not install packages, open an interactive login, or accept credentials in
action arguments. Instead, `kind:"check"` reports exactly what is missing and
returns copy-pasteable setup guidance to the user.

## 1. Install Git

On macOS:

```bash
xcode-select --install
```

Homebrew Git is also supported:

```bash
brew install git
```

On Debian or Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y git
```

For other platforms, use <https://git-scm.com/downloads>. Confirm that the
gateway account can find it:

```bash
git --version
```

## 2. Install GitHub CLI

Use the platform instructions at <https://cli.github.com/>. With Homebrew:

```bash
brew install gh
```

Confirm that the gateway account can find it:

```bash
gh --version
```

## 3. Authenticate GitHub

For a gateway running in your interactive user session:

```bash
gh auth login
gh auth status
```

Run those commands as the same OS account that launches FloofClaw. GitHub CLI
stores the login in that account's credential store.

For a headless gateway, store a suitably scoped token in the action's secret
namespace. The runtime exposes the value only to this action as `$GH_TOKEN`,
which `gh` reads automatically:

```bash
printf '%s' "$TOKEN" \
  | ./bin/fclaw auth set-stdin -h action:git_github:gh_token
```

Never pass a token in `argv`. Action arguments and command output are preserved
as run artifacts. The wrapper rejects `gh auth token`, `--show-token`, and
`--with-token` for the same reason.

After installation or secret changes, restart the gateway so its process sees
the updated `PATH` and environment.

## 4. Configure commit identity

Inside each repository that should create commits:

```bash
git config user.name "Your Name"
git config user.email "you@example.com"
```

Use `--global` only when one identity is correct for every repository used by
the gateway account.

## 5. Verify through FloofClaw

Ask FloofClaw to check Git and GitHub setup, or invoke the action directly:

```json
{
  "op": "start",
  "kind": "check",
  "repo": "repos/project"
}
```

Repository paths are relative to `FCLAW_WORKSPACE_ROOT`. The selected
directory must already exist. To clone a new repository, run `git clone` or
`gh repo clone` from an existing parent directory such as the workspace root.
