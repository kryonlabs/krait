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
