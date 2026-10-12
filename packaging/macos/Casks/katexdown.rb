# Homebrew cask TEMPLATE for Katexdown (macOS).
#
# This file is not a published cask. It lives in the repository so a project tap can copy it and
# so its arguments stay in step with packaging/macos/install-macos.sh. It has NOT been exercised
# on a Mac yet: run `brew audit --cask`, `brew install --cask ./Casks/katexdown.rb` and
# `brew uninstall --cask katexdown` before publishing it to a tap.
#
# Why the pieces are the way they are:
#   * depends_on cask: "kate"        - Kate must exist; the installer refuses to create it.
#   * depends_on formula: "qtwebengine" - Method 1 borrows the WebEngine runtime from Homebrew, and
#     letting Homebrew solve that dependency installs qtbase (macro/macdeployqt) too. The installer
#     is then called with --no-brew, so it never runs `brew` as root under `sudo`.
#   * arch: :arm64                   - Method 1 is arm64-only; Homebrew has no Qt bottle for Intel
#     macOS, so an Intel install has no engine to borrow (design/macos-install.md).
#   * installer script:              - runs the same installer a curl|bash user would, with the
#     plugin URL for THIS cask version (the versionless release alias could move under its feet).
#   * uninstall script:              - the same installer in --uninstall mode, which removes exactly
#     the files its manifest recorded and re-signs Kate.
#
# `executable: "/bin/bash"` rather than the script itself: Homebrew's download does not set the
# executable bit, so the script is passed to bash as an argument.

cask "katexdown" do
  version "0.3.8"

  # TODO(tap): replace with the sha256 of install-macos.sh for this version, before publishing in a
  # tap: `curl -fsSL <url> | shasum -a 256`. :no_check keeps the template installable meanwhile.
  sha256 :no_check

  url "https://github.com/q0ppa/katexdown/releases/download/v#{version}/install-macos.sh"
  name "Katexdown"
  desc "Markdown preview plugin for Kate (KaTeX math, GitHub theme)"
  homepage "https://github.com/q0ppa/katexdown"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on macos: ">= :monterey"
  depends_on arch: :arm64
  depends_on cask: "kate"
  depends_on formula: "qtwebengine"

  installer script: {
    executable: "/bin/bash",
    args:         [
      "#{staged_path}/install-macos.sh",
      "--no-brew",
      "--plugin-url",
      "https://github.com/q0ppa/katexdown/releases/download/v#{version}/katexdown-#{version}-macos-arm64.so",
    ],
    sudo:         true,
  }

  uninstall script: {
    executable: "/bin/bash",
    args:         ["#{staged_path}/install-macos.sh", "--uninstall", "--purge"],
    sudo:         true,
  }
end
