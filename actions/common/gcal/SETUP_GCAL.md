# Setting up the `gcal` action

`gcal` uses [`gog`](https://github.com/steipete/gogcli) for Google Calendar
and OAuth. FloofClaw remains the only durable credential store: every `gog`
invocation gets a new private home and encrypted file keyring, then that home
is removed. The action never reads your normal `gog` configuration or an OS
keychain.

Install `gog` 0.12.0 or newer (`brew install gogcli` on macOS), enable the
Google Calendar API, and create a Google OAuth client with application type
**Desktop app**. Download its JSON file. Do not use a Web application client.

## Add a client and account

Aliases are lowercase identifiers of at most 32 characters using letters,
digits, `_`, and `-`.

```sh
./bin/fclaw action auth -h --name gcal -- client add primary \
  --credentials ~/Downloads/client_secret_desktop.json

./bin/fclaw action auth -h --name gcal -- account add personal \
  --email you@example.com --client primary

./bin/fclaw action auth -h --name gcal -- authorize personal
```

The normal authorize command opens Google's browser consent flow. For a
browserless session use `--manual`. For a split remote flow:

```sh
./bin/fclaw action auth -h --name gcal -- authorize personal --remote --step 1

printf '%s' 'THE_COMPLETE_REDIRECT_URL' |
  ./bin/fclaw action auth -h --name gcal -- authorize personal \
    --remote --step 2 --redirect-url-stdin
```

Remote PKCE state is retained in the action's scoped secret namespace for at
most the short flow window; step 2 deletes it. The redirect URL is accepted
only on stdin, so its authorization code does not enter shell history or
process arguments. Never paste a redirect URL into Discord or model text.

## Multiple accounts and clients

Repeat `client add` and `account add` with different aliases. Accounts may
share one client or select different clients. If more than one account exists,
set an installation default or pass `account` in an action call:

```sh
./bin/fclaw action auth -h --name gcal -- client list
./bin/fclaw action auth -h --name gcal -- account list
./bin/fclaw action auth -h --name gcal -- account default personal
```

Without a default, an omitted account returns `ACCOUNT_REQUIRED`; the action
never guesses what “my calendar” means.

## Health and reauthorization

```sh
./bin/fclaw action auth -h --name gcal -- status personal
./bin/fclaw action auth -h --name gcal -- reauthorize personal --force-consent
```

The model-facing `kind:"diagnose"` reports the same health classes without
tokens, provider response bodies, or client secrets. Revoked or expired grants
return an exact safe reauthorization command. Transient failures are marked
retryable.

An External OAuth app left in Google's **Testing** publishing status can issue
refresh tokens that expire after seven days. For durable use, publish the
consent app to **In production**, then run `reauthorize ... --force-consent`
once to obtain a new refresh token.

## Original single-account credentials

Older installations used `action:gcal:client_id`, `client_secret`, and
`refresh_token`. They remain directly usable and authoritative; configure the
account email once without copying or renaming those three credentials:

```sh
./bin/fclaw auth set -h action:gcal:account_email you@example.com
```

When all four original fields are present, every ordinary call selects the
reserved `original` account and ignores requested/default aliases and newer
account or client records. Token rotation writes back to the original
`refresh_token` name. The action never copies or prints the three original
credential values.

The original client must be the existing Google Desktop OAuth client with
`http://localhost` registered. If the top-level client pair no longer matches
its downloaded JSON, restore it atomically without printing the secret:

```sh
./bin/fclaw action auth -h --name gcal -- client restore-original \
  --credentials ~/Downloads/client_secret_desktop.json --yes
```

Then `reauthorize original --force-consent` uses the normal local callback;
the browser returns directly to the waiting trusted operator process.

Migration into the multi-account layout remains available when an operator
explicitly wants it:

```sh
./bin/fclaw action auth -h --name gcal -- migrate \
  --client primary --account personal --email you@example.com --yes
```

Migration copies values without printing them, exercises the new live status
path, and deletes the legacy keys only after successful verification. On any
failure the old keys remain.

## Backup and removal

The portable records live in FloofClaw's `secrets/` directory (or below
`FCLAW_HOME`). Back up that directory with the same care as a password vault;
do not copy an ambient `gog` home because none is authoritative.

Removal requires explicit confirmation and refuses to remove a client still
referenced by an account:

```sh
./bin/fclaw action auth -h --name gcal -- account remove personal --yes
./bin/fclaw action auth -h --name gcal -- client remove primary --yes
```

Removing the local record does not revoke Google consent. Revoke the app at
<https://myaccount.google.com/permissions> when access must be invalidated at
Google too.
