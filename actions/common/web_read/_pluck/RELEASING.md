# Releasing Pluck

Pluck has two histories with different jobs:

- `main` is the private development line on Vellum. It keeps the complete
  working history.
- `release` is the stable publication line on Vellum. It contains one snapshot
  commit per version and never merges back into `main`.
- The public GitHub repository exposes `release` as its `main` branch. The
  private development branch is never pushed there.

The release commit tree is identical to the approved development tree, but its
only parent is the previous release commit. Consequently, internal commits,
messages, authors, and abandoned files are not part of the public ancestry.

## First-time remote setup

Keep `origin` pointed at the private Vellum repository. Add the public GitHub
repository under the deliberately explicit name `github`:

```sh
git remote add github git@github.com:FloofLogic/pluck.git
git config remote.github.push refs/heads/release:refs/heads/main
```

Do not configure GitHub as a mirror and never push `main` to it.

## Release checklist

1. Finish and commit the release on `main`, including the version in
   `build.zig.zon` and any user-facing documentation.
2. Push `main` to Vellum and confirm the worktree is clean.
3. Write a notes file outside the repository. Each non-empty line must be a
   user-visible feature or fix beginning with `- `; do not describe the
   development process.
4. Stage the release commit and annotated tag:

   ```sh
   ./scripts/stage-release.sh vX.Y.Z --notes /path/to/release-notes.txt
   ```

   The script checks the version, branch, clean state, Vellum tracking ref,
   tests, forbidden paths, likely credentials, private machine paths,
   submodules, and oversized blobs. It constructs the commit with
   `git commit-tree`, so development ancestry cannot leak into `release`.
5. Review exactly what will become public:

   ```sh
   git show --stat release
   git show --format=fuller --no-ext-diff release
   git diff --stat release^ release        # omit release^ for the first release
   git ls-tree -r --name-only release
   ```

6. Publish the stable line to Vellum first:

   ```sh
   git push origin release
   git push origin vX.Y.Z
   ```

7. Publish only that stable line to GitHub:

   ```sh
   git fetch github main
   git push github release:main
   git push github vX.Y.Z
   ```

8. Verify the public boundary:

   ```sh
   git fetch github main
   test "$(git rev-parse release^{tree})" = \
     "$(git rev-parse refs/remotes/github/main^{tree})"
   if git merge-base --is-ancestor main refs/remotes/github/main; then
     echo "ERROR: private development ancestry is public" >&2
     exit 1
   fi
   git log --first-parent --format='%h %s' refs/remotes/github/main
   ```

The expected public log is short: one `Pluck vX.Y.Z` commit per released
version, with feature-oriented bullet points in each commit body.

## Later releases

Run the same checklist from the next tested `main`. The staging script uses the
current `release` tip as the new commit's only parent, producing a normal
fast-forward on both Vellum and GitHub. Never merge `release` back into `main`,
merge `main` into `release`, rebase the published release line, or force-push
the public branch.
