# AI Analysis Workbench

The AI Analysis Workbench combines deterministic capture statistics with a
local language model. It does not upload data to an external service. The first
version supports Ollama's local HTTP API.

The upper controls are grouped into three tabs so chat and analysis retain most
of the available space. **Models** manages the runtime and installed models,
**Scope & Access** controls which traffic and guarded actions the model can
use, and **Live Capture** contains resource statistics, evidence buffering,
filters and experiment markers.

SavvyCAN checks for a project-local runtime at `local-ai/bin/ollama`. When
present, it starts that runtime on `127.0.0.1:11435`, stores models under
`local-ai/models`, and stops the process when the workbench is destroyed.
The complete `local-ai` directory is excluded from Git because runtimes and
model files are large and platform-specific.

The full workbench is a main workspace tab. A lightweight **AI Chat** dock is
shown on the right side of the main window and shares the workbench's selected
model, runtime and conversation. The dock can be resized, moved, floated or
closed without relaying out the full workbench.

## Chat

The **Chat** tab keeps a multi-turn conversation for the current SavvyCAN
history. It uses the selected analysis model. Enable **Include capture
snapshot** to attach the same bounded, filtered evidence used by capture
analysis; leave it disabled for generic questions. **Clear** discards the
conversation shown and sent to subsequent turns. The bounded history survives
application restarts. In both chat views, press **Enter** to send and
**Shift+Enter** to insert a newline.

Messages are displayed as individual responsive bubbles. Your messages use a
right-aligned blue bubble, assistant messages use a left-aligned neutral
bubble, and application notices are centered separately. Bubbles reflow as the
workbench or dock is resized.

Chat may propose CAN frames and guarded diagnostic operations. Transmission is
blocked in Read only mode; Full bus access requires timed arming plus a separate
confirmation for every operation.

The arm duration can be set to 1, 5 or 15 minutes, 1 hour, or **Indefinite**.
Indefinite arming remains active until it is manually disarmed, the access mode
is changed, or Emergency Stop is used. Its checked state is restored after an
application restart. Timed arming is not restored after restart. Arming does
not bypass the selected transmission-confirmation policy.

Chat can propose `frame.send_once` for a validated, single-shot classic CAN
frame. Multiple requested frames are represented as an ordered action list and
each frame remains subject to the access and confirmation checks.

The direct phrase `send N random frames` is handled deterministically by
SavvyCAN rather than the language model. It creates 1-100 standard 11-bit,
eight-byte frames on the selected AI scope bus and submits canonical
`frame.send_once` actions to the configured access and confirmation policy.

`frame.send_loop` provides a bounded repetition of one validated frame. The
count is limited to 1-1000 and the interval to 1-60000 ms. Repeated sends are
added as enabled rows in the CAN Trace sender table, making them visible and
editable without opening another window. Rows disable automatically after the
requested count.
Single frames use the native direct-send path instead. **Emergency stop**
disables sender rows and disarms full access.

The **Confirm sends** setting controls confirmation popups:

- **Every transmission** reviews the workflow and confirms each send.
- **Once per workflow** uses one explicit workflow approval for all listed sends.
- **No popups while armed** executes validated actions without confirmation
  dialogs until access expires or is disarmed. Access checks and audit logging
remain enabled.

The selected Read only or Full bus access mode is restored on restart. An
indefinitely armed Full access selection is also restored; timed arming starts
disarmed.

Scripting documentation and suggestions are excluded from ordinary chat tasks.
They are included only when the user explicitly asks for a script or
JavaScript.

Current app-wide action coverage and known gaps are tracked in
`ai_capability_audit.md`.

Recognized actions can populate request lists, Object Dictionary entries,
graphs, dashboard widgets, filters, payload formats, scripts, fuzzing settings
and Frame Sender drafts after review.

The action parser also normalizes common model-generated OBD aliases such as
`clear_obd_pid_requests`, `modify_obd_pid_requests` with `remove_all`, and
`add_obd_pids`. Every normalized edit still requires user confirmation.

Before each chat request, SavvyCAN supplies a structured capability registry
and retrieves relevant excerpts from its installed help pages. Questions about
UDS, OBD-II, CANopen, payload formatting, frame sending or scripting therefore
receive the corresponding application documentation without loading the source
tree or every manual into the model context.

Qwen, Llama 3.1 and Mistral models receive that registry through Ollama's
native tool-call API, allowing ordinary language to select a real SavvyCAN
operation. Printed `savvycan-action` JSON remains as a compatibility fallback.
Every action is validated and passes through the configured access, arming and
confirmation policy. Scripting drafts are used only when scripting is
explicitly requested.

## Local runtime

Install Ollama separately, start it, and download at least one model. The
default endpoint is:

```text
http://127.0.0.1:11434
```

Use **Refresh models** to read the models installed in that runtime. The model
fields remain editable, so a known model tag can be entered manually.

The **Manage models** row handles project-local model installation without a
terminal. Select a suggested tag or type any Ollama model tag, then choose
**Download** to store it under `local-ai/models` for offline use. Downloads can
be several gigabytes. **Remove** deletes that model from the project-local store. The
workbench starts the bundled runtime when needed and refreshes both model
selectors after the operation completes.

Suggested local models include:

- `qwen3:8b` for the main CAN-data analysis pass.
- `gemma3:4b` for a quick independent review.
- `qwen2.5-coder:7b` for formatter expressions, scripts and code-oriented work.
- `deepseek-r1:7b` for an alternate reasoning pass.
- `gpt-oss:20b` for a stronger reasoning pass when 16 GB of combined memory
  and slower GPU/CPU offload are acceptable.
- `llama3.1:8b` as a general-purpose comparison model.
- `mistral:7b` as a compact general-purpose alternative.

The model dropdowns show approximate runtime RAM and download sizes. Runtime
RAM is an estimate: longer context, parallel requests and CPU/GPU offload can
increase it. The bundled runtime keeps a completed model loaded for at most
about 30 seconds. The lightweight server remains ready between requests; use
the **Stop** button in the Models tab or **Emergency stop** to shut it down.

The main-window footer reports **AI: starting**, **AI: generating**,
**AI: ready**, **AI: stopped** or **AI: error**. **AI: ready** describes the
server, not model memory: the model may have unloaded after its idle timeout
and will load again for the next request.

**Start bundled runtime** selects the project-local installation. The endpoint
field can still target a separately managed Ollama instance when preferred.

The analysis model produces the first result. An optional review model receives
the same evidence and the first result, then checks claims and returns a revised
answer. Select **None** when only one model should run. The models run
sequentially rather than simultaneously, which avoids competing for GPU memory.

Reasonable starting points depend on available memory:

- A 4B to 8B quantized model is suitable for quick experiments on an 8 GB GPU.
- A 12B-class quantized model may use GPU and system-memory offload.
- `gpt-oss:20b` is a stronger reasoning option but needs about 16 GB total
  inference memory, so a laptop with 16 GB system RAM and an 8 GB GPU may run it
  slowly or leave too little memory for SavvyCAN.

Model output quality must be evaluated on actual captures. No general language
model has authoritative knowledge of an unknown vehicle's proprietary signals.

## Capture scope

The workbench analyzes a recent portion of SavvyCAN's current capture:

- **Bus** selects one bus or all buses.
- **CAN-ID allowlist** accepts comma-separated hexadecimal IDs and ranges, such
  as `123, 7E0-7EF, 18DAF100`.
- **Recent frame limit** limits preprocessing time and model evidence size.

For every accepted identifier, SavvyCAN supplies frame counts, length range,
capture duration, mean interval, first payload, observed changed-bit mask and a
small sample of payloads. The **Evidence** tab shows the exact JSON sent to the
model. This evidence is a starting point; richer transition, entropy,
correlation, counter and checksum tools remain roadmap items.

## Resources

The Resources and live evidence panel updates every two seconds. It reports
whole-system CPU use, used/total/free system RAM, SavvyCAN resident memory and
free space on the filesystem containing the working directory. On NVIDIA
systems it also reports GPU name, compute utilization, allocated/total VRAM,
temperature and power through `nvidia-smi`. A very small allocated-VRAM value
while **AI: ready** normally means the model has unloaded after its idle
timeout. Watch the reading while **AI: generating** to verify GPU offload.
These readings stay on the local computer.

The context label estimates prompt tokens before inference. Tokens are local
model text units, not API credits. A conservative model-dependent limit blocks
oversized requests before starting inference. If the bundled worker reports
that it was killed or ran out of memory, Full access is disarmed, the prompt is
preserved, and the workbench recommends reducing capture size or model size.

## Live evidence

Select **AI live buffer**, then use **Start**, **Pause / Resume**, **Stop** and
**Reset** to manage a capture that is separate from the main CAN trace. Frame
and duration limits bound its memory use. **Preview** builds the exact evidence
JSON without contacting a model.

The live filter can use:

- the AI bus and CAN-ID allowlist;
- enabled main-window CAN-ID filters;
- the currently visible Sniffer IDs; or
- all applicable filters combined.

When **Ignore Sniffer-notched bits** is enabled, the Sniffer notch mask is
applied locally as frames enter the AI buffer. Notched activity therefore does
not consume model context. Sniffer filtering requires an open Sniffer window
with its desired IDs selected.

Experiment markers attach a label, local timestamp and AI-buffer position to
the evidence. Use these for physical actions such as pressing a switch or
changing a control.

Automatic live analysis batches changes at the configured interval and sends a
new summary only after the minimum changed-bit threshold is met. Raw frames are
never streamed token by token to the model.

Older chat context is compressed deterministically: recent conversation,
application outcomes and confirmed/hypothesis lines are retained while
repetitive older text is omitted.

**Emergency stop** cancels the active request, stops live capture and automatic
analysis, disarms Full access and stops the bundled runtime.

## Instructions and results

Describe the action or experiment in the instruction editor. Useful context
includes what changed physically, relevant timestamps, suspected units and
known identifiers. **Analyze capture** sends the deterministic evidence to the
selected local model. **Stop** cancels the current request.

The model is instructed to distinguish observations from hypotheses and return
JSON containing:

- summary
- observations
- hypotheses
- formatter candidates
- next experiments
- cautions

Generated formatter or DBC definitions are suggestions and are not applied
automatically.

## Access policy

**Read only** prevents every AI-originated transmit action. **Full bus access**
must be explicitly armed and expires after five minutes. A separate modal
confirmation displays the capability and complete arguments before each native
transmit operation. Workbench connection, validation and busy-state checks
still apply, and the model cannot approve its own request.

Guarded operations include UDS DID reads, DID scans and DTC reads; OBD PID
queries and discovery; CANopen node scans, SDO uploads and SDO writes; enabling
a reviewed Frame Sender draft; and starting a configured fuzz run. AI-started
fuzzing must have a duration from 100 ms through 60 seconds and stops
automatically.

The **Audit** tab records model discovery, analysis and review requests,
completion, failures, cancellation and blocked access attempts.

## App-wide actions

Chat uses the same capability catalog as the application dispatcher. It can
open the main analysis tools and propose reviewed edits for UDS DID lists, OBD
PID lists, the CANopen Object Dictionary, the main payload formatter, script
drafts, fuzzing configuration and disabled Frame Sender rows.

Each edit shows its complete JSON arguments before it is applied. Fuzzing
configuration never starts fuzzing, Frame Sender drafts remain disabled, and
script drafts are neither saved nor compiled automatically.

Actions marked `confirm-send` or `armed-confirm-send` require the timed access
gate and an additional confirmation. Unknown capabilities and invalid
arguments are rejected.

One response may contain several action blocks or a JSON array of actions.
SavvyCAN shows the ordered workflow before processing it. Each result is added
to subsequent conversation context and written to the audit log.

Chat history is retained in application settings. Audit records are appended as
JSON Lines under the platform application-data directory and recent records are
shown in the Audit tab on startup.

The model receives current GUI context including the active workspace, selected
trace frame, capture state, connected-bus count, DBC/overwrite state and active
payload display. Capture evidence includes per-ID samples, unique-payload
counts, changed-bit masks, per-byte entropy and per-bit transition counts.

When a review model is selected it reviews both capture analysis and ordinary
chat replies before they are displayed or dispatched.

The model proposes an action using a fenced block:

~~~~text
```savvycan-action
{"capability":"obd.add_pid","arguments":{"pid":"0x0C","format":"auto"}}
```
~~~~
