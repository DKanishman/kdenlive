# Unfinished transition work — portable recovery checkpoint

The owner retired the local installation on 8 November 2026 after requesting a GitHub backup. This document preserves the unfinished engineering state; it is not authorization to resume development or reinstall software.

## Published baseline

The last verified application commit is c62bb3acdabfa300c8ac2f58bb9e74fde869e98e on codex/mcp-linux-completion. Full editor coverage remains false. See VERIFICATION.json and dependency-versions.json for the portable results and build identities.

## Unfinished source

The private MLT dependency was pinned to eca4e32d28e2c588a9c3b7c23e2276d9cd1f4edb. The archived full worktree patch includes the previous seven registered patches and these unfinished additions:

- src/framework/mlt_transition.c: optional kdenlive:mix_progress_offset and kdenlive:mix_progress_length preserve the original phase of a retained transition. Offsets are relative, so moving a fragment preserves its blend. Strict integer parsing and bounds checks reject malformed, partial and overflowing metadata; absent or invalid metadata uses the existing behavior.
- src/tests/test_service/test_service.cpp: new phase and rendered-output tests, including repeated cropping, movement, progressive/interlaced wipes and native XML round trips. Initial direct phase/render checks passed. The expanded suite still fails on interlaced wipe output because of the separate bounds defect below.
- src/tests/CMakeLists.txt: a UNIX-only Native:luma_bounds regression target. Core is always built; MOD_CORE is not a valid guard for this target.
- test_luma_bounds.c: copy this untracked test to src/tests/test_luma_bounds/test_luma_bounds.c when restoring. It puts the logical luma-map boundary against an unreadable guard page and covers YUV422, RGBA and RGBA64 with small, odd and scaled map/image heights and both interlaced field orders.

## Confirmed defect, not repaired

All three rasterizers in src/modules/core/transition_luma.c initialize y_offset from the field and then multiply the derived row by field_count. With interlaced output, the second field starts one source row too far ahead and can read a row beyond the luma map. Small maps are also affected. Native guarded-memory testing reproduced SIGSEGV. This was an owned regression process; the installed editor was not replaced by the experiment.

A proposed repair still needs implementation and verification: reset per-field y_offset to zero, address row (y_offset >> 16) * field_count + field, and clamp to the last luma row for tiny/odd/scaled maps in all three rasterizers. Prove correct field parity and exact decoded output; do not weaken the guard test or pixel checks.

## Resumption requirements

Restore a clean pinned MLT checkout, apply unfinished-mlt-worktree.patch once, and copy the untracked test source as described above. Do not apply the earlier seven patches again. Test the private framework by placing its build output library directory before the installed prefix in LD_LIBRARY_PATH. The pinned Ruby validation utility also needs its private GEM_PATH.

The phase helper alone does not complete multicam cuts through mixes. Native editing must preserve both participants and their exact source ranges, crop parameter-driven transitions faithfully, preserve phase metadata through native mix recreation/undo/reload, and prove unchanged retained frames and audio. The current guarded multicam operation still rolls unsupported edits back. Wider full-editor requirements in dev-docs/mcp.md also remain unfinished.

Machine-specific home paths, temporary filenames, live process/socket identities and session tokens have been removed from this public checkpoint. Archived diagnostic helpers are portable/redacted records; review and adapt paths before reuse.
