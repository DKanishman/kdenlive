# Native MCP progress checkpoint — 2026-11-04

Work is paused at the user's request. Full editor-wide coverage remains the target and is not complete. This checkpoint is not a release certification.

## Integrated work

- Recovered the native MCP 0.4 extension and integrated whole-project semantic state, bounded state pages, immutable oversized-result receipts and edit preflight.
- Integrated native ASS metadata/event transactions and whole-project marker-category transactions. Fixed strict subtitle style, timestamp and normalized UTF-8 size checks before model creation.
- Hardened rotoscoping data: native point/handle validation, finite request bounds, chronological frame keys, and explicit rejection of unsupported non-linear easing.
- Fixed Windows connector deadline rounding using an integer monotonic clock; registered its portable suite with CTest.
- Preserved reproducible MLT patches for bulk playlist inspection, SDL device lifetime, OpenFX registration and the Windows relative-XML-path crash.

## Executed verification and pending checks

- The isolated dependency build passed all five native suites. The playlist API passed 20 exact-result comparisons through 8,192 entries. The XML regression reproduces the original relative-path failure and passes with the repair. SDL ownership/restart/timeout tests passed.
- Live editor checks passed for catalogs, profile changes, timeline edits, effects, normal save/Save As, reopen, rendered pixels/audio, sequences, dialogs, playback-token stability and unsaved notes. These were run before the last source hardening; they must be repeated against the next checkpoint build.
- The latest complete eight-suite run had six passing suites. MCP reported 152/155 cases passing; three failures were fixture assumptions (QtQuick menu setup, marker reply budgeting, and subtitle work-file isolation). The connector also exposed a deadline-rounding defect. These were corrected afterward.
- Connector regression after the correction: 25 cases, one Unix-only platform skip, passing on both Python 3.12 and 3.14.
- Build 32 completed after the first rotoscoping validation integration. The final non-linear-easing refusal was edited afterward and still needs an incremental build. The corrected native suite and final live ASS workflow have not been rerun. Repository-required C++ formatting was applied while saving this checkpoint; no additional feature checks were run after the pause request.
- The final editor build imports the new bulk playlist API. Original installed Kdenlive runtime files were preserved; dependency validation used a separate prefix.

## Git branches

- `codex/mcp-prototype`: integrated source, tests, dependency patches and this checkpoint.
- `codex/mcp-full-coverage-wip`: additional unverified native alignment/timecode repairs, initial project-extras module and staged native test fragment. This branch is intentionally not integrated into the tool catalog/CMake yet.

## Resume order

1. Incrementally rebuild the latest integrated source; run the eight relevant CTest suites with the expanded timeout, then the live all/state/ASS/language/monitor workflows. Preserve exact source and loaded-module provenance.
2. Review and integrate the separate WIP branch. Finish alignment job/proposal/atomic-apply contracts, multicam and grouped audio time maps. Complete project notes, cleanup and source-creation tests before the checked OTIO/library/archive workflows.
3. Add truthful scene-analysis/stabilization/custom-job results and cancellation, full mask editing/registry persistence, and repair the native rotoscoping rasterizer's fixed intersection buffer/subdivision limits.
4. Complete the remaining export/persistent-queue, title/effect, configuration and interactive feature matrix. Persist render plans through native snapshots rather than a second timeline model.
5. Verify optional prerequisites and actual results for native speech/AI masks, external Glaxnimate and online resources. Native video capture is disabled upstream; current capture coverage concerns audio voiceover. No model downloads, provider authentication or recording were performed.

The initial project-extras prototype and native alignment patches remain unverified. Do not replace the integrated source with old staging snapshots. Local build logs, test media, helpers and detailed task checkpoints remain in the Codex task workspace, outside this source tree.


## Linux resume session - 2026-11-07 (stopped by user)

Networking and real Unix sockets now pass in the agent environment. The last
executed full Python suite ran 38 tests: 37 passed, one Windows-only skip. Later
build-helper/native edits were not retested after the stop request.

Pinned MLT source downloaded and the original four patches applied. The user
installed lilv-devel and pango-devel. MLT configured successfully with a local
Kwalify 0.7.2 compatibility launcher. Compilation was interrupted at the user
request (exit 130, last observed step 358/565). Kdenlive was not built/installed;
no native suites or live editor checks completed in this session.

Preserved unfinished additions: revised WIP audio-correlation/envelope/timecode
primitives and mcptest fragment; notes/non-destructive cleanup module and test
source (not registered); fifth MLT rotoscoping-bounds patch/native regression
(not registered in helper). Its development changes also remain in the ignored
MLT checkout. Review and register unfinished pieces before the next complete
build; do not treat discovery or these source edits as full MCP coverage.

The original dirty files were backed up before new edits in
/home/dk/.codex/work/kdenlive-mcp-linux-completion/initial-changes.tar. The current
branch and all existing changes remain in place. See mcp-linux-verification.json
for the current evidence and resume order.


## Current local-tools resume — 2026-11-07

Local source reads/writes and command execution work. The configured project
write root does not grant Unix IPC: the real connector suite currently has 36
passing cases, one Windows-only skip and one EPERM failure when creating its
AF_UNIX listener. Native endpoint/live-editor control is still unverified.

The patched MLT dependency now passes all 222 checks (21 native plus 201
schema/runtime-query checks). Project-extras registration, the rotoscoping patch
and the metadata patch are present in the working tree. The resumed incremental
editor build is running with one compiler job; audio alignment sources compile.
This entry is a progress checkpoint, not build or full-coverage certification.
Current evidence and remaining work are in `mcp-linux-verification.json`.


## Active local MCP completion — 2026-11-07

The user resumed the full goal: native local-only MCP control, a compiled
application launchable on demand, and ongoing synchronization to the
DKanishman/kdenlive GitHub fork. This work is active.

Verified ordinary project file edits and C++/Python/Node build workflows.
Reviewed execution passes actual GitHub cloning, Unix and loopback socket
roundtrips, and the full updated Python suite (40 passed, one Windows-only skip).
The patched MLT dependency passes all 222 native/schema/runtime checks. Ordinary
command networking/IPC remains restricted; the saved GitHub-only proxy has not
been verified as active in this chat.

The native editor build continues. Corrected QJsonObject iterator compatibility
in custom profile tools and an export test helper. Launch provenance now hashes
effective repository content, survives staging/committing unchanged source, and
excludes development progress notes and generated Python caches. Its regression
checks pass. Native effect/export data is staged in the isolated prefix before
editor tests, without depending on the stock Flatpak's private resources.

A confirmed native mask-removal defect is repaired in source: unknown names
are no-ops, exact named entries are removed even when files are shared, saved
registry data is refreshed, and observers see the persisted state. The new
native regression covers source-file preservation and reload of the saved
registry. Compilation/execution of that regression is still pending.

No successful native editor install or live local MCP session is claimed yet.
The wider alignment, source-creation, interchange/library/archive, analysis,
mask/AI/speech, persistent render queue and remaining feature contracts in
mcp.md and the preserved WIP design inventory remain completion gates.

## Native connection and Linux runtime verification — 7 November 2026

The local test editor exposed a real Unix endpoint and answered initialize,
122-tool discovery, capabilities and state. Disposable live edits, exact color
preview pixels, undo/redo, notes, normal Save As and reopen passed. This was an
earlier development binary; final installed-binary verification remains required.

The editor and all selected native test binaries compile. Mask removal's native
regression passes. Two nested-menu cases pass after preserving the popup window
flags when reparenting a warmed submenu. Rendering now checks actual preset
codec/format errors and rejects missing requested streams; real video-only and
audio-only fixtures prevent accepting duration alone as proof of export.

Private signed Qt runtime packages resolve the missing desktop style/WebP plugin.
A pinned private libavcodec/x264 build enables H.264, retaining system format,
filter and utility libraries so ASS subtitles remain available. No system codec
or stock Flatpak package was replaced. GitHub source downloads and native local
IPC work through approved execution; ordinary GitHub DNS remains restricted.

The latest complete MCP suite reached 163/164 cases. Roto alpha checks exposed
legacy cJSON symbols in PlusGPL binding to modern host libcjson. Seventh patch
`mlt-private-cjson.patch` hides only that bundled implementation and preserves
`mlt_register`. Its rebuilt module exports no cJSON symbols. A complete MLT and
eleven-suite editor verification/install job is now running. No successful editor
install receipt has been issued yet; full editor coverage is still false.

### Tested installed checkpoint

All 222 MLT checks and eleven selected editor/connector suites pass. The complete
MCP suite passes 164 native cases after cJSON isolation, with the original roto
pixel assertions intact. An unchanged-source repeat passed and installed the
editor with executable SHA256
`233833609d69826a2726f1b243e5a3e40cc05b377e37d6e0576985fe2ec7d0a6`.

The installed app launched visibly through the private on-demand connector.
Its `/proc` executable/library provenance, initialize/tool/state/UI reads, native
timeline edits, undo/redo, notes, normal Save As/reopen and actual H.264 export
passed. Decoded red/blue pixels were [252,0,0] and [0,0,253]. The app was returned
to a new blank project. The desktop launcher and local-only Codex MCP entry are
configured, preserving prior settings. Current-chat catalog refresh has not been
verified. Full editor coverage and the broader contracts above remain unfinished.
