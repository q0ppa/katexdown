# AUR build flow — the prepare() overlay must never copy .git

## Goal / problem

`aur/PKGBUILD` builds straight from the repository that ships it (no AUR
entry needed). The source is a local `git+file://` clone of the repo's parent
directory, so it only contains *committed* state. To also build uncommitted
working-tree changes, `prepare()` overlays the live repository over the
makepkg work copy with a tar pipe.

The failure: the overlay initially copied the repository's **`.git`
directory** too (nothing excluded it). A makepkg work copy is not an ordinary
clone — `libmakepkg/source/git.sh` keeps a `--mirror` cache of the source in
`$SRCDEST` (= the `aur/` dir) and creates the work copy in `$srcdir` bound to
that mirror, so that repeat builds update from the *local source* and work
offline. Overwriting the work copy's `.git` rewrote its `origin` to the real
repository's remote (`git@github.com:q0ppa/katexdown.git`) and replaced its
refs/reflog with the repo's. The next `makepkg` run then took the "updating
working copy" path of `extract_git` and ran a plain `git fetch` — against
**GitHub over SSH** instead of the local `file://` source. Result: a build
that succeeds once from a clean directory, then breaks on every later run
with `Failure while updating working copy of ... git repo` (no SSH auth /
network: `Could not read from remote repository`).

## Design (the mechanism, and why this shape)

- `source=("$_pkgbase::git+file://${PWD%/*}")` — clone the repo that the
  `aur/` directory lives in. `sha256sums=('SKIP')`, git sources are
  checksummed by ref semantics only.
- `prepare()` overlays the live tree: `tar --exclude='./build*'
  --exclude='./runtime' --exclude='./aur' --exclude='./.git' -C "$startdir/.."
  -cf - . | tar -xf -`. The exclusions are load-bearing:
  - `./build*`, `./runtime`, `./aur` — bulky/dev dirs that must not replace
    the work copy's own `build/` (created by cmake later) or drag megabytes
    of downloaded assets into the package build.
  - `./.git` — must never reach the work copy (see Goal/problem). The
    committed state is already there via the file:// clone, so `.git` in the
    overlay can only corrupt the update machinery, never add anything.
- `pkgver()` runs `git describe` on the work copy after `prepare()`; because
  `.git` is now makepkg's own (bound to the mirror, objects shared), it
  reports the version of the fetched source — matching the committed
  `pkgver=` line workflow (bump `pkgver=` one commit behind, let `pkgver()`
  rewrite it during the build).

## No AUR publish workflow

There is intentionally **no** `.github/workflows/publish-aur.yml`. A workflow that
regenerated `.SRCINFO` and pushed `aur/PKGBUILD` to the AUR used to exist and
failed on every run, for three independent reasons — any one of them is fatal,
so it was removed rather than patched:

- **The PKGBUILD is not AUR-installable.** Its `source=` is
  `git+file://${PWD%/*}`, a local clone that only exists when `makepkg` runs
  from inside this repository. On an AUR user's machine there is no such path.
  The whole design above depends on this local source; changing it to
  `git+https://…` to satisfy the AUR would drop the "builds your working tree"
  guarantee this file documents.
- **It pushed to the wrong package.** The remote was
  `ssh://aur@aur.archlinux.org/katdown-git.git` while the PKGBUILD's `pkgname`
  is `katexdown-git`. Worse, `katdown-git` on the AUR is upstream
  (maintained by `uwuclxdy`), not this fork — a push can only ever be rejected
  for lack of ownership rights, and it risks colliding with upstream's package.
- **The credentials are not part of the repo.** The job needs
  `AUR_SSH_PRIVATE_KEY`/`AUR_USERNAME` secrets registered to an AUR account that
  owns the target package. Forks (and this repo) never had them, so the job
  failed before it could even reach the push.

`aur/prepare-aur.sh` (`updpkgsums` + `makepkg --printsrcinfo`) stays: it is a
local developer convenience for hand-publishing a *different* package later,
not something CI runs.

## Invariants

- A repeat `makepkg` run in `aur/` (existing `src/`, `pkg/` and `$SRCDEST`
  mirror from a previous build) must succeed from the local source alone —
  no network, no SSH, no GitHub access required after the first clone.
- The work copy's `origin` after `prepare()` must still be the local
  `$SRCDEST` mirror (path or `file://`), never the repository's real remote.
- The overlay must not delete or replace anything in the work copy that the
  current run's `build()`/`package()` stages created earlier — it runs
  before them, so this reduces to "skip dev/build dirs".
- Uncommitted working-tree changes in the repository must still be built.

## Pitfalls

- **`.git` is invisible in `tar -C dir -cf - .` listings** unless you
  deliberately exclude it — the overlay bug shipped for several builds
  because `--exclude='./build*'` style patterns read like they cover "dev
  junk" while the metadata dir slips through. If the exclusion list ever
  changes, `.git` must stay on it.
- **A stale poisoned work copy outlives the code fix.** A `src/` work copy
  whose `.git` was already overwritten still points `origin` at GitHub; the
  PKGBUILD fix prevents *new* poisoning, it does not repair old debris —
  remove `aur/src` (and, if a previous run died mid-packaging left it with
  `d--x--x--x` perms, `aur/pkg`) before rebuilding.
- **Repeated `makepkg` runs leave debris** in the repo: `aur/katexdown`
  (the `$SRCDEST` mirror clone), `aur/src`, `aur/pkg`. All are gitignored;
  deleting them is safe and is the standard cure for "works once, then
  fails".

## Test seams

No automated test covers the packaging flow; verification is running
`makepkg -d` twice in a row from `aur/` (clean, then with the debris of the
first run in place) and checking both succeed and that the work copy's
`git remote get-url origin` points at the local mirror, not GitHub.

## Where it lives

- `aur/PKGBUILD` — `source`, `prepare()` (the `.git` exclusion), `pkgver()`.
- `aur/prepare-aur.sh` — local `updpkgsums` + `.SRCINFO` regeneration.
- `.gitignore` — `/aur/src/`, `/aur/pkg/`, `/aur/katexdown/`,
  `/aur/*.pkg.tar.*` (makepkg debris).
- No CI workflow may `git push` this PKGBUILD to the AUR (see above).
