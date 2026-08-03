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

The compact panel is also exposed through `trace_sender.*` capabilities. Chat
receives `trace_sender_rows` context containing row number, bus, CAN ID, frame
flags, payload, interval, limit, sent count, and status. It can add or edit
drafts, start, stop, send configured rows once, remove or clear rows, copy the
selected Trace frame, save/load JSON lists, or transfer rows to the advanced
Frame Sender. Exact bit changes use `trace_sender.update_bits` with zero-based
byte and bit positions; bit 0 is the least-significant bit. Starting or
transmitting rows still passes through full-access,
arming, and confirmation checks. The older `frame.*` draft/grid actions refer
to the advanced sender and its trigger/modifier workflow.

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

The compact `ai_chat_skill.md` operating guide is supplied to normal chat
requests. Detailed workbench manuals are loaded only for explanatory questions;
ordinary actions rely on the selected domain manifest, native tool schemas and
current application state. This keeps small GUI commands substantially smaller
than source-analysis or documentation requests.

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
model. This evidence is a starting point. Deterministic differential experiments,
diagnostic correlation, signal clustering, counter/checksum inference and DBC
candidate export are available in **RE Tools > Sniffer**. Their results are not
automatically added to the AI prompt; export or describe reviewed candidates
when asking the model to reason about them.

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
open the main analysis tools and propose reviewed edits for UDS and OBD
request lists, OBD dashboards, the CANopen Object Dictionary, DBC databases,
filters, playback sequences, Sniffer experiments, the payload formatter,
script drafts, fuzzing configuration and Frame Sender rows.

Each edit shows its complete JSON arguments before it is applied. Fuzzing
configuration never starts fuzzing, newly created Frame Sender drafts remain
disabled, and script drafts are neither saved nor compiled automatically.

Actions marked `confirm-send` or `armed-confirm-send` require the timed access
gate and an additional confirmation. Unknown capabilities and invalid
arguments are rejected.

One response may contain several action blocks or a JSON array of actions.
SavvyCAN shows the ordered workflow before processing it. Chat normally receives
one combined completion summary per workflow, while every individual result is
still written to the audit log. Ask for detailed or individual action results
when each result should also be shown separately in chat.

Chat history is retained in application settings. Audit records are appended as
JSON Lines under the platform application-data directory and recent records are
shown in the Audit tab on startup.

Use **Clear audit log** in the Audit tab to clear the visible records and delete
the persisted `ai-audit.jsonl` file. New application events start a fresh log.

The model receives current GUI context including the active workspace, selected
trace frame, capture state, connected-bus count, DBC/overwrite state and active
payload display. Capture evidence includes per-ID samples, unique-payload
counts, changed-bit masks, per-byte entropy and per-bit transition counts.

When a review model is selected it reviews both capture analysis and ordinary
chat replies before they are displayed or dispatched.

## Application skills and natural-language actions

Chat routes each current message through an application-owned skill registry.
The registry contains domain triggers, exact capability schemas, operating
rules and worked examples for OBD-II, UDS, CAN frames, CANopen, fuzzing, trace
display, capture playback, DBC editing, reverse-engineering experiments,
connections and scripting. This is local application data rather than model
training or a cloud service.
The installed definitions live in `help/ai_skills.json`; an embedded fallback
keeps the core skills available if that file is missing or invalid.

The focused skills are:

- **SavvyCAN interface** for workspaces, trace options, filters, formatting and
  analysis tools.
- **Raw CAN** for one-shot frames, bounded loops and sender drafts.
- **Capture Playback** for capture sources, transport and replay settings.
- **DBC Signals** for databases, nodes, messages and signal definitions.
- **Reverse Engineering** for Sniffer notching and differential experiments.
- **OBD-II**, **UDS** and **CANopen** for their separate diagnostic object and
  request models.
- **Connections** for existing interfaces and new connection profiles.
- **Fuzzing** for bounded fuzz configuration and guarded execution.
- **Scripting** only when JavaScript or scripting is explicitly requested.

Up to three matching skills can be combined for a cross-domain request. Native
tool definitions are generated directly from `AIActionRegistry`, so the model
does not rely on a separately maintained argument reference.

Each domain is also packaged as a standalone OpenAI Agent Skill under
`skills/`. Every folder has a clearly named `SKILL.md` and
`agents/openai.yaml`, and `skills/skill-bundle.json` provides a human-readable
bundle index. The embedded router records the corresponding package path in
`ai_skills.json`.

Repository-local symlinks under `.agents/skills/` make the individual packages
discoverable in Codex. They can be invoked explicitly as `$savvycan-obd`,
`$savvycan-uds`, `$savvycan-canopen`, `$savvycan-raw-can`,
`$savvycan-interface`, `$savvycan-capture-playback`,
`$savvycan-dbc-signals`, `$savvycan-reverse-engineering`,
`$savvycan-connections`, `$savvycan-fuzzing`, or `$savvycan-scripting`.
ChatGPT desktop uses the same standalone skill format and exposes skills with
`@` mentions. These instruction packages explain workflows; only SavvyCAN's
native capability dispatcher can inspect or operate the running application.

Only the capabilities belonging to the best-matching skills are exposed as
native model tools for that turn. The active workspace provides a small routing
hint, while explicit words in the current message receive greater weight. This
keeps small local models from choosing among the entire application API on
every request. Scripting remains unavailable unless the message explicitly
mentions scripting or JavaScript.

Short follow-ups with no new domain phrase retain the previous skill selection,
so phrases such as "add one more" or "send five more" keep the immediately
preceding OBD or frame context. Clearing chat also clears that routing context.

The skill bundle has an explicit version recorded in model context and audit
entries. **Validate skills** on the Context tab checks the natural-language
routing cases in `ai_skill_evaluations.json`, rejects references to unknown
capabilities and reports any native capability missing from every skill. The
same checks are available in the `test/aiskills.pro` Qt test.

Enable **Preview AI actions without applying them** under Scope and Access to
exercise natural-language routing and argument validation without changing GUI
state or transmitting CAN traffic. The proposed workflow and validation result
are retained in chat and audit history.

The model also receives the current OBD PID list, UDS DID list, request and
response identifiers, bus selection, connection state, polling state and cycle
interval. References such as "the enabled PIDs" or "that DID" can therefore be
resolved against visible application state rather than old chat text.

DBC turns receive a bounded structured view of loaded files, nodes, messages,
signals, scaling, value types, enum tables and multiplex relationships. The
selected trace frame and open Sniffer evidence are included when available, so
the DBC and reverse-engineering skills can be combined to turn a supported
candidate into a reviewed signal edit. Byte-aligned fields can use
`byte_offset`/`byte_length`; arbitrary fields use `start_bit`/`bit_length`.

Application state is relevance-scoped before prompting: raw-frame turns receive
the selected frame, diagnostic turns receive only their workbench state, and
connection turns receive connection profiles. Broader trace and Sniffer state
is included only for matching interface questions.

OBD descriptions are resolved a second time by SavvyCAN's built-in PID catalog
before an action is validated. For example, `rpm`, `engine speed` and `coolant
temperature` resolve to their known Mode 01 PIDs without trusting an identifier
invented by the model. `obd.configure_pids` is a compound intent used for
requests such as:

> Clear the list, add RPM and coolant temperature, then start polling.

SavvyCAN expands that intent into an ordered clear, add and request workflow,
validates every resulting action, applies the configured review and transmit
permissions, and reports the actual result back into chat.

UDS DID names are not globally reliable because many are manufacturer-specific.
The UDS skill therefore uses the current request list or an explicit DID and
asks for clarification when neither is available. It must not infer a
manufacturer-specific DID from a descriptive name alone.

## Online OpenAI and Headroom providers

The Models tab selects one of three providers:

- **Local - Ollama** keeps the existing fully offline behavior.
- **Online - OpenAI API** sends requests to the OpenAI Responses API.
- **Gateway - Headroom / OpenAI compatible** sends OpenAI Responses API
  requests to a Headroom proxy, normally at `http://127.0.0.1:8787`.

Online providers use the same application-skill router, capability schemas,
action normalization, access arming, confirmations and local dispatcher as the
offline provider. A network model can propose an operation, but it never sends
CAN traffic or edits a workbench directly.

Network requests are disabled by default. Enable them explicitly on the
**Online Provider** tab. Filtered CAN evidence uploads have a separate setting
and are also disabled by default. Application state, compressed chat history,
capture evidence, provider-side storage and per-request confirmation each have
independent controls. The confirmation dialog displays the complete prompt
text before protocol wrapping.

**Confirm each online API call** is enabled by default. Disable it to stop the
preview popup on normal sends. When a Review model is selected, a chat turn can
make two API calls: the primary generation and the review. Selecting **None**
for Review model avoids the second call.

This privacy control is separate from **Confirm CAN transmissions** in Scope
and Access. **No popups while armed** suppresses approval dialogs for
AI-proposed CAN and diagnostic transmissions; it does not suppress online
prompt previews.

An HTTP `429 Too Many Requests` response is a temporary provider rate or quota
limit. SavvyCAN keeps the selected online provider active, displays any
`Retry-After` guidance returned by the provider and allows a later retry. It
does not fall back to Ollama automatically.

The usage line reports tokens, not a request count. For example, `5092 input
tokens` means one request contained approximately that much context. Input can
include the current question, the selected application skill and native tool
schemas, permitted GUI state, chat history, relevant help extracts and
Graphify context. Disable unneeded context controls or clear chat to reduce it.

Short connectivity messages such as `test`, `ping` and `are you working` use a
minimal online prompt without application documentation, history or native
tools. If these still receive HTTP `429`, the limiting factor is the provider
account, quota or request allowance rather than SavvyCAN's normal context size.

For direct OpenAI access, provide `OPENAI_API_KEY` in the process environment
or enter a key for the current SavvyCAN process. For Headroom, use
`HEADROOM_API_KEY` when a separately managed or remote proxy requires
authentication. Requests to SavvyCAN's managed localhost Headroom process use
the managed upstream OpenAI key automatically, including as the bearer
credential required by Headroom's Responses API route. Keys entered in the
provider/gateway key field are held only in memory and are not written to
`QSettings`, profiles, projects or audit logs. The managed Headroom upstream
key has its own explicitly enabled storage control described below. Non-local
endpoints must use HTTPS.

Headroom is an optional optimizer and gateway. It does not itself provide an
OpenAI model entitlement. Configure its upstream provider separately, then
select the upstream model name in SavvyCAN. **Token reduction** prioritizes
immediate prompt compression. **Cache stability** preserves stable prompt
prefixes so the upstream provider can reuse its prompt cache. The mode is
applied when the managed proxy starts; restart Headroom after changing it.
SavvyCAN uses Headroom's OpenAI Responses API route so reasoning models and
GUI function tools can be used together. It does not add a provider-specific
mode parameter to individual requests. Saved endpoint URLs ending in
`/chat/completions` are normalized to `/v1/responses` automatically.

SavvyCAN can manage a project-local Headroom installation without terminal
commands. Select **Gateway - Headroom / OpenAI compatible**, then use the
**Managed Headroom** controls:

1. Select **Install**. SavvyCAN creates `local-ai/headroom` and installs the
   Headroom proxy there. Python 3 and an internet connection are required for
   this one-time download.
2. Enter the upstream OpenAI key. Enable **Remember upstream key on this
   computer** to keep it in SavvyCAN's per-user application-data directory with
   owner-only file permissions. Otherwise the key remains in memory only.
3. Select **Start**, or leave **Auto-start** enabled. SavvyCAN starts the proxy
   on `127.0.0.1:8787`, routes Headroom requests through it, and stops the
   managed process when SavvyCAN exits.
4. Select **Test connection** to run a real health request. A successful local
   Headroom test displays **Verified** and enables docked chat. SavvyCAN also
   runs this check shortly after starting its managed proxy. For direct OpenAI,
   the test makes an authenticated request to the provider's model endpoint.

The docked chat **Send** button is disabled while a request is running, when
network-provider access is disabled, or after a connection test/request fails.
Re-enable provider access and select **Test connection** to verify and restore
chat availability.

The separate **API key** field authenticates SavvyCAN to a remote gateway. It
is normally left blank for the managed localhost proxy, where SavvyCAN
automatically supplies the upstream key. The remembered upstream key is not
stored in the repository, project files, `QSettings`, chat history or audit
output. It is protected by filesystem permissions rather than an
operating-system credential vault; disable the remember option to delete it.

Usage from the most recent network response is displayed in the Online
Provider tab. Audit records contain the provider, destination hostname and
estimated token count, but never the API key or prompt contents.

The Audit tab displays the newest event first. Its persisted `ai-audit.jsonl`
file remains append-only and chronological.

After each successful Headroom request, SavvyCAN reads the proxy's `/stats`
totals and audits active compression separately from provider prefix-cache
reuse. Headroom deliberately retains instructions, user text and stable prompt
prefixes; a normal stateless SavvyCAN chat request can therefore have no
eligible content to compress even though Headroom is working. Large tool
results and redundant tool schemas provide the main active-compression
opportunity. Cache-read tokens and their estimated discount are reported
separately when the upstream provider supplies cache metrics. USD values
depend on Headroom's provider-pricing data.

## Graphify repository context

Graphify can complement the application skill registry with structural
knowledge of the SavvyCAN source tree. It does not replace the capability
schemas or live workbench state: Graphify explains how the implementation is
connected, while the registry remains the authority for actions the running
application can perform.

Generate a graph with Graphify so the repository contains:

```text
graphify-out/graph.json
graphify-out/GRAPH_REPORT.md
graphify-out/graph.html
```

Then enable **Use Graphify repository context** on the AI Workbench
**Context** tab. SavvyCAN auto-detects a graph beside the source or build
directory, and a different `graph.json` can be selected manually.

When the `graphify` executable is available, SavvyCAN runs a local, bounded
`graphify query` only for source-code, implementation, build or architecture
questions and adds only the relevant subgraph to
the model context. The query budget is configurable from 200 to 4000 tokens.
Arguments are passed directly to `QProcess` rather than through a shell. If the
CLI is unavailable but `GRAPH_REPORT.md` exists, SavvyCAN uses a bounded portion
of that report as broad orientation.

Graphify repository context is enabled by default when a graph is available,
and local models can use it without a network connection. For OpenAI or a
Headroom gateway, Graphify context is withheld unless **Permit retrieved
Graphify context in network requests** is separately enabled. The normal
network request preview shows the resulting prompt before it is sent.

Graph provenance remains important. `EXTRACTED` edges are parsed source
relationships. `INFERRED` and `AMBIGUOUS` edges are supplied as hypotheses and
must not override live application state, capability validation or bus-access
permissions.

The model proposes an action using a fenced block:

~~~~text
```savvycan-action
{"capability":"obd.add_pid","arguments":{"pid":"0x0C","format":"auto"}}
```
~~~~
