# Local retirement backup — 8 November 2026

The owner requested backing up this Kdenlive MCP project and its dependency lists to GitHub, then removing the local checkout, compiled installation, project-added packages and temporary work artifacts. This archive records the unfinished work; it is not a completed full-MCP release.

## Verified application baseline

The last verified application commit is `c62bb3acdabfa300c8ac2f58bb9e74fde869e98e` on `codex/mcp-linux-completion`. All 15 selected native/Python suites and the installed local MCP multicam/undo/redo, five reopen cycles and H264 video/audio export passed. Full editor coverage remains false.

The existing remote branches `master`, `codex/mcp-prototype` and `codex/mcp-full-coverage-wip` preserve the earlier implementation and design checkpoints. Their local heads matched GitHub before cleanup. The old Windows recovery checkout was clean and had the same already-published heads.

## What this archive adds

- The full unfinished private MLT worktree patch against pinned MLT revision `eca4e32d28e2c588a9c3b7c23e2276d9cd1f4edb`, plus its untracked native guard regression source.
- Historical checkpoint notes and relevant test evidence, including the expected failing baseline for the confirmed luma-map row overread.
- Unique source versions from the two pre-edit checkpoints; source blobs already reachable from the published Git history were omitted from those archives.
- Reproduction/test/launch helpers and exact dependency records.
- Deduplicated historical Windows recovery helper/source files (upstream dependency clones and generated outputs excluded).

The phase-preservation work is unfinished. The luma renderer bounds bug was reproduced but not repaired. Nothing in this archive should be enabled as a completed mixed-camera implementation.

## Restore unfinished MLT source for inspection

1. Clone upstream MLT and check out the exact pinned revision above.
2. Apply `unfinished-mlt-worktree.patch` to that clean checkout. It already contains the seven earlier registered MLT patches; do not apply them a second time.
3. Copy `test_luma_bounds.c` to `src/tests/test_luma_bounds/test_luma_bounds.c`.
4. Read `UNFINISHED-CHECKPOINT.md` for the terminal test results and remaining work before building or installing.

The patch was checked against the pinned dependency index before upload. Archive content hashes are in `backup-manifest.json`. Global Codex configuration, credentials, private keys, compiled caches and generated media are intentionally not included.

Public backup metadata is portable: local home paths, live process/socket identifiers and session state were removed. VERIFICATION.json retains the relevant test outcomes without raw runtime state.
