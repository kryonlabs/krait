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
not included yet. Workspace replacement and a replacement preview remain
planned.
