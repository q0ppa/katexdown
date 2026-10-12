# Testing package — reproducible GitHub-branch makepkg install

## Goal / problem

The Vim-style preview reading navigation needs a real Kate session for final
focus-routing verification, but the repository's normal `aur/PKGBUILD` is a
development recipe. It clones a local `git+file://` source and overlays the
working tree, which is useful for local iteration but is not a clean recipe
for a remote package consumer. The testing package must be clearly separate
from that flow and must not be mistaken for an AUR publication.

## Design

`aur/testing/PKGBUILD` is an AUR-style, VCS package recipe named
`katexdown-testing-git`. It fetches only the public
`testing/vim-navigation` branch from the project's GitHub repository, builds
with CMake, and installs the plugin in the same Kate namespace as the stable
package. Its dependency list mirrors the CMake requirements, including
`qt6-webchannel`, which is required by the preview's QWebChannel bridge.

The package provides the `katexdown` virtual name and conflicts with both
`katexdown` and the existing local `katexdown-git` package. This makes a
testing/stable swap explicit and prevents two packages from owning the same
plugin file. The recipe is not copied to or pushed to an AUR repository; the
existing `publish-aur` workflow is restricted to `mommy` and requires
credentials, so publishing this testing branch would be a separate,
deliberate action.

## Invariants

- The testing recipe source names `testing/vim-navigation`, never a moving
  default branch.
- `pkgver()` produces a valid Arch version that changes with each source
  commit.
- `qt6-webchannel` remains a runtime dependency alongside
  `qt6-webengine`.
- The testing and stable package names cannot be co-installed.
- Build output and makepkg debris stay under `aur/testing/` and are not
  committed.
- Installation is user-run with `makepkg`; this project does not install
  system packages or invoke `sudo`.

## Build, rollback, and verification

From a fresh checkout of the published branch:

```bash
git clone --single-branch --branch testing/vim-navigation \
  https://github.com/FilthyS/katexdown.git katexdown-vim-testing
cd katexdown-vim-testing/aur/testing
makepkg --printsrcinfo
makepkg -C -f -si
```

Use `sudo pacman -U /path/to/previous/katexdown-git-*.pkg.tar.zst` to replace
the testing package with a saved stable archive, or
`sudo pacman -R katexdown-testing-git` to remove the testing plugin. The
testing recipe should be rebuilt with `-f` after a branch update; `-C` removes
the previous source/build directory first.

The branch's automated evidence is the final headless CTest result (6/6
passing), with render-feature coverage at 21 passes and 2 skips when KaTeX
runtime assets are unavailable. The remaining reverse preview-to-editor
native-key route requires manual verification in a real Kate window. A
one-off aggregate memory scroll/restore result of `1200 -> 0` was followed by
passing reruns and is not classified as a known failure.

## Where it lives

- `aur/testing/PKGBUILD` — clean GitHub-branch testing recipe.
- `aur/testing/.gitignore` — source, package, and archive debris exclusions.
- `README.md` — user-facing install and rollback commands.
- `.github/workflows/publish-aur.yml` — existing credentialed workflow, not
  used by this testing recipe.
