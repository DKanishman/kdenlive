# Dependencies recorded before local removal

Fedora 45 added 591 RPM packages in these project-setup transactions:

- Transaction 9: `dnf install git cmake ninja-build gcc-c++ extra-cmake-modules pkgconf-pkg-config` (14 packages).
- Transaction 10: `dnf builddep mlt kdenlive` (563 packages).
- Transaction 11: `dnf install lilv-devel pango-devel` (14 packages).

`dependencies-fedora.json` records every exact NEVRA, installation reason and repository. `packages-installed-for-project.txt` is the complete exact-version list. These include Qt Designer, Qt Linguist and OpenCASCADE Draw. The no-action removal preview selected exactly these 591 packages, freeing approximately 3 GiB, with no additional pre-existing packages scheduled for removal. Cleanup must revalidate this before an actual transaction.

`dependency-versions.json` records the pinned private MLT, FFmpeg/x264, Qt/KF versions and registered patch hashes. Private binaries, Ruby validation gems, formatter tools, QML modules and build/test artifacts lived inside the project build folder or its Codex work folders. They were not global Python/Ruby installations.

Private codec correction: only the built libavcodec and libx264 override the host runtime; retain host FFmpeg filter/utility libraries to preserve their full module set. The reproduction script records the original source builds. It must be adapted with that correction before reuse.

Local MCP used a stdio Python bridge plus a same-user Unix socket, never a TCP listener. The startup shortcut and the `kdenlive` MCP configuration entry are installation integration points to remove. Other MCP connections and stock application settings are separate.
