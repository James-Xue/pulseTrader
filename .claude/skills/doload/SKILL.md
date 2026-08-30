---
name: doload
description: Restore the conversation content of the most recent /dosave archive by reading it and replaying it into the current session as context. Invoked by the user with /doload (optionally /doload <timestamp-or-filename> for a specific archive).
allowed-tools:
  - Glob
  - Read
  - Bash
  - PowerShell
---

# /doload — Restore a Session Archive

Restore the conversation content saved by /dosave back into the current session.

## Steps

1. **Locate the archive.**
   - Default: the newest `session_*.md` in `.claude/session_archive/` under the project root (`E:\Quantification\pulseTrader`), selected by filename timestamp.
   - If the user passed an argument (`/doload 20260830_153000` or a filename), match it against the archive filenames and use that one.
   - If no archive exists, tell the user to run /dosave first and stop.

2. **Read the archive file** in full.

3. **Restore the conversation:** replay the archived content into the chat in chronological order — present every archived turn (user and assistant) as visible message content, so the conversation enters the current context window and work can resume where it left off. Do not silently summarize; replay the actual content.

4. **Give a recap** after replaying:
   - Archive date and message count
   - A 3–5 bullet summary of where the conversation left off (active task, open questions, next steps)
   - A reminder: /doload restores conversation context only — it cannot roll back filesystem or git state. Re-running git operations from the restored context is the way to recover that state.
