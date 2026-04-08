# Training Mode for Patch Approvals

## 1. Purpose

This document specifies the `xcodex` design required to support a new patch-approval outcome: `Training Mode`.

The intent is to let the user branch from a proposed code edit into a training-oriented workflow that transforms the proposed patch into a structured learning artifact, such as pseudocode, intent scaffolding, or guided hints, and renders that artifact in Neovim.

This design is driven by one hard requirement:

- training-specific prompt text and instructions must not contaminate the main agent conversation context

In practical terms, selecting `Training Mode` once must not bias later edits in the same thread.

## 2. Goals

- Add `Training Mode` as a fifth option in patch approval prompts.
- Preserve access to the exact proposed file changes at the moment the user chooses the option.
- Generate the training representation via a one-off helper model call.
- Keep that helper call isolated from the main thread's future context.
- Render the result in Neovim only.
- Support a completion handshake from Neovim/plugin similar to manual apply.
- Reuse existing patch approval, editor launch, and result-artifact patterns where possible.
- Keep the main patch approval flow stable for existing approve/deny/manual-apply behavior.

## 3. Non-Goals

- Rendering training output inside the main `xcodex` conversation transcript.
- Recording training prompts or training output into the parent conversation history.
- Reusing the review-mode writeback flow.
- Defining the full Neovim plugin implementation beyond the file contract it should receive.
- Designing a generic hidden-agent framework for all future features in v1.

## 4. User Experience

### 4.1 Prompt Expansion

When `xcodex` shows the patch approval modal, the choices become:

- `Yes, proceed`
- `Yes, and don't ask again for these files`
- `Manual apply in editor`
- `Training Mode`
- `No, and tell xCodex what to do differently`

The prompt text remains:

- `Would you like to make the following edits?`

Recommended shortcuts:

- `y` => `Yes, proceed`
- `a` => `Yes, and don't ask again for these files`
- `m` => `Manual apply in editor`
- `t` => `Training Mode`
- `n` and `Esc` => `No, and tell xCodex what to do differently`

### 4.2 Training Mode Flow

1. Agent proposes file edits.
2. `xcodex` renders the diff summary in the existing approval modal.
3. User selects `Training Mode`.
4. `xcodex` preserves the original patch payload.
5. `xcodex` performs a one-off helper model call that transforms the patch into a training representation.
6. `xcodex` serializes a stable training payload containing the original patch plus the generated training artifact.
7. `xcodex` launches the configured external editor command.
8. Neovim/plugin renders the training artifact.
9. Neovim/plugin writes a completion result artifact.
10. `xcodex` resolves the pending patch approval based on that result.

The training representation is visible in Neovim only. It is not injected into the main chat transcript.

## 5. Hard Context-Isolation Requirement

### 5.1 Required Behavior

Training Mode must not cause either of the following to become part of the parent thread's ongoing conversation state:

- the training-specific prompt
- the helper model's generated training output

Later generations in the same parent thread must behave as if Training Mode never added any extra in-band instructions.

### 5.2 What Counts as Contamination

For this feature, contamination means any path that causes training-mode prompt/output to be written into the parent session's conversation history state.

The core boundary is:

- `Session::record_conversation_items(...)`
- `Session::record_into_history(...)`

Anything passed through those parent-session paths becomes part of future same-thread context.

### 5.3 Allowed Boundary

The helper transformation may run in a separate sub-agent session, but its prompt/output must remain isolated unless explicitly written back to the parent.

The design in this document forbids that writeback.

## 6. Current Baseline

Today the patch approval flow is:

1. `ApplyPatchRuntime::start_approval_async()` requests approval.
2. `Session::request_patch_approval()` stores a pending `oneshot::Sender<ReviewDecision>`.
3. `EventMsg::ApplyPatchApprovalRequest` is emitted.
4. TUI converts that event into `ApprovalRequest::ApplyPatch`.
5. `ApprovalOverlay` renders the diff and choices.
6. A selection emits either:
   - `AppEvent::CodexOp(Op::PatchApproval { ... })`, or
   - `AppEvent::OpenManualPatchApply(...)`
7. Core resolves the pending approval.
8. The patch runtime either applies the patch, skips it, or aborts.

Manual apply already proves that:

- a patch approval can branch into an app/editor-local workflow
- the full patch payload is still available at that branch point
- the branch can later resolve the original patch approval

## 7. Chosen Architecture

### 7.1 Summary

Training Mode should be implemented as:

- a new TUI patch-approval option
- which branches into an app-local workflow
- which invokes an isolated helper sub-agent / one-shot model call
- which writes the result to a Neovim-facing payload
- which never records helper prompt/output into the parent session history
- which later resolves the original patch approval based on a result artifact

### 7.2 Why This Architecture

This combines two repo-proven patterns:

- manual apply for the TUI/editor branching and completion artifact
- review/delegate sub-agent machinery for isolated one-off model execution

It satisfies all agreed constraints:

- separate option to manual apply
- helper model call, not local-only transformation
- Neovim-only output
- low contamination risk

## 8. Implementation Requirements

### 8.1 Add a New TUI-Only Approval Action

Add a new approval action in the patch approval overlay:

- `TrainingMode`

This should be added to the TUI-layer `ApprovalDecision` enum, not overloaded onto `ReviewDecision`.

Rationale:

- `ReviewDecision` is the core approval result.
- `TrainingMode` is a UI workflow choice that must run extra logic before the final approval result is known.

### 8.2 Add a New App Event

Add a dedicated app-level event:

- `AppEvent::OpenTrainingMode(TrainingModeRequest)`

The request must preserve:

- patch approval id
- optional turn id
- current working directory
- full `HashMap<PathBuf, FileChange>`
- optional reason

Rationale:

- `Op::PatchApproval` is too small; it loses the diff payload
- Training Mode needs the original patch data before the final approval result is chosen

### 8.3 Add an App-Level Training Workflow

The app layer should own the full Training Mode workflow:

1. receive `OpenTrainingMode`
2. preserve the patch request payload
3. invoke an isolated helper model call
4. collect the helper output
5. write a stable training payload file
6. launch the external editor with that payload
7. wait for a result artifact
8. resolve the original patch approval

The app layer should not push the helper result into the parent conversation transcript.

### 8.4 Use an Isolated Helper Model Call

The helper model call should be implemented using the existing sub-agent/delegate machinery rather than by submitting an in-band prompt into the parent thread.

Required behavior:

- helper prompt runs in a separate sub-agent session
- helper output is returned to the app workflow only
- parent thread history is not updated with helper prompt or helper output

### 8.5 Do Not Reuse Review Writeback Semantics

Review mode is not an acceptable end-to-end template because it explicitly writes synthesized output back into the parent session history when the review finishes.

Training Mode must not do that.

Allowed reuse:

- helper sub-agent execution mechanics
- event forwarding mechanics

Forbidden reuse:

- review-mode parent-history writeback

## 9. Contamination Rules

### 9.1 Allowed

- helper sub-agent prompt stored only in helper session
- helper sub-agent output stored only in helper session
- training payload file written for Neovim
- local app/TUI informational state
- local result-artifact files

### 9.2 Forbidden

- parent `record_conversation_items(...)` for training prompt/output
- parent `record_into_history(...)` for training prompt/output
- turning training output into a parent-thread assistant message
- turning training prompt into a parent-thread user/developer message

### 9.3 Expected Result

If implemented correctly:

- training instructions do not leak into the parent conversation context
- training output does not persist in parent history
- future same-thread generations are not influenced by the training workflow

## 10. Helper Model Call Design

### 10.1 Required Properties

The helper call must be:

- one-off
- isolated from the parent thread
- non-interactive from the user's perspective
- invisible in the main transcript

### 10.2 Suggested Input Shape

The helper prompt should be synthesized from:

- patch diff / `FileChange` payload
- optional reason text
- explicit instructions to transform the patch into a training representation

Suggested training output shape:

- summary of intent
- pseudocode or implementation outline
- structured hints per file/hunk
- optional misconceptions / watch points

The exact prompt text is implementation-defined, but it must stay inside the helper session only.

### 10.3 Output Handling

The helper output should be parsed into a structured training artifact where possible.

If parsing fails:

- the raw helper text may still be embedded into the editor payload
- but it must remain app-local / editor-local

## 11. Proposed Training Payload

`xcodex` should write a stable JSON payload for Neovim/plugin.

Suggested shape:

```json
{
  "schema_version": 1,
  "kind": "training_mode_request",
  "approval_id": "call_123",
  "thread_id": "optional-parent-thread-id",
  "turn_id": "optional-parent-turn-id",
  "cwd": "/repo",
  "reason": "optional approval reason",
  "changes": [
    {
      "path": "/repo/src/app.rs",
      "kind": "update",
      "move_path": null,
      "diff": "@@ -10,2 +10,3 @@\n-foo\n+bar"
    }
  ],
  "training": {
    "format": "structured_hints_v1",
    "summary": "High-level learning goal",
    "items": [
      {
        "path": "/repo/src/app.rs",
        "intent": "Describe the change at a learning level",
        "pseudocode": [
          "step 1",
          "step 2"
        ],
        "hints": [
          "hint 1",
          "hint 2"
        ]
      }
    ],
    "raw_text": "optional raw helper output"
  }
}
```

### 11.1 Payload Requirements

- `schema_version` for forward compatibility
- explicit `kind`
- original patch payload preserved
- helper-generated training artifact embedded
- absolute `cwd`
- absolute paths in `changes`
- optional parent `thread_id`
- optional parent `turn_id`

## 12. Editor Launch Contract

### 12.1 Launch

`xcodex` launches the configured external editor with one argument:

- path to the generated training payload file

This matches the current manual-apply style of editor integration.

### 12.2 Neovim Responsibility

The Neovim/plugin side is responsible for:

- reading the payload
- rendering the training artifact
- deciding when the workflow is complete
- writing the result artifact

### 12.3 Neovim-Only Visibility

The training artifact should be visible in Neovim only.

The app may show minimal local status or error messages, but the training content itself should not appear in the main chat transcript.

## 13. Completion and Approval Resolution

### 13.1 Completion Artifact

Training Mode should mirror manual apply's result-file contract.

Suggested result shape:

```json
{
  "schema_version": 1,
  "kind": "training_mode_result",
  "approval_id": "call_123",
  "status": "completed"
}
```

Allowed statuses:

- `completed`
- `cancelled`
- `failed`

### 13.2 Resolution Semantics

Per the agreed requirement, Neovim/plugin may mark Training Mode completed like manual apply.

Recommended patch approval resolution:

- `completed` => `ReviewDecision::ExternallyApplied`
- `cancelled` => `ReviewDecision::Abort`
- `failed` => `ReviewDecision::Abort`

Rationale:

- `ExternallyApplied` already means "the patch was satisfied externally"
- it already short-circuits patch execution while allowing the turn to continue

### 13.3 Why Not Always Abort

Always aborting would discard the resumed-flow benefit that manual-apply completion already introduced.

Because the plugin can mark the workflow completed, Training Mode should resolve as externally satisfied when appropriate.

## 14. Exact Control-Flow Hooks

### 14.1 Patch Approval Event Enters TUI

Patch approval requests are converted to TUI approval state in:

- `codex-rs/tui/src/chatwidget.rs`
- `handle_apply_patch_approval_now(...)`

At that point, the proposed diff is available as:

- `changes: HashMap<PathBuf, FileChange>`

### 14.2 Approval Options

Patch approval choices are assembled in:

- `codex-rs/tui/src/bottom_pane/approval_overlay.rs`
- `patch_options()`

This is where `Training Mode` should be inserted as the fifth option.

### 14.3 Selection Branching

Patch-approval selection currently branches in:

- `apply_selection(...)`

This is where `TrainingMode` should branch to a new app event, parallel to `ManualApply`.

### 14.4 App Handling

Manual apply is handled in the app layer already.

Training Mode should add a parallel handler that:

- runs helper transformation
- writes payload
- launches editor
- reads result
- submits final `Op::PatchApproval`

### 14.5 Core Approval Resolution

The original patch approval must still ultimately resolve through:

- `Op::PatchApproval`

This keeps the core tool orchestration model unchanged.

## 15. Exact Files / Modules Involved

Primary TUI branch points:

- `codex-rs/tui/src/chatwidget.rs`
- `codex-rs/tui/src/bottom_pane/approval_overlay.rs`
- `codex-rs/tui/src/app_event.rs`
- `codex-rs/tui/src/app.rs`
- `codex-rs/tui/src/external_editor.rs`

Core isolation / approval boundaries:

- `codex-rs/core/src/codex.rs`
- `codex-rs/core/src/tools/runtimes/apply_patch.rs`

Helper-model execution machinery:

- `codex-rs/core/src/codex_delegate.rs`
- `codex-rs/core/src/tasks/review.rs`

Parallel `xcodex` TUI implementation that likely must stay aligned:

- `codex-rs/xcodex/tui2/...`

## 16. Recommended Implementation Strategy

### 16.1 Phase 1

Implement Training Mode with:

- new fifth TUI option
- app event + app handler
- isolated helper sub-agent call
- stable training payload file
- external editor launch
- result artifact
- `ExternallyApplied` / `Abort` resolution

Do not write training content into parent history.

### 16.2 Phase 2

Generalize helper-sub-agent labeling if needed.

The current delegate path is review-oriented in naming and source tagging. Training Mode should eventually have its own source label rather than masquerading as review.

### 16.3 Phase 3

Optionally improve payload schema and plugin UX once the isolation boundary is proven.

## 17. Rejected Alternatives

### 17.1 Same-Thread In-Band Prompt

Rejected because:

- it contaminates parent conversation context
- it can influence later same-thread generations
- it violates the primary design requirement

### 17.2 Local-Only Transformation

Rejected because the agreed requirement is to use a one-off helper model call.

### 17.3 Review-Mode Writeback Pattern

Rejected because review explicitly writes synthesized results into the parent conversation history, which Training Mode must not do.

## 18. Unknowns / Follow-Ups

- Whether to add a dedicated helper-session source label instead of reusing the review-oriented delegate defaults.
- Whether the training artifact should have a fully structured schema in v1 or allow a mostly free-form `raw_text` field plus minimal structure.
- Whether both `codex-rs/tui` and `codex-rs/xcodex/tui2` must be updated in the same change to preserve parity.
- Whether the plugin should validate that the current file contents still match the expected patch context before marking completion.

## 19. Acceptance Criteria

- Patch approval modal shows five options, including `Training Mode`.
- Selecting `Training Mode` preserves the full proposed patch payload.
- `xcodex` performs a one-off helper model transformation without submitting training instructions in-band to the parent thread.
- Training prompt/output is not recorded into the parent session history.
- Training artifact is rendered in Neovim only.
- Neovim/plugin can mark the workflow `completed`, `cancelled`, or `failed`.
- `completed` resolves the original patch request as externally satisfied.
- `cancelled` and `failed` resolve the original patch request as aborted.
- Later generations in the same parent thread are not influenced by the training helper prompt/output.
