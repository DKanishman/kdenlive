# Native MCP on Linux

The native MCP fork must be compiled. Installing the stock Kdenlive Flatpak or
running the Python bridge alone does not add an MCP server to that editor.
The helper uses a separate prefix under `build-mcp-linux/install`, preserves
the stock installation and never installs system packages itself.

## Fedora prerequisites

Run in your ordinary terminal with network access and permission to install packages:

```bash
sudo dnf install git cmake ninja-build gcc-c++ extra-cmake-modules pkgconf-pkg-config
sudo dnf builddep mlt kdenlive
```

`dnf builddep` requires the distribution's builddep support and source package
metadata. It installs the distribution package's dependencies; CMake remains
the authority for this newer fork's requirements: Qt 6.10+, KF 6.21+,
KDDockWidgets 2.4+, OpenTimelineIO and the multimedia development libraries.
See [upstream build instructions](build.md) and
[KDE's Fedora dependency guidance](https://develop.kde.org/docs/getting-started/building/help-dependencies/).

## Build and test

From the repository root:

```bash
python3 -B dev-tools/mcp-linux.py doctor
python3 -B dev-tools/mcp-linux.py build --jobs 2
```

The build command clones MLT at the exact revision in
[the dependency patch instructions](../packaging/patches/README.md), checks and
applies all seven patches, builds/installs it locally, checks essential runtime
modules and runs its native CTest suite. It then configures/builds this editor,
runs MCP, subtitle, group, sequence, marker, OTIO, FFT and Python suites, and
installs the editor only after those checks pass. Its own effect/export preset
data is staged in the local prefix before tests; a stock Flatpak's private data
is not used. Tests use isolated settings,
software Qt rendering and dummy SDL audio. Successful execution records Git
revision, pending source content, dependency changes and patch hashes in
`build-manifest.json`. Effective repository source content is hashed independently
of Git staging/commit state, so publishing unchanged tested sources does not make
the installed app stale. Progress reports under `dev-docs` and the root README
are excluded from that identity. A failed command is a build failure, not a successful
installation. Network, a compiler and Unix socket permissions are required.

The helper defaults to two compiler jobs to limit memory use. A custom location
must precede the subcommand, for example `--root /short/path build --jobs 2`.
Keep that same `--root` for launch/config/probe. Existing dependency checkouts
are never reset, and patches that cannot apply or reverse-check are errors.

## Launch and connect

```bash
python3 -B dev-tools/mcp-linux.py launch
```

For disposable settings/data/cache, add `--isolated`; to open an existing local
project, add `--project /absolute/path/project.kdenlive`. Launch refuses an absent
or stale build receipt. Its editor and MLT runtime come from the same prefix.
It creates a same-user directory with mode 0700 and sets the editor's `TMPDIR`
so Qt and the connector agree on the exact socket path. Existing endpoints are
never deleted automatically. Use `--socket-name another-unique-name` before the
subcommand for another editor instance. Socket paths must fit Linux AF_UNIX limits.

In a second terminal, while the editor is running:

```bash
python3 -B dev-tools/mcp-linux.py probe
python3 -B dev-tools/mcp-linux.py config
```

`probe` performs initialize, initialized notification, tools/list, get_info,
get_capabilities and get_state. It checks native replies without editing a
project. Close the probe before connecting another client; the native endpoint
accepts one client. `config` prints a standard `mcpServers` stanza for a stdio
client, with an explicit interpreter, bridge and socket path. Import that stanza
into your MCP client's settings; no client configuration is edited automatically.
The bridge checks Linux SO_PEERCRED before sending a request. Native startup
logs and `get_info.endpoint` expose Qt's actual endpoint; `get_info.buildVersion`
also includes the configured build revision.

## Current verification and remaining work

Branch: `codex/mcp-linux-completion`. The editor and native tests compile against private patched MLT 7.43.0. All 222
MLT tests and all eleven selected editor/connector suites pass. The native MCP
suite passes all 164 cases, including the unchanged roto pixel assertions after
the cJSON isolation repair. The full Python suite passes 40 cases with one
Windows-only skip. A repeat with source held unchanged passed and installed the
editor with a matching build receipt.

The installed app launched visibly on this laptop through an on-demand local
connector. Its executable hash and loaded private libraries were checked through
`/proc`. Real MCP initialize, 122-tool discovery, capabilities, project state and
native UI inspection pass. Exact timeline edits, undo/redo, notes, normal Save As,
reopen and real H.264 export pass; decoded frames are red/blue as requested.
The app was returned to a blank project after preserving its test fixture.
Full editor coverage remains false; tool count is not coverage evidence.

The personal on-demand stdio wrapper is stored outside the source under
`/home/dk/.codex/work/kdenlive-mcp-launch/connect.py`. It launches only the verified
build, serializes launch attempts using a private local lock, never deletes an
existing endpoint, and hands requests to the existing bounded same-user bridge
without consuming/replaying them. A desktop entry named **Kdenlive MCP (local
build)** and Codex MCP server `kdenlive` are configured. The user's previous Codex
configuration bytes were preserved, with a private backup. Current-chat tool
catalog refresh is not yet verified; the local connector itself has been tested.
The wrapper uses separate native settings and only local stdio/Unix transport.

### This laptop's private runtime prerequisites

The host has Qt/KF development packages but lacked `org.kde.desktop` and Qt's WebP
image plugin. Signed x86_64 Fedora `kf6-qqc2-desktop-style` 6.31.0-1 and
`qt6-qtimageformats` 6.11.2-1 packages were verified and unpacked under
`build-mcp-linux/qt-runtime`. `runtime_env` prepends their QML/plugin directories.
No system installation was changed. A normal system installation of those
runtime packages is also sufficient.

Fedora's ffmpeg-free lacks libx264. For the existing positive H.264/transcode tests,
a private libavcodec was built from FFmpeg n9.0.2 commit
`946fcce07b6dcd0331c8cc609192aeff5e1924f8` with x264 stable commit
`31e19f92f00c7003fa115047ce50978bc98c3a0d`. Both sources came from their GitHub
repositories. The build enables GPL/shared/libx264 and disables x86 assembly
because no assembler is installed. This is a local development runtime, not a
portable distribution package. Only libavcodec and libx264 override the host
libraries; system FFmpeg tools and the matching system avformat/avfilter/avutil
libraries remain in use, preserving ASS/subtitle filters. Other optional external
encoders still require their own dependencies; query native preset availability.
Do not install an entire minimal FFmpeg stack over the full system filter runtime.

First-run setup is native UI. The isolated QA profile selected installed `dv_pal`,
private melt/render paths and SDL2 dummy audio; it did not alter user projects or
stock Flatpak settings. Runtime/socket tests use approved execution because this
chat's ordinary commands still cannot bind local sockets or resolve GitHub.
Git reads and real source downloads succeed through approved execution. This is
not unrestricted access, and new project dependencies/servers need their own checks.

### Work still required

The matched installation, live core workflows and on-demand local setup pass.
Full feature-contract completion and wider audiovisual/persistence verification
remain required. Alignment, multicam, source creation, checked interchange,
library/archive, analysis/masks/AI, persistent queue and other pending contracts
are tracked in the project progress notes. Full editor coverage remains false.
