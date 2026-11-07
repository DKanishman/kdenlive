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
applies all six patches, builds/installs it locally, checks essential runtime
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

The current source is on `codex/mcp-linux-completion`. The patched MLT 7.43.0
checkout and separate local installation exist. All six dependency patches are
registered in the helper. Current verification passes all 21 native MLT tests
and all 201 source-schema/runtime-query tests. Logs are in
`build-mcp-linux/verification-logs/mlt-native-current.log` and
`mlt-schema-current.log`.

Kdenlive has configured successfully against that local MLT prefix. Its
incremental editor build is running; the modified audio correlation and envelope
sources compile. Project notes/non-destructive cleanup and their native tests
are registered, but their execution and the complete editor build are not yet
verified. There is no successful build receipt or verified native editor install.

The current Python suite runs 38 tests: 36 pass, one Windows-only case is skipped,
and the real Linux AF_UNIX roundtrip fails at socket creation with EPERM. This is
an active sandbox restriction. Writable project files and working child-process
stdio pipes do not establish permission to create or connect native Unix sockets.
No test suppresses or skips that Linux restriction. The editor's current MCP
endpoint requires local Unix IPC; no TCP listener or remote relay is used.

Full editor coverage remains incomplete. The alignment/multicam/time-map
contracts, source creation, archive/library/OTIO workflows, specialized analysis
and masks, and persistent render queue still require implementation and native
behavior/persistence verification. The WIP branch has not been merged wholesale.

The original and resumed changes are preserved. See
[mcp-progress.md](mcp-progress.md) for the historical checkpoints and
[mcp-linux-verification.json](mcp-linux-verification.json) for current evidence.
