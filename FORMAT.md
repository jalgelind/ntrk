# NTRK editor compatibility reference

The base remains version 2: 32-byte header, four-byte Note, fixed-width NAME,
and six-byte EDIT v1. Wire integers are little-endian. The detailed playback
layouts remain in README.md and the codecs in ntrk.h are authoritative.

## Section registry and resource bounds

| ID | Owner / meaning |
|---|---|
| 0x0001 | FXPL primary-channel effect columns and row-wide macro lanes |
| 0x0002 | NAME instrument names, 22 bytes per instrument; sole instrument-name writer |
| 0x0003 | SYNP synth parameters |
| 0x0004 | MACR macro table |
| 0x0005 | MIXR mixer configuration, all sixteen channel records |
| 0x0006 | TUNE grid and carried swing value |
| 0x0007..0x0009 | Reserved planned SECT/PARM/TRAK; not assigned by this change |
| 0x000A | SLIC sample slice offsets |
| 0x000B | SMUT arrangement slot mute masks |
| 0x000C | ITUN instrument tuning |
| 0x000D | EFXS required effect vocabulary |
| 0x000E | EDIT v1 hidden-channel mask; unchanged |
| 0x000F | IPAN instrument pan; always written critical, including centered pan |
| 0x0010 | Spent AUTO ID; never reuse |
| 0x0011 | META descriptive and private editor metadata; always optional |
| 0x0012 | IMUS required interactive score; schema/preservation only, no runtime |

Directory capacity is now **64 entries**, shared by known and unknown blocks.
The historical maximum was 32. Raising the bound permits any historical
32-block input to retain every optional block when a save adds mandatory IPAN
and currently known blocks. Older readers with the 32-entry resource cap refuse
larger directories. New inputs at the 64-entry cap can still require an additional
IPAN or other newly authored block: save explicitly refuses if it would exceed
the cap. No extra is discarded to make room. There is no separate 16-extra cap.
Unknown IDs must be unique, optional (flags zero), and carry a bounded payload;
unknown critical blocks still refuse. Size queries apply the same validation as
writes. Whole output length and directory offsets must fit u32; overflow, null
nonempty payloads, duplicate or known extension IDs and short output buffers
refuse before output is touched. Every successful size query reports the exact
required capacity; a short-buffer write also reports that capacity.

`Module::extensions` and `metadata` are bounded borrowed views into the input,
without allocation. Payload ID and bytes survive load/save unchanged; directory
order, absolute placement and unused padding are not retained. Payloads must be
self-contained and use payload-relative internal offsets. An editor **cannot
repair structural references inside unknown opaque payloads**: references to
pattern/order/channel/instrument IDs may become stale after structural editing.
Preserving bytes is not a guarantee of preserving such unknown relationships.
New extensions required to render the intended composition must be critical.
Historical FXPL and MIXR remain explicit optional-enhancement exceptions: a pure
replayer can skip FXPL embellishments and still play the base cells, while MIXR
is interpreted by the separate mixer and may be replaced by host configuration.
Consequently, skipping either can change audio; optional does not promise an
identical rendering across those capability levels. Their existing writer flags
are unchanged. META is strictly non-audio and has no such exception.

Borrowed input storage (order, notes, FXPL, samples, slices, META, IMUS and extensions)
must stay readable and unchanged throughout serialization. The output buffer
must not overlap any of that source storage: in-place saving is unsupported.
Use a separate output buffer, then replace the original. A loaded Module remains
a view whose input buffer must outlive every use of its borrowed pointers.

## META 0x0011 framing

Maximum 65,536 bytes, maximum 64 namespaces. Container: `u16 entry_count`,
`u16 reserved=0`, followed by entries, ending exactly at the payload boundary.
Each entry: `u8 namespace_bytes` (1..63), `u8 flags=0`, `u16 version`,
`u32 payload_bytes`, namespace bytes, payload bytes. Namespace characters are
ASCII `[a-z0-9._-]`; namespaces are unique and case-sensitive, without NUL.
`ntrk.*` is reserved for shared semantics, `nseq.*` for this application; third
parties should use a reverse-DNS namespace. Versions are namespace-specific.
The container has no implicit audio semantics and is never critical.

`meta_valid`, `meta_next` and `meta_encode` own this layout. Core validates the
container, lengths, names and uniqueness, but treats namespace payloads as opaque,
including unknown versions of known namespaces. It neither requires a future
namespace to use today's field grammar nor discards an unrecognised field.

Version 1 of the namespaces below uses field TLVs: `u16 tag`, `u16 flags=0`,
`u32 bytes`, data, repeated to the exact namespace boundary. Tags identify fields
uniquely; unknown tags are preserved verbatim. `meta_field_next` and
`meta_field_encode` are the shared codec. An editor refuses modification of a
future namespace version, malformed field framing or ambiguous duplicate target
tags, preserving the original payload. Core can still carry that namespace.
No implicit text conversion or geometry-based trimming occurs at save.

### ntrk.names version 1

| Tag | Meaning |
|---|---|
| 1 | Song title |
| 2 | Artist |
| 3 | Comments |
| 0x0100+c | Track name, zero-based capacity channel c=0..15 |
| 0x0200+p | Pattern name, zero-based sparse pattern ID p=0..255 |

Values are UTF-8, at most 4096 bytes each; NUL and ASCII controls except newline
and tab are not accepted by setters. Absent fields mean empty text. Clearing a
known field removes it; empty namespaces and an otherwise empty META are omitted
by the editor. A core-loaded explicitly empty container is retained. Instrument
names remain solely in NAME. Names outside active geometry are retained and may
be edited through the public model API; saving never truncates them. Shrinking
channels refuses removal of a named active track; future naming namespace
versions conservatively refuse shrinking because names cannot be inspected.

### nseq.tracker version 1

Private, non-audio state. Field 1 is document navigation: exactly eight bytes,
`u16 subfield_version=1`, `u16 order`, `u16 row`, `u16 channel`. It is pulled from
the UI on explicit Save as a non-mutating, best-effort metadata overlay and is a
remembered view, never playback position, transport, mixer or host settings.
The overlay never consumes history-owned metadata capacity. If its namespace or
subfield version is unsupported, its framing cannot be edited safely, or META
or the top-level directory would exceed capacity, the original metadata is
serialized unchanged instead. Optional navigation never blocks a valid song save
or prevents names undo. Even successful overlays leave the editor model unchanged.
Absent means the application's ordinary initial view; stored indices are advisory and require clamping if consumed.
Unknown namespace/subfield versions survive save and are not overwritten.
The current UI records this state without automatically restoring it on open.
No application settings or absolute paths are serialized. Future generator
recipes should store generator identity/version, seed, all inputs and provenance
needed for reproducibility; a seed alone cannot promise a reproducible result.
External source locations should use portable relative paths/URIs and content
hashes with explicit user consent; private absolute paths do not belong here.
No sample data or audio state belongs in this namespace.

## Active channels and editing

Header channels is **1..16**. Pattern and FXPL row strides use the actual count;
new songs may still start at sixteen. All sixteen MIXR channel records remain
capacity, including gain, explicit pan, sends and routing of inactive channels.
There is no implicit pan or auto-trimming of existing sixteen-channel songs.
MOD/XM imports retain their source counts and must render identically to their
previous zero-padded sixteen-channel representation.

EDIT and SMUT masks preserve **all sixteen bits**, including inactive bits.
Only active bits affect drawing/playback. Growing reactivates retained bits and
mixer records. Shrinking refuses removed notes (including unreferenced patterns),
any primary byte or FXPL byte, slot mutes, hidden state or names. It moves the
row-wide lanes after the new channel-lane stride. Mixer capacity is retained.
Geometry undo captures channels, rows, columns, notes, plane, hidden and slot
masks. Geometry changes refuse while player or audition voices read the module.
Above sixteen needs a new critical schema for masks/routing and is deferred.
Old fixed-sixteen validators refuse smaller songs; they do not misrender them.

## Editor use-case coverage

| Use case | Status / compatibility policy |
|---|---|
| Notes, note-off, effect commands | Supported, frozen four-byte cell; new audio vocabulary requires critical EFXS |
| Multiple effects and macros | Supported FXPL/MACR; geometry uses active channels |
| Instruments and synth recipes | Supported existing instrument/SYNP representation; instrument names only NAME |
| PCM samples, loops and slices | Supported existing blobs/SLIC; external media/provenance is extension-ready descriptive META |
| Tuning and pan | Supported ITUN/IPAN, instrument-owned centered defaults preserved |
| Mixer and routing | Supported MIXR fixed sixteen-record capacity; inactive records retained |
| Arrangement and slot mutes | Supported order list/SMUT; future named playback loops/sections deferred |
| Labels and descriptions | Supported title/artist/comments, track/pattern names; Tracks dialog exposes song/artist/track naming with undo |
| Editor UI state | Supported optional private navigation on explicit save; further layout state extension-ready |
| Generator reproducibility | Extension-ready private versioned recipe; full reproducibility schema and UI deferred |
| Data provenance and source paths | Extension-ready non-audio namespaces; prefer content hashes/portable paths, never private absolute paths |
| Polyphony per cell, multisample maps, point envelopes | Deferred; new semantics change sound and require critical schemas |
| Per-pattern lengths, swing rendering, named playback loops | Deferred; carried TUNE swing has no new rendering meaning |
| More than sixteen tracks | Deferred; masks and routing require a new critical schema |

EditModule owns and re-points metadata and extension buffers through adopt,
copy, move, undo and save; destroying a ModuleFile cannot invalidate its copies.
The player and mixer never interpret META. A host's choice of how to expose
editor metadata separately from audio snapshots is outside this format contract.

## IMUS — required interactive music score (`0x0012`)

**Format and codecs are implemented; interactive playback is not implemented.**
The v2 core header is unchanged. IDs `0x0007..0x0009` remain reserved and
`0x0010` remains spent. META (`0x0011`) stays strictly descriptive: it cannot
carry audio or gameplay semantics. IMUS has exactly one directory entry, whose
flags MUST be CRITICAL (`1`). It is not an optional hint or a linear fallback.
A legacy reader refuses the block; recognition alone is not playback support.

All integers below are unsigned little-endian, unaligned, with no padding.
The payload is at most 65,536 bytes. Its 12-byte header is:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u16 | container version, 1 |
| 2 | u16 | required profile, 1 |
| 4 | u32 | record count, at most 512 including unknown records |
| 8 | u32 | persisted next document ID, nonzero |

Each record has `u8 kind, u8 flags, u16 record_version, u32 body_bytes, u32 id`,
then exactly `body_bytes` bytes. Version and ID are nonzero. IDs are unique
across all kinds, including opaque records, and less than `next_id`. IDs are
stable through rename, reordering, save and undo. Allocate monotonically;
undo must retain the largest next-ID high-water mark. `UINT32_MAX` as next-ID
means allocation is exhausted; never wrap or reuse a deleted ID. Counts are
bounded at 32 controls, 16 layers, 64 sections and 256 cues, even for future
versions of those kinds. No trimming, clamping, truncating or reference repair
is implicit. The payload ends exactly at the last record.

Record flag bit 0 means required semantics; other bits are zero. A record
version or kind not understood is never decoded as v1. An unknown optional
record can be ignored by a supported-profile host but is preserved. Unknown
required semantics refuse supported-profile use. Future nonzero container
versions/profiles retain this framing envelope; only framing, IDs, resource
bounds and flags are checked, never their bodies interpreted as profile 1.
They can be opened only through explicit editor preservation.

Known v1 bodies begin with `u8 name_bytes` and that many UTF-8 bytes (1..63,
well-formed Unicode, no ASCII controls, DEL or NUL). Names are descriptive,
not references; duplicate names are allowed. The fixed fields below follow.
Trailing extension fields use repeated `u16 tag, u16 flags, u32 bytes, payload`.
Tags are nonzero; flag bit 0 means required, all other bits zero. No extension
tags are currently understood. Optional fields are preserved exactly. Required
fields make the score opaque to this implementation. Repeated extension tags
are allowed and their order is preserved. Truncated fields are malformed.

| Kind | Fixed fields after name | Bounds and defaults |
|---|---|---|
| 1 control | u8 macro_index; u16 default_value; u16 smoothing_ms | existing absolute MACR 1..32, normalized default 0..65535; smoothing MUST be 0 |
| 2 layer | u16 channel_mask; u16 default_gain; u16 fade_ms | nonempty active-channel mask, disjoint across layers; gain 0..65535; fade 0..60000 ms |
| 3 section | u16 start_order,start_row,end_order,end_row,entry_bpm,entry_speed; u8 quantization,end_action,tail_policy | bpm 32..255; speed 1..31; enums below |
| 4 cue | u32 section_id,row_offset; u16 type,strength | referenced section ID; row offset strictly within span; marker=1/strength 0, accent=2/normalized strength |

Defaults are explicit wire values, not omitted fields. Typed builder views use
control default 0, layer gain 65535/fade 0, section tempo 125/speed 6/row/stop/cut,
and marker strength 0; IDs, masks, names and nonempty spans still need authoring.
An empty container is legal and still requires explicit playback choice.

Controls require an FXPL plane and a nonempty absolute macro. SELF scopes and
inactive channel scopes are refused; ALL refers only to active channels.
Destinations must not overlap within or between named controls (global slot parameters
and master gain are global regardless of scope; insert parameters use channel scope). Scale and offset retain MACR's
signed Q8.8 semantics. Normalize with `(u32(value)*255+32767)/65535`; the
existing macro input is only 8-bit. A host initializes defaults before the first
row; latest accepted host update before a row wins. It retains current values
and reapplies them **after authored row cells** on every row. The current macro
input API is not itself that held-value evaluator. Profile 1 allows no smoothing
evaluator (`smoothing_ms=0`); row latency and byte quantization are explicit.

Layer gain is a separate host factor, multiplied with score channel gain before
routing into dry output and sends. It never overwrites authored automation or
SMUT/user mute state. Normalized gain is the wire u16 divided by 65535. At initial
binding only, initialize each layer's current gain and target to `default_gain`,
with no active ramp; channels outside masks use unity. Section transitions and
loop wraps preserve current gains, targets and ramps, including their progress.
They never reapply defaults. Unbinding discards that host state; a new binding
initializes defaults again.

A layer request becomes effective at the host's declared audio command boundary.
`fade_ms=0` sets current gain and target immediately before the first affected
output frame and cancels a prior ramp. For `fade_ms>0`, use wide arithmetic to
compute `N=ceil(fade_ms*sample_rate/1000)` at the current finite positive sample
rate. Starting from current normalized gain `g0`, affected frame `k` (1..N) uses
`g0+(target-g0)*k/N`, with frame N set exactly to target. Every channel in the
mask and both stereo samples use the same frame gain. A replacement request
starts from the current gain, without a discontinuity. This is per output frame,
not per sample or callback; block partitioning cannot change the result.

Pause freezes music sequencing, voices, musical effect state and ramp progress;
paused output frames do not consume ramp frames. Coalesce layer requests while
paused and apply them at the first resume command boundary, before its first
output frame. A host's output mute/fade is separate from these musical gains. On resume, continue from the frozen state,
without retroactive cue emission. Hosts must declare this pause behavior before
interactive binding. For a sample-rate change, quiesce rendering at a frame
boundary and preserve current gain/target; convert a running ramp's remaining
time `remaining_frames/old_rate` to `ceil(remaining_time*new_rate)` frames and
resume linear interpolation from current gain. This rounds completion by less
than one new-rate frame; never restart the full fade or restore defaults. Reject
invalid/unsupported rates before binding or resuming. Rate changes while paused
use the same conversion but do not advance the ramp.

A zero-gain layer keeps sequencing and voices running during unpaused rendering;
no new signal feeds sends but existing shared tails continue. The library does
**not** implement these ramps, rate-change or pause processing yet.

Section coordinates refer to order **occurrences**, not pattern IDs. Spans are
half-open lexicographic `[start,end)`: start order is in the order list, rows
are `<module.rows`, and `(order_count,0)` is the sole terminal endpoint. No
zero-length span. Quantization 0=row, 1=beat, 2=bar, using TUNE rows-per-beat/bar
and anchored to the **outgoing section's start**. A request takes effect at the
first eligible row boundary strictly after receipt; latest pending request wins.
The outgoing section end is always eligible, even off-grid. A request received
at a boundary cannot retroactively take that boundary. With no request,
end_action 0=stop, 1=loop. A loop wrap keeps the monotonic clock, voices and tails;
it does not perform a fresh section entry. Cues at row offset zero fire again
on the new traversal.

An interactive binding with any section records requires the host to supply an
initial stable section ID, which must resolve to an understood section. There
is no first-record or lowest-ID default; a missing, unknown or opaque selection
refuses binding. With no sections, a supported interactive host follows the
ordinary order/restart arrangement while applying controls and layers; cues
cannot exist without section references. LinearPreview always follows the
ordinary arrangement and ignores all interactive data, regardless of sections.

On an initial entry or a transition to another section, reset voice and tracker
channel/effect command memory, reload score MIXR state, set entry bpm/speed,
restore held controls, preserve current host layer gains/targets/ramp progress,
then process the destination row. `tail_policy` 0=cut clears mixer effect buffers;
1=carry retains mixer tail buffers while resetting score parameter/command state, and requires a host with
explicit carry capability. Stop terminates voices and clears tracker command
memory; cut clears effect buffers immediately, while a carry host drains effect
buffers to its bounded tail deadline. A host must declare support for all requested policies and these reset semantics
before binding playback. There is no implemented tail processor or binding in
this library yet; carry must never be advertised just because its byte decoded.

Profile 1 section spans require fixed timing: swing is zero and primary note
commands Bxx, Dxx, Fxx (including F00), E6x and EEx are refused inside spans.
FXPL is scanned too: its current player namespace allows only 0x30 accent and
0x31 glide; future commands 0x32..0x3f are refused there. Mixer commands are a
separate namespace, not ProTracker transport commands. Ordinary files remain
unrestricted. Section entry bpm/speed explicitly determine timing; no timing
state is inherited from an earlier order occurrence.

Cues are notifications, not scripts, implicit transitions or sound triggers.
Emit at row start, once per traversal, sorted by stable cue ID for simultaneous
cues. Their row offsets count rows across order occurrences from section start.
An interactive host needs a bounded, generation-tagged feedback queue; cue
sample offsets must be measured against its continuous audio clock.

`ntrk_imus.h` supplies borrowed container/record iterators, typed named views,
typed body encoding, raw-record container size-query/write, UTF-8/framing and
module-context validation. No allocation or format decoding belongs on the
audio thread. Pass a Module to codec validation for geometry, macro, reference
and score checks; module load/save always do so. All validation precedes output
writes; short buffers leave output unchanged and return required size. Input
and output storage must not overlap. Future bytes survive as bytes, not as v1
objects. Editors must atomically remap every known reference or refuse geometry
and semantic changes; opaque future semantics freeze unsafe edits.

`module_load()` refuses IMUS. `module_load_ex(..., EditorPreserve)` validates
framing, known semantics and retains opaque data; `InteractiveV1` requires the
supported schema, **not** playback permission. Both publish only validated views.
`Module::imus/imus_bytes` borrow the backing file. Save validates and preserves
them in a critical block; no export silently strips the score.

Default `player_start`, `render_add`, `mix::mixer_render_add` and skip paths
block required interactive data. No interactive interpreter is implemented.
The only playback opt-in currently provided is
`player_start_ex(..., PlaybackMode::LinearPreview)`: standard order/restart,
SMUT and MACR semantics, ignoring controls/layers/sections/cues, bound to the
chosen Module and IMUS view. Parsing permission never authorizes playback.
Callers must choose and label linear preview explicitly; ordinary APIs never
silently downgrade. Future runtime binding must be a separate capability and
module-generation contract with renderer backstops.
