# Local Linux execution and MCP completion

Updated 2026-11-07 for the active local Codex chat. The app identifies the chat
as `kind: codex`, `hostId: local`, with the checkout under `/home/dk/code`.
Older notes describing an unavailable Work-chat composer control concerned a
different execution surface and are not instructions for this chat.

## Verified execution boundary

- Ordinary project reads, edits, compiler execution, Python virtual environments,
  Node/npm tests, and disposable Git commits work within authorized roots.
- Ordinary command networking and Unix socket binding remain restricted.
- Automatically reviewed execution has passed a real public GitHub clone,
  same-user Unix socket roundtrips, and local-only TCP roundtrips. The full
  current MCP Python suite passes on that reviewed path.
- The saved global GitHub-only network proxy configuration is not reflected in
  this chat's ordinary command environment. It must not be described as an
  enforced or verified filter. A reviewed request is a separate permission path.

Use the available reviewed-access mechanism for authorized Git downloads,
local GUI/MCP launch, and native tests requiring local IPC. Keep ordinary source
edits and compilation within the project boundary. A future rejected action
requires investigation of that specific rejection; an earlier approval is not
a blanket grant for unrelated operations.

No global full-access mode, firewall change, SELinux change, remote tunnel,
HTTP relay, or new listening TCP service is required for the Kdenlive MCP.
The server uses a same-user local Unix socket and a standard-library stdio
bridge, with peer credentials checked before requests are sent.

## Native application and dependency

The Fedora stock Flatpak does not contain this fork's native MCP endpoint. Use
the separate prefix `build-mcp-linux/install` and preserve the stock app.
The pinned patched MLT dependency is already built locally; all 222 native and
schema/runtime checks passed in the latest verification. See
`mcp-linux-verification.json` for current-source evidence and remaining work.

The complete reproducible workflow is:

```bash
python3 -B dev-tools/mcp-linux.py doctor
python3 -B dev-tools/mcp-linux.py build --jobs 2
```

The helper never installs host packages. It stages its own native effect/export
data before editor tests and installs the executable only after the selected
native/Python suites pass. Build receipts bind effective source content rather
than the transient Git staging state, so publishing an unchanged tested tree
does not invalidate the executable. Progress notes are excluded from the build
identity. Changed application/build inputs still require rebuilding.

## Launch and connect locally

After a verified build/install:

```bash
python3 -B dev-tools/mcp-linux.py launch --isolated
```

This starts a separate interactive editor with disposable settings and a private
socket at `build-mcp-linux/run/kdenlive-mcp-local`. The helper refuses an existing
endpoint rather than deleting another instance's socket. For another instance,
select an explicit unique `--socket-name` before the subcommand.

In another command process, while the editor is running:

```bash
python3 -B dev-tools/mcp-linux.py probe
python3 -B dev-tools/mcp-linux.py config
```

The read-only probe must complete initialize, tool discovery, identity,
capabilities, and native state. Its connection must close before another client
connects; the server accepts one client. The generated client stanza names the
explicit Python bridge and exact private socket. Configuring a client does not
prove that this particular chat has loaded its tool catalog. Verify a real call.

For user work, launch without `--isolated` only after isolated workflows pass.
Opening an existing project is explicit through `--project`; generated fixture
projects are sufficient for initial lifecycle/edit/render tests. A successful
transport probe does not establish full editor coverage. Native edits, history,
persistence, rendered video/audio, cancellation and remaining feature contracts
are completion gates in `mcp.md`.

## GitHub checkpoints

The authorized remote is `DKanishman/kdenlive`; continue the local
`codex/mcp-linux-completion` branch. The connected GitHub account is DKanishman.
Publish incremental, accurately described checkpoints and verify the remote
commit/tree after publication. Use the connector or an existing authenticated
Git path; do not create or expose credentials in the repository. Preserve
concurrent remote changes and all existing local work.

## Completion remains unproven

The objective is full native local MCP control plus a compiled application that
can be launched on demand. A tool count, available UI action, successful build,
or passing narrow test suite is not a replacement for current-source native
behavior and audiovisual/persistence evidence across the requested feature scope.
