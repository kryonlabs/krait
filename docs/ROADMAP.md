# Krait development roadmap

The target is a complete workflow for coding, agent tasks, and game authoring.
Existing modes are foundations; their presence does not imply parity with
VS Code, ZCode, or Godot. Complete each milestone with working UI and realistic
acceptance checks before marking it done.

## 1. Reliable tasks and changes

- [x] Generate card IDs independently of columns; refuse destructive moves.
- [x] Save long card descriptions without fixed-buffer truncation.
- [x] Bind cards to distinct persisted agent conversations and reopen them.
- [x] Persist the latest write batch's before/after images for recovery.
- [x] Refuse revert when files contain subsequent edits; remove newly created
  files on revert and retain recovery after session switching.
- [x] Preserve unsaved editor buffers when agent writes trigger reload.
- [x] Include user and project AGENTS.md instructions in model context.
- [x] Persist run state and offer Resume after interrupted sessions.
- [x] Cancel pending approval/network waits and stop between tool calls.
- [x] Terminate active run-tool commands and their process groups on cancellation
  or timeout, with forced cleanup for commands that ignore termination.
- [x] Source-bound validation and explicit Review-to-Done acceptance for
  project-bound cards through Krait's move API.
- [x] Editable acceptance criteria and task-spec validation.
- [x] Persist acceptance events with task/source fingerprints and criteria.
- [ ] Acceptance-history browser and dependency-aware scheduling.
- [x] Before/after review UI and persisted per-file acceptance/revert.
- [x] Archive previous write batches and browse their before/after history.
- [ ] Historical rollback, retention controls, and large-file review without preview limits.
- [x] Detect stale writes against full file-tool read versions in the active session.
- [x] Refuse file writes and reverts against the UI snapshot of unsaved files.
- [x] Persist task-scoped read fingerprints across session switching and restart.
- [ ] Coordinate editor mutations atomically with writes and normalize path aliases.
- [ ] Make multi-file changes transactional; handle external writers and
  filesystem errors without losing the previous recovery checkpoint.
- [x] Descriptor-relative read/write/revert with symlink refusal and nested-repository write protection.
- [ ] Apply workspace boundaries consistently to search, screenshots and proposal application.
- [x] Per-project tool-round, batch-action and provider-request timeout limits.
- [ ] Context summarization, token/cost budgets and total run deadlines.

Direct recovery covers the latest file-tool write batch; earlier batches remain browsable in checkpoint history. It does not
undo shell commands or provide filesystem isolation. Resume starts a new
model turn that inspects current state instead of replaying an interrupted
command. Cancellation terminates run-tool commands; graphics and compile-gate
operations still finish their current operation before stopping. Instruction lookup currently reads ~/.kryon/krait/AGENTS.md and
the bound project's AGENTS.md; nested directory rules remain to be implemented.

## 2. Daily coding

- [ ] LSP transport and lifecycle; diagnostics, completion, navigation,
  references, rename, formatting and code actions in the editor.
- [x] Workspace search with regex, case matching, exclusion glob, scrollable
  results and navigation to matching lines; filename-only Quick Open.
- [x] Bounded workspace replacement with before/after preview, exclusions,
  stale-file and unsaved-buffer checks, and persistent recovery records.
- [ ] Transactional workspace replacement, recovery UI and larger search scope.
- [x] Searchable command palette for available menu actions with keyboard selection.
- [ ] Multi-cursor editing, configurable shortcuts and extensible command registration.
- [ ] DAP launch/attach, breakpoints, stepping, variables and call stacks.
- [x] Configurable project validation commands, manual Validate, and persisted
  per-check command/exit/duration/output reports.
- [x] Source snapshot evidence and refusal to accept stale validation.
- [x] Saved validation-command results view with exit codes, timings, output
  and source/task freshness checks on refresh.
- [x] Archive and browse validation reports without changing active acceptance evidence.
- [ ] Individual test-case explorer, report retention, and completion gates across
  external synchronization.
- [ ] Diff/merge review and remote workspaces.

Acceptance: edit, navigate, refactor, test and debug a real non-Kryon project
without leaving Krait, alongside the existing Kryon workflow.

## 3. Agentic Kanban

- [x] Editable priorities, labels, dependencies and acceptance criteria;
  unfinished or unknown dependencies block acceptance.
- [ ] Configurable columns, scheduling and concurrency controls.
- [ ] Per-task workspace isolation and conflict-aware integration.
- [ ] Validation evidence and automatic transitions into Review.
- [ ] Durable task/session/run/change/validation relationships.
- [ ] GitHub synchronization conflict handling.
- [ ] Structured provider tools, MCP, skills and reusable workflows.

Acceptance: a card runs in its own session, survives restart, produces a
reviewable change set with test evidence, and completes only after acceptance.

## 4. Production 2D authoring

- [ ] Reusable scenes, instances, overrides and resource references.
- [ ] Asset import/reimport and dependency tracking.
- [ ] Comprehensive scene undo/redo and scalable document storage.
- [ ] Script diagnostics/debugging and richer animation authoring.
- [ ] Navigation and dependable platform export presets.

Acceptance: build and export a multi-scene game with reusable characters,
imported assets, animations and recoverable editing history.

## 5. Broader capabilities

- [ ] Mature 3D authoring, materials, lighting, animation and physics.
- [ ] Versioned installable language/tool extensions.
- [ ] Advanced agent orchestration and remote development.

Application features belong in Krait; terminal behavior belongs in Kapsule.
Reusable runtime changes belong in the upstream Kryon repository, committed
there before updating the downstream submodule pointer.
