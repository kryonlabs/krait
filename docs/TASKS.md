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

Passing validation places a manual run in Review; it does not mark a Kanban
card Done. Results describe the checks at execution time and are not yet
bound to a source snapshot. Re-run checks after changing code. Source-version
validation and explicit task acceptance remain separate roadmap work.
