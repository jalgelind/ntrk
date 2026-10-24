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

Borrowed input storage (order, notes, FXPL, samples, slices, META and extensions)
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
