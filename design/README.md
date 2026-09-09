# Design docs

Design decisions in this codebase live in `design/` so that future work — by
humans or agents — can find *why* something works the way it does without
reverse-engineering it. Code answers *what*; these docs answer *why*, which
invariants a change must not break, and which pitfalls already cost us.

## Rules

- **One doc per major feature / design / pitfall.** A doc is focused: it covers
  one topic end to end, not everything in the file it happens to touch.
- **Write the doc when the design settles** (a feature lands, a design
  changes, or a bug turns out to be a pitfall worth recording). Do not wait for
  a cleanup pass — a fix that encodes an invariant is the best moment to write
  the pitfall down.
- **Update the doc when the design changes.** If a change alters an invariant,
  the doc is part of the change. Stale docs are worse than none; fix them in
  the same commit as the code.
- **Record invariants and pitfalls, not code.** No line-by-line API listing
  (it drifts). Structure roughly:

  ```markdown
  # <topic> — <one-line summary>

  ## Goal / problem
  ## Design (the mechanism, and why this shape)
  ## Invariants (rules a change must not break)
  ## Pitfalls (bugs/decisions that already cost time — and why they happened)
  ## Test seams (env hooks, and which test pins which invariant)
  ## Where it lives (files, in one or two lines)
  ```

- **AGENTS.md at the repo root** is the standing reminder for agent work; the
  PR template and CONTRIBUTING.md carry the same reminder for review time.

## Index

| Doc | Topic |
|-----|-------|
| [lazyrender.md](lazyrender.md) | Preview loading & renderer memory: page lifetime (freeze/discard), per-document engine gating, image decode modes and the parking machinery, plus the renderer-memory maintenance (recycle) that bounds a long-session renderer |
| [fragments.md](fragments.md) | Fragment links: heading anchor ids (GitHub slug rules, every level), and how a clicked `doc.md#section` link makes the preview jump to that section |
| [print.md](print.md) | Export & print: interactive chrome (the floating outline control) is hidden by `@media print` in `base.css`, which the exported standalone .html embeds, so printed output is clean document content |
| [aur-build.md](aur-build.md) | The `aur/PKGBUILD` build flow: the `git+file://` source plus the `prepare()` working-tree overlay, why the overlay must never copy `.git`, and what debris a `makepkg` run leaves behind |
| [windows-install.md](windows-install.md) | Windows distribution: the WebEngine payload must sit next to `kate.exe` (import resolution), so the installer is a self-elevating NSIS exe; the Qt-minor guard, the single-authority file list, and how the CI verifies install/render/uninstall against a real Kate |
