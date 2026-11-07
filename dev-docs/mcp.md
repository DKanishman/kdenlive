# Experimental native MCP control

This development API adds an opt-in, same-user MCP endpoint to the actual Kdenlive
GUI. Direct editing tools use native bin/timeline/effect models and document undo
history. Interactive tools call native actions and Qt controls. They do not
maintain a second timeline model or simulate mouse/keyboard coordinates.

The expanded source reports version `0.4.0`. Build/runtime evidence must match the
executable being used; stock Kdenlive and older development installations do not
acquire these tools from a source checkout. This expansion is under validation.
**Complete coverage of every editor function has not been established.**
`get_capabilities` reports that distinction.

## Start and connect

For the Linux/Fedora build helper, matching runtime, connection configuration
and current platform blockers, see [Native MCP on Linux](mcp-linux.md).

Build/install Kdenlive and its normal dependencies. This branch supplies the
reviewed [MLT dependency patches](../packaging/patches/README.md) for bulk
playlist inspection, SDL device lifetime and Windows OpenFX registration. Build
MLT in a separate prefix and configure Kdenlive with
`-DMlt7_DIR=<isolated-prefix>/lib/cmake/Mlt7` to use its matching headers and libraries.
On Windows, keep the compatible runtime DLLs beside the executable and the MLT
modules in its parent prefix's `lib/mlt` directory.

Start the interactive editor
with an explicit unique endpoint:

```powershell
& 'C:/path/to/bin/kdenlive.exe' --no-welcome --mcp-socket kdenlive-mcp-local
```

MCP is disabled without `--mcp-socket`. Names have 8-100 ASCII letters, digits,
dots, underscores or hyphens and start with a letter or digit. An existing
instance's endpoint is never removed to obtain its name. MCP-enabled launches
use Qt file dialogs so native UI tools can inspect them.

Configure a stdio MCP client to run an explicit Python 3.10+ interpreter and the
standard-library connector:

```powershell
& 'C:/path/to/python.exe' -B 'C:/path/to/bin/kdenlive-mcp.py' --socket kdenlive-mcp-local --timeout 30
```

Windows uses a local named pipe. Linux accepts the actual absolute AF_UNIX path
of the Qt endpoint. Same-user access is enforced on those platforms; other
platforms are currently refused. One client is accepted; no TCP port is opened.
A socket name is an address rather than a secret.

## Contract and state

The service implements `initialize`, `ping`, `tools/list` and `tools/call`, with
protocol revisions `2025-06-18` and `2025-11-25`. Messages are one UTF-8 JSON-RPC
object per line, bounded to 1 MiB each way. Batches are unsupported. String request
IDs have at most 128 UTF-16 code units; notifications never execute edits.

Use the connected executable's `tools/list` as the authoritative argument schema.
Catalogs can include operations with unavailable native prerequisites or
unsupported combinations. Discovery is not successful execution.

1. Read `get_info`, `get_capabilities` and `get_state`.
2. Rediscover native IDs after opening or switching projects/sequences.
3. Pass the exact latest `state.expectedState` to direct editing commands. Large projects return a compact header; use `get_state_page` with that same token to read complete native sections.
4. Inspect native readback/state and the saved/rendered result when persistence
   or audiovisual correctness matters.

Saved/populated projects are no longer excluded solely because they are not new.
Per-operation readiness, type, group, mix, lock, selection, frame, size and
dependency checks remain. Tokens bind document/sequence, undo revision and
relevant native state, including subtitle metadata, bin hierarchy and markers.
Manual changes can invalidate a prepared operation. After `STALE_STATE`, inspect
and plan again instead of replaying the old edit.

Successful tools return `ok:true` in `structuredContent` and a text copy of the
same data. Failures return `isError:true`, `ok:false` and `error:{code,message}`,
usually with state. Protocol errors are JSON-RPC errors. Acceptance of an
asynchronous operation is not completion.

Large replies may omit the duplicate text while retaining complete `structuredContent`.
An oversized result instead returns a receipt with the original outcome and job IDs.
Use `get_result_page` to read its immutable contents, then explicitly call
`release_result_receipt`. A pending receipt blocks new edits; owned-job cancellation
remains available. Reading or releasing a receipt never replays or undoes an operation.
After reconnecting, `get_result_page` with no arguments discovers any pending receipt.

Editing times are integer project frames. Clips occupy the half-open range
`[startFrame,startFrame+durationFrames)`; source `inFrame/outFrame` are inclusive.
Subtitle ends and zone ends are exclusive where stated by their schemas. Follow
the individual tool contract rather than assuming every native interval matches.

## Native tools

These are source registrations in the development API. This list does not replace
build/test evidence or the connected executable's `tools/list`.

| Area | Tools | Scope |
| --- | --- | --- |
| Identity/state | `get_info`, `get_state`, `get_state_page`, `get_capabilities` | Session/process identity, complete semantic state tokens, bounded native section pages, tracks, clips, sources, history and guarded context. |
| Result delivery | `get_result_page`, `release_result_receipt` | Immutable oversized native results, lossless bounded pages and explicit release before the next edit. |
| Projects | `list_profiles`, `new_project`, `open_project`, `save_document`, `set_project_profile`, `project_job_status`, `save_project` | Queued native lifecycle/profile conversion; separate new-copy save. Dirty documents are never silently discarded. |
| Custom profiles | `get_profile_template`, `get_custom_profiles`, `save_custom_profile`, `delete_custom_profile` | Native user profiles, exact rational fields, atomic save and file-hash guarded replacement/deletion; built-in/current/default profiles are protected. |
| Sources | `import_media`, `insert_media_clip`, `add_color_clip` | Local images/GIF/audio/video; exact single visual/audio stream source-range insertion; finite generated colors. |
| Source streams | `list_media_streams`, `configure_source_streams`, `configure_source_audio`, `insert_media_streams` | Actual native stream IDs, per-stream gain/normalization/channel settings and grouped video/multiple-audio insertion. |
| Transcoding | `list_transcode_profiles`, `start_transcode`, `transcode_status`, `cancel_transcode` | Native installed profiles; owned output/import jobs, source preservation and bounded cancellation. |
| Bin | `get_bin`, `edit_bin` | Paged folders/media/zones; create/move/name/description/rating/tags/subclips and unused-entry deletion. Source files are preserved. |
| Editing | `move_clip`, `cut_clip`, `trim_clip`, `slip_clip`, `delete_item`, `duplicate_clip` | Native media/group edits with source/effect/history preservation. Unsupported mixed/grouped combinations are errors. |
| Zones | `edit_timeline_zone`, `remove_gap`, `insert_zone` | Explicit-track lift/extract/space/gap removal and exact source insert/overwrite. |
| Groups/selection | `group_items`, `ungroup_items`, `set_selection`, `set_clips_enabled` | Explicit native IDs/group scope; selection is separate from editing. |
| Tracks | `add_track`, `delete_track`, `move_track`, `rename_track`, `set_track_locked`, `set_track_muted` | Native layout/name/lock/output state/history. |
| Speed | `set_clip_speed`, `set_time_remap_enabled`, `set_clip_time_map` | Forward/reverse speed and ramp/freeze/reverse frame maps with readback. |
| Sequences | `list_sequences`, `create_sequence`, `switch_sequence`, `rename_sequence`, `delete_sequence` | Deletion requires `acknowledgeHistoryReset:true`, clears ALL document history and refuses active/last/referenced targets. |
| Markers | `get_markers`, `edit_marker`, `edit_marker_category` | Paged guides/source markers, points/ranges and global categories. |
| Marker files/preferences | `inspect_marker_file`, `marker_file`, `get_marker_settings`, `edit_marker_settings` | Strict native JSON/text imports, new-file JSON/text/FFmetadata exports, and separately acknowledged native defaults/templates; settings are not timeline undo commands. |
| Effects | `asset_catalog`, `get_effect_stack`, `edit_effect`, `set_effect_keyframes` | Installed metadata; clip/track/master/bin stacks, parameters/order/enabling/zones and easing. Editable logical values and raw native parameters are separate. |
| Analysis | `start_effect_analysis`, `get_effect_analysis`, `cancel_effect_analysis` | Native effect Filterjob execution and result-aware status/cancel. This does not imply every other clip-analysis pipeline is covered. |
| Transitions | `get_compositions`, `edit_composition`, `edit_mix` | Cross-track compositions and same-track mixes, parameters, edit point and duration. |
| Titles | `create_title`, `get_title`, `format_title_text` | Editable native objects and bounded selected text formatting, gradients/outline/shadow, native title-hash guards and undo. Paragraph alignment/spacing apply to the whole text object; gradient reflow can require acknowledgment. |
| Subtitles | `get_subtitles`, `edit_subtitle`, `subtitle_file`, `subtitle_language` | Events/styles/layers, file import/export and native undoable language tracks. See persistence requirements below. |
| Convenience effects | `set_clip_transform`, `set_clip_audio` | Bounded native bitmap transform or gain/fades with readback/history. |
| Monitoring | `control_playback`, `extract_preview` | Project-monitor seek/play/pause/zone and exact detached-snapshot PNG extraction. Seek acceptance is not display completion. |
| Source monitor | `get_monitors`, `control_monitor`, `monitor_job_status`, `preview_source_frame` | Explicit monitor context, asynchronous exact-frame display provenance, source zones, stepping/looping and detached source previews. |
| Speech/text | `speech_capabilities`, `start_speech_job`, `speech_job_status`, `cancel_speech_job`, `get_speech_transcript`, `edit_speech_transcript` | Offline local prerequisites, word timestamps and native transcript metadata edits. Recognition requires matching scripts, Python packages and installed models. |
| Media jobs | `media_job_status`, `cancel_media_job`, `set_media_proxy`, `relink_media` | Jobs/nonblocking cancel, proxy request/readback and relinking. |
| Rendering | `list_render_presets`, `start_render`, `render_status`, `cancel_render` | Native avformat presets, image sequences/guide sections/audio stems, range/quality/bitrate/two-pass/proxy/subtitle options, session dependency queue, progress/cancel and non-replacing publication. |
| History | `undo`, `redo` | Shared document history, including manual edits. |

## Lifecycle, media and asynchronous work

`save_project` writes a new `.kdenlive` copy, refuses existing outputs and leaves
the live document URL/clean state alone. `save_document` performs normal native
Save/Save As, requires an explicit overwrite policy, and updates the live URL and
clean state on success. It retains native backup/cache/error decisions and its
job must be polled. Copy and normal-save publication have separate contracts.

Project jobs report `queued`, `running`, `needsUserAction`, `completed` or
`failed`. Loading progress windows retain a running status and expose
`progressUi`; a nested decision dialog still reports `needsUserAction`. Backup, missing-media, lock, conversion and other dialogs are explicit
decision points. Inspect them through interactive tools; queuing an operation
does not prove the document was opened, replaced or persisted.

Imports use the native asynchronous loader. Import acceptance creates a bin item;
it does not prove decoding finished. Poll readiness or inspect failure/unsupported
status before insertion. Typed intake accepts decodable local image/GIF/audio/
video content up to 64 GiB. Playlists/project XML, URLs and scripts are outside
that importer. Still images are bounded to 8-16384 pixels per side and 100 million
pixels. PNG source-in is zero; normal still-duration settings establish the
available native hold.

VFR/nonseekable/transcode-dependent imports remain insertion-gated and report
`needs_transcode`. Typed imports suppress the loader's conversion dialog while
retaining native metadata. Use `list_transcode_profiles` and `start_transcode`,
poll the owned job, and verify decoded output before placement. Public transcode
readiness includes decoded VFR/nonseekable state even if the native loader's
private request flag has already been consumed. Multistream insertion uses actual
MLT stream IDs from `list_media_streams`; automatic AVSplit applies only to an
exact video/audio pair, and larger selections use ordinary native groups.

Render and analysis starts return job IDs. Poll their status tools to terminal
and inspect actual output/results. Process exit, progress reaching 100 or a
widget changing is insufficient on its own. Cancellation distinguishes the
request from worker completion. Render cancellation addresses only owned
processes and remains available when the current project is not ready.

## Effects and animation

General effect tools address instance IDs from `get_effect_stack` and parameters
from the native editable schema. They do not accept arbitrary effect XML or
scripts. Hidden/read-only/action/job parameters require their native workflow;
an analysis button is not a numeric assignment. Third-party effects can require
additional installed runtime dependencies.

`set_effect_keyframes` uses complete ordered target-local keys and native easing.
The convenience transform retains 1-64 complete linear keys. Its
`x,y,width,height` are whole project pixels; opacity/rotation can be fractional.
The source bitmap, including transparent padding, maps directly to that rectangle
with caller-chosen aspect ratio and native center rotation. The convenience tool
updates only its session-owned transform and rejects conflicting active effects,
a disabled stack or changed forced filter zones.

`set_clip_audio` controls its own gain/fades rather than normalizing the whole
mix. Gain is -60 to +12 dB. Fade N spans N frame intervals and N+1 inclusive
endpoints; both spans total at most `durationFrames-1`. Other native effects stay
in the stack. Import, placement and effect application are separate undo steps.

## Subtitle language history and persistence

ASS imports stage and validate script metadata, embedded fonts, styles, layer
defaults and events before one native transaction. Undo restores the prior
metadata and events; redo retains the imported event IDs. Full-file validation
rejects malformed sections and late cues before creating subtitle state.

`subtitle_language` uses native undo for create, activate, rename and delete.
Language commands retain event IDs, groups, styles, layer state and snap points;
they do not clear document history. The former history-conflict restriction has
been removed after native history/save/reopen regressions passed. Live-editor
integration remains a separate validation requirement.

Current and prior saved-project subtitle assets must survive later live
undo/redo, Save As and discarded edits. Inspect both actual work files and
published ASS assets; checking only cached events or an export from the model is
insufficient. A language switch does not temporarily expose a locked subtitle
model as editable.

## Rich title and marker-file boundaries

`format_title_text` accepts selected UTF-16 ranges and preserves plain text,
other title objects, geometry and native metadata. Ranges cannot split surrogate
pairs or overlap. Native per-style gradients can merge or split their coordinate
regions; edits affecting unselected gradient text require
`acknowledgeGradientReflow:true`. Inspection and formatting budget each native
shadow raster before loading/updating title items. Legacy point-font conversion
and inline HTML resources remain guarded native workflows.

Marker imports use exact file hashes and validate all rows/categories/source
ranges before one native transaction. Text export follows Kdenlive's placeholder
order and is not necessarily reimportable for every template. Inspection pages
are bounded by encoded response bytes as well as count. Export checks placeholder
expansion before allocating it and publishes a new file without replacement.
Native defaults/templates use `expectedSettings` and an explicit acknowledgment;
changing them does not create a timeline undo entry. Used-category remapping is a
native whole-project transaction preserving marker IDs, range durations and snap
points across bin aliases and inactive sequences in one undo command. Pending
marker imports or unavailable models defer the operation before any changes.

## Local speech prerequisites

The speech tools do not install packages or download models. `speech_capabilities`
separates configured files, package metadata and completed recognition evidence.
A successful probe is insufficient to claim recognition works. Jobs accept one
local Audio/AV bin file with an explicit audio stream and inclusive frame range,
up to 30 minutes, with CPU inference. Matching bundled scripts consume prepared
mono PCM, and Whisper receives an existing model file to prevent automatic model
downloads. A build using older installed scripts reports that the script contract
needs updating.

Transcript edits change native bin text metadata with undo. Inspect the returned
source ranges and insert them through timeline tools to produce an edited video.
Whisper can translate supported multilingual models into English. Seamless,
sequence/playlist mixes and live capture require additional workflows and verified
prerequisites; their availability is not implied by the catalog.

## Interactive native controls

`list_actions` discovers main-window registered actions with their actual label,
enable/check state and ID. `trigger_action` queues an enabled action and returns
a UI job, not a claim that the intended feature finished.

`get_ui` inspects visible native Qt widgets and returns `uiToken`.
`set_ui_value` sets a recognized editable input; `activate_ui_control` invokes a
recognized activation or dialog rejection. Operations pass `expectedUi`, are
queued, and are inspected with `ui_job_status`. Tokens bind native editing
context/focus and control state. Secret inputs are redacted; validators, masks,
ranges, read-only and disabled states are checked.

`get_ui_model` pages a discovered list/tree/table. Pass its `expectedModel` to
`select_ui_rows` or item-view activation along with `expectedUi`. Sorting or
replacing same-count rows invalidates the model token. Control IDs are local to
the session and can expire.

UI jobs distinguish `pending`, `running`, `returned` and failure. A handler
returning is not semantic completion: inspect native state/jobs and final output.
Modal input is explicit. Other features reached through actions/dialogs remain
interactive reachability until their complete input/result/error/persistence
workflows are exercised. The separate `get_quick_ui`, `get_quick_model`, `set_quick_value`,
`activate_quick_control` and `quick_job_status` tools inspect bounded supported
QtQuick controls/models/menus with their own context tokens. Ordinary QWidget
inspection does not cover those surfaces. Neither interface executes arbitrary
Qt methods or properties.

## Remaining coverage requirements

There are substantive implementation gaps beyond checks not yet executed:

- Newly added stream, transcode, custom-profile, monitor, speech and advanced
  export contracts require execution evidence for each supported combination.
- Duplication includes native mix partners and grouped subtitles; comprehensive
  timing/effects/group/undo validation remains required. Time-map groups retain
  duration/type restrictions.
- The render dependency queue is session-local; persistence across application
  restart and the complete multichannel/container matrix remain unfinished.
- Rich title formatting, specialized analysis, multicam/alignment,
  interchange/archive, capture, external animation and online resources are partly
  or wholly interactive routes. Installed speech prerequisites remain separate.
- The supplied SDL lifetime repair passes isolated device/refcount regressions
  and owned GUI profile/document replacement with the SDL dummy audio driver.
  Physical audio-device behavior still requires the corresponding hardware.
- Full coverage depends on available plugins/codecs/models/devices/applications.
  Discovering an asset/action proves neither its prerequisites nor its output.

These remain completion requirements. They do not redefine the goal as the
subset that currently passes. A full-coverage claim needs current-source semantic
runtime evidence for every supported editor function. Counts of tools or action
names cannot substitute for that evidence.

## Verification and recovery

Relevant build targets and suites include:

```text
cmake --build <build-directory> --target kdenlive mcptest subtitlestest groupstest sequencetest markertest
ctest --test-dir <build-directory> -R "^(mcptest|subtitlestest|groupstest|sequencetest|markertest)$" --output-on-failure --no-tests=error
python -B tests/test_kdenlive_mcp_proxy.py -q
```

Tests exercise the native dispatcher/models, real media and selected rendered
images/audio/results. Test-source presence is not a passing run. Subtitle-core
changes require broader subtitle/group/sequence suites and actual work-file/
published-asset preservation checks. GUI integration must cover normal save,
reopen/resumption, profile conversion, modal decisions and export. An owned helper
can use isolated settings/cache and generated assets; original user projects are
not needed.

State inspection pages native models without fixed clip, source, track or bin-depth
limits. Tokens cover active and inactive sequence content, native notes, sources,
metadata, markers, groups and effects. Monitor position, scrolling and ordinary
playback telemetry do not invalidate editing tokens. Complete hashing of raw,
unbuilt playlists above 512 entries requires the additive MLT bulk-entry API;
builds without it explicitly report the missing prerequisite. Frames, individual
requests, effects, jobs and messages retain their advertised bounds. The connector is
sequential. Timeout/disconnection after send can leave an uncertain outcome; it
exits without reconnecting/replaying. Explicitly reconnect, read and reconcile
before editing again. Unrestricted simultaneous human/AI editing and arbitrary
scripting are not provided.
