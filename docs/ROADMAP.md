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
- [ ] Goal acceptance and validation.
- [ ] Review UI for changes, selective acceptance, and checkpoint history.
- [ ] Detect stale writes against read versions and unsaved editor contents.
- [ ] Make multi-file changes transactional; handle external writers and
  filesystem errors without losing the previous recovery checkpoint.
- [ ] Enforce workspace boundaries for file tools, including symlink paths.
- [ ] Context summarization and configurable execution budgets.

Recovery currently covers the latest file-tool write batch only. It does not
undo shell commands or provide filesystem isolation. Resume starts a new
model turn that inspects current state instead of replaying an interrupted
command. Cancellation terminates run-tool commands; graphics and compile-gate
operations still finish their current operation before stopping. Instruction lookup currently reads ~/.kryon/krait/AGENTS.md and
the bound project's AGENTS.md; nested directory rules remain to be implemented.

## 2. Daily coding

- [ ] LSP transport and lifecycle; diagnostics, completion, navigation,
  references, rename, formatting and code actions in the editor.
- [ ] Workspace search/replace with preview, exclusions and regex.
- [ ] Multi-cursor editing, command palette and configurable shortcuts.
- [ ] DAP launch/attach, breakpoints, stepping, variables and call stacks.
- [ ] Configurable build/test tasks and a test-results explorer.
- [ ] Diff/merge review and remote workspaces.

Acceptance: edit, navigate, refactor, test and debug a real non-Kryon project
without leaving Krait, alongside the existing Kryon workflow.

## 3. Agentic Kanban

- [ ] Structured task priorities, labels, dependencies and acceptance criteria.
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
