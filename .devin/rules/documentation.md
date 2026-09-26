---
description: "Documentation policy — AGENTS.md is read-only"
trigger: always_on
---

Do NOT create, append to, or edit `AGENTS.md`. It is a hand-curated
minimal file; session findings, feature documentation, debugging notes
and gotchas must go in standalone docs:

- `doc/features/<feature>.md` for feature docs (update its README index)
- `doc/engineering/<topic>.md` for implementation notes/gotchas
- workspace `dev-tools/SESSION_LOG.md` for per-session findings
