# Migration provenance

Source repository: `/Users/lrc/graduate/delegation-codebase`

Base commit: `af2445d04e7192cb725578e5daeb4ac44fb244e7`

The migrated backend also includes the source worktree's uncommitted changes to
`allocator-delegation/delegation_backend.cpp`, `DESIGN.md`, `build.sh`, the
jemalloc adapter, and the untracked first-open test/header present on
2026-10-15. The binary diff of all source-repository tracked changes at capture
time had SHA-256:

`7fd518c47c6d5d95749ec51d20e1c2a8c73415f88ec1ca91fdcfc09b4d422fb5`

Scudo files and report directories from that worktree were not migrated.
