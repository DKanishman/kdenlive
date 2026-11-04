# Full editor coverage WIP — paused 2026-11-04

This branch preserves unfinished work separately from `codex/mcp-prototype`. It is not build- or runtime-verified and must not be treated as a completed feature release.

Included prototypes:

- Native short/empty audio-correlation repairs, audio-envelope range/decode/cancellation corrections and timecode-unit/rate/overflow handling.
- A native regression fragment for those primitives, not yet registered in the test runner.
- Initial `mcpprojectextras` tools for project notes/history, exact non-destructive unused-bin cleanup, slideshow/title-template intake and asynchronous source status. The standalone module passed a syntax check before this snapshot; it is not yet in CMake or the tool dispatcher.

Outstanding integration includes checked alignment jobs/proposals/atomic apply, generated-media workflow tests, multicam and grouped audio time maps, complete rich template validation, and OTIO/library/archive services. No audio/video recording or optional model installation has been performed.

The intended contracts and remaining prerequisites are recorded alongside this document. Native changes here need independent review and real native/GUI execution. Work stopped at the user's request; resume only when requested.
