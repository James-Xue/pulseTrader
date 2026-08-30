---
name: summarize-diff
description: Produce a structured summary of uncommitted changes in the git working copy — changed files, what each file changed, overall theme, and a suggested commit message. Invoked by the user with /summarize-diff.
allowed-tools:
  - Glob
  - Read
  - Grep
  - Bash
  - PowerShell
---

# /summarize-diff — Summarize Uncommitted Git Changes

Summarize the current git working copy state: which files changed, what changed in each file, the overall theme, and a suggested commit.

## Steps

Run git commands from the project root `E:\Quantification\pulseTrader`.

1. **Inventory changes.**
   - `git status --porcelain` — changed, staged, and untracked files. Ignored files (build/, data/, logs/, .env, .claude/, ...) never appear here; no need to filter them.
   - `git diff --stat` and `git diff --cached --stat` — insertion/deletion counts for unstaged and staged changes.

2. **Read the diffs.**
   - `git diff` (unstaged) and `git diff --cached` (staged); combine both if the working copy has both.
   - For untracked files, `git diff --no-index -- /dev/null <file>` or read the file directly to describe its content.
   - For binary files, rely on `--stat` plus the file path/extension; do not attempt a content diff.

3. **Write a per-file summary.** For every changed file, describe the changes hunk by hunk: what was added/removed/modified and why (infer intent from names, comments, and surrounding code). Keep each file summary to 1–5 bullet points.

4. **Identify the overall theme** — one sentence, e.g. "new feature", "bug fix", "refactor", "docs", "test infrastructure", or a mix (name the dominant one).

5. **Suggest a commit** following the Chinese commit-description template from AGENTS.md §11 (提交说明格式). Fill in only the `xxx` placeholders; keep field names, order, and defaults unchanged (per AGENTS.md §11, do not add fields like `Co-Authored-By`). For 代码评审人/业务评审人, use the name of the model driving this session (e.g. deepseek-v4-flash).

   ```
   TD: 暂无（xxx）
   问题原因：xxx。
   解决方案：xxx。
   影响范围：xxx。
   代码评审人：xxx
   业务评审人：xxx
   代码提交版本：主干
   ```

   **Do NOT commit or push** — this skill only suggests. Pushing is manual-only (user rule, 2026-08-30): never run `git push`; the user pushes manually.

## Output format

```markdown
## Changed files (N)
| Path | Status | +/− | Summary |
|------|--------|-----|---------|

## Per-file details
### <path>
- ...

## Overall theme
<one sentence>

## Suggested commit
TD: 暂无（xxx）
问题原因：xxx。
解决方案：xxx。
影响范围：xxx。
代码评审人：xxx
业务评审人：xxx
代码提交版本：主干
```

If the working copy is clean (`git status` shows nothing to commit), say so and stop.
