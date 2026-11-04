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
