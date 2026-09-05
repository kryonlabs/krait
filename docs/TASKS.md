# Project validation

Create `.krait/tasks.json` in the project root:

```json
{
  "tasks": [
    {"name": "build", "command": "make", "timeout_seconds": 300},
    {"name": "tests", "command": "make test", "timeout_seconds": 600}
  ]
}
```

The Agent view's **Validate** button runs these checks without requiring an
AI provider. Agents can invoke the `validate` tool through the same tool
approval controls as shell commands. Commands run in the project root with
the user's environment. Review project commands before running them.

Configure 1–32 checks. Each requires a name and command; the timeout defaults
to 60 seconds and supports up to 3600 seconds. Checks run sequentially.
Failure does not prevent subsequent checks from reporting results; Stop ends
execution and terminates an active command and its process group.

Results appear in the conversation as an expandable validation tool result.
The latest structured report is stored beside that session's `history.jsonl`
as `history.jsonl.validation.json`. It includes the project and task identity,
start time, command, exit code, duration, and up to 8191 bytes of combined
output for each check. Exit 124 indicates timeout; 130 indicates cancellation.
A malformed configuration or interrupted check cannot leave the previous
report marked as a successful current run.

Passing validation places a manual run in Review. For a project-bound Kanban
card, use that card's agent session to run validation, then click **Accept**
in Review to move it to Done. Krait refuses the move if results are missing,
failed, from another task, or stale. Cards without a project remain manually
managed. Direct editing/import of board files is not an acceptance mechanism.

Reports include a SHA-256 source snapshot. Validation checks the tree both
before and after execution; checks that change source files do not pass.
Acceptance recomputes the snapshot so later edits invalidate the result.
Names, file modes, regular-file contents and symlink destinations contribute
to it. The following directory names are excluded at every level: `.git`,
`build`, `out`, `node_modules`, `.cache`. Keep source inputs outside these
output/dependency directories. Symlink targets' contents and external
services/dependencies are not covered; rerun validation when they change.
`.krait/tasks.json` itself is included, so changing the checks invalidates
older results. Source-based acceptance does not prove arbitrary product
requirements: review the task's acceptance criteria before clicking Accept.

## Cards and acceptance criteria

Open a card to edit its Task, Criteria, Labels, or Depends on fields. Choose
Low, Normal, High, or Urgent priority. Dependencies are card IDs separated
by whitespace or commas; IDs are displayed in the editor. Unknown IDs,
self-dependencies and dependencies outside Done prevent acceptance. A
cycle therefore cannot be accepted until its dependency list is corrected.

Metadata is stored as a JSON `meta:` header in each card's text file. Existing
cards without metadata continue to load. Labels are free text; dependencies
are interpreted as IDs. Criteria are multiline review requirements, supplied
to the agent when handing off a card. Accept is the user's confirmation that
those requirements have been reviewed, not an automated proof of them.

Validation fingerprints the complete card file, including requirements and
metadata. Editing it after validation invalidates the result even if source
files stay unchanged. New validation reports are required after upgrading
from reports that did not record task specifications.

Each successful move into Done records an acceptance event beneath
`~/.kryon/krait/kanban/.acceptance/<card-id>/`. Events preserve the title,
criteria, task/source fingerprints and acceptance timestamp. They remain
available after card deletion; a history browser is still planned. Board
imports and direct file edits do not yet enforce this workflow.

## Reviewing agent file changes

Click **Changes** in the Agent view to inspect the latest write batch.
Use the arrows to select a file and scroll its Before/After contents.
**Accept file** keeps that file's changes; **Revert file** restores its
previous contents (or removes a newly created file). Both actions refuse
files that have subsequently changed. The batch Revert action restores only
pending files, preserving accepted files. Decisions survive session switching
and restart alongside the recorded before/after contents.

File acceptance is separate from accepting a Kanban task: it confirms that
file's edits, while task acceptance also requires current validation and
completed dependencies. Unsaved editor buffers are preserved on reload.
The preview currently displays up to 4096 wrapped lines per version and
indicates that limit. Open larger files in the editor for complete review.
Only the latest batch is retained; checkpoint-history browsing remains on
the roadmap.
