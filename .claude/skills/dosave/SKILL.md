---
name: dosave
description: Save ALL conversation content of the current session into the project session archive (timestamped markdown rendering + verbatim raw transcript copy). Invoked by the user with /dosave.
allowed-tools:
  - Glob
  - Read
  - Write
  - Bash
  - PowerShell
---

# /dosave — Archive the Current Session

Save all conversation content of the current session so it can be restored later with /doload.

## Storage layout

Archives live in `.claude/session_archive/` under the project root (`E:\Quantification\pulseTrader`):

- `session_YYYYMMDD_HHMMSS.md` — readable rendering of the conversation (this is what /doload restores)
- `session_YYYYMMDD_HHMMSS.jsonl` — verbatim copy of the raw Claude Code transcript (complete record)

## Steps

1. **Locate the current session transcript.**
   - Claude Code stores one `.jsonl` file per session under `C:\Users\DPIHehao\.claude\projects\<encoded-cwd>\` (e.g. `E--Quantification-pulseTrader` or `C--Users-DPIHehao`).
   - Find the most recently modified `.jsonl` whose parent folder name contains `pulseTrader`; if no match, fall back to the most recently modified `.jsonl` anywhere under `C:\Users\DPIHehao\.claude\projects\`.
   - If multiple candidates are equally recent, pick the largest one (longest conversation).

2. **Render a readable markdown archive.** Compute the timestamp with `Get-Date -Format "yyyyMMdd_HHmmss"`, create `.claude/session_archive/` if missing, then run the extraction below:

```powershell
$src = '<absolute path of the transcript .jsonl>'
$dst = '<absolute path of session_archive>\session_<timestamp>.md'
$out = New-Object System.Collections.Generic.List[string]
$out.Add('# Session Archive')
$out.Add('')
$i = 0
Get-Content -LiteralPath $src | ForEach-Object {
    if ([string]::IsNullOrWhiteSpace($_)) { return }
    $o = $_ | ConvertFrom-Json
    $i++
    if ($o.type -eq 'system') { return }
    $ts = ''
    if ($o.timestamp) { $ts = "  *($($o.timestamp))*" }
    $blocks = $o.message.content
    if ($blocks -is [string]) { $blocks = @(@{ type = 'text'; text = $blocks }) }
    $texts = @()
    foreach ($b in $blocks) {
        if ($b.type -eq 'text') { $texts += $b.text }
        elseif ($b.type -eq 'tool_use') { $texts += "[tool: $($b.name)]" }
        elseif ($b.type -eq 'tool_result') {
            $r = $b.content
            if ($r -is [array]) { $r = ($r | ForEach-Object { $_.text }) -join ' ' }
            $short = [string]$r
            if ($short.Length -gt 200) { $short = $short.Substring(0, 200) + ' ...' }
            if ($short) { $texts += "[tool result: $short]" }
        }
    }
    if ($texts.Count -gt 0) {
        $out.Add("## Turn $i  **$($o.type)**$ts")
        $out.Add('')
        $out.Add(($texts -join "`n`n"))
        $out.Add('')
    }
}
$out | Set-Content -LiteralPath $dst -Encoding UTF8
```

   Rules: keep every user turn and every assistant reply in order; render tool calls as one-line notes; truncate tool results to 200 chars; skip empty turns. The archive must be sufficient to resume the conversation from scratch.

3. **Copy the raw transcript verbatim** to `session_<timestamp>.jsonl` (`Copy-Item -LiteralPath $src -Destination $dst`).

4. **Cleanup:** keep only the 10 newest archives (by filename timestamp) in `.claude/session_archive/`; delete the rest.

5. **Report** to the user: archive path(s), message count, and total size.
