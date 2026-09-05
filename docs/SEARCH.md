# Workspace search

The search panel supports literal queries, POSIX extended regular expressions,
case matching, and one exclusion glob. Examples: `*.json`, `vendor/*`, or
`tests/*`. Exclusions apply to project-relative paths; matching directories
are skipped. Standard ignored directories are also skipped.

Content search detects text from file contents rather than requiring a known
filename extension. Symlinks are not traversed and files containing NUL bytes
are skipped. Each matching line produces a result; long-line excerpts are
positioned near the match. Click a result to open the file at its matching
line. Scroll over the result list to reach results below the visible area.
Changing a query refreshes results even when its length stays the same.

Quick Open searches filenames only. Content matching and filename matching
are separate so a code occurrence cannot crowd a file out of Quick Open.
Regex and case options also apply to Quick Open.

The current result cap is 64 and the recursive depth limit is eight. The panel
reports its result cap. Search reads saved files; unsaved editor buffers are
not included yet. Paths too long for the 512-byte result path are skipped.

Use **Replace with** and **Preview** to review replacements across the search
scope. An empty replacement deletes matches. Use the arrow buttons to visit
each changed file and the Before/After switch to inspect both versions; scroll
to see more text. Changing the query, replacement, or search options discards
the preview. Regex replacements support `$0` through `$9` for captured groups
and `$$` for a literal dollar sign. Literal replacements insert the text as-is.

**Apply all** checks every proposed file against its saved before-image and
refuses to start if a target has unsaved editor changes, a file has changed,
the project has switched, or an agent is running. Each write checks again and
uses the same descriptor-relative replacement primitive as agent file writes.
Symlink paths and nested repositories are refused. Clean open buffers reload
after applying; unsaved buffers remain intact.

Before writing, Krait saves a JSON recovery record containing the project root
and complete before/after text in `~/.local/state/krait/replacements/`. The
status bar reports the record path. Restore from these records manually;
there is no replacement undo browser yet. If an error or conflict occurs
during writing, Krait stops and reports how many files were applied. The batch
is not an all-or-nothing transaction and does not lock out external writers.

Replacement uses the saved-file search scope and its depth limit. A search
that reaches the 64-result cap must be narrowed before previewing; results
count matching lines, so multiple hits in one file also consume this limit.
Files and replacement outputs are limited to 16 MiB each. The visual preview
shows at most 4096 wrapped lines. Unreadable or ignored search paths are not
included; this is not a whole-filesystem refactoring tool.

Open the command palette with **Ctrl+Shift+P** (Cmd+Shift+P on macOS), or
**Tools → Commands**. Type words from an action's category or label, such as
`file save` or `kaps term`. All query words must match, regardless of case or
order. Up/Down selects a result, Enter runs it, and Escape closes the palette.
Only currently available menu actions appear. Ctrl+P remains filename Quick
Open. Commands share the menu handlers and availability checks; configurable
bindings and extension-provided commands remain planned.
