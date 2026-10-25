# ntrk module player

A static, fully Canvas2D player and small cell editor. The actual ntrk replayer,
importer and mixer run inside an AudioWorklet; the shared C++ core owns playback semantics.
Open/drop local modules first; **Example** is optional. No backend, CDN, account,
analytics, framework or new runtime dependency. Files stay in the browser.

## Build and bundle

Use an existing Emscripten SDK (tested with 6.0.8), a C++11 compiler and Python
3.8+. From the standalone ntrk checkout:

```sh
make web-dist EMXX=$HOME/emsdk/upstream/emscripten/em++
make test                       # includes test-web; no SDK needed for tests
python3 -m http.server 8080 --bind 127.0.0.1 --directory out/web
# Open http://127.0.0.1:8080/ ; terminate the server with Ctrl-C.
```

`out/ntrk-player.zip` is the upload artifact; `out/web/` is its runnable staging
directory. The ZIP contains only HTML/CSS/JS/WASM, example configuration, optional
example song, hosting instructions and a CC0 notice. ZIP order/timestamps/modes
are fixed: identical inputs and toolchain yield identical ZIP bytes. `make web`
stages the same player without packaging. All output is ignored by Git.

To include a host-owned song as an optional example:

```sh
make web-dist EMXX=$HOME/emsdk/upstream/emscripten/em++ \
  WEB_DEMO=/absolute/path/to/song.ntrk WEB_DEMO_NAME='Example name'
```

The name defaults to the input stem. This writes `demo.json` and `demo.ntrk`;
**nothing loads automatically**. Example verifies its SHA-256 and loads that exact asset or reports its
failure, with no silent fallback to another tune. Invalid/missing example
configuration never disables Open. A later plain `make web` or `make web-dist`
removes a previously staged example and selects the generated library example
(**Night circuit**) for the Example button. No parent checkout/music asset is
required. A missing `WEB_DEMO` source makes the build fail.

In js-seq, `make tracker-web` stages the authored song as the optional example;
`make tracker-web-dist` packages that configuration. Both still launch a generic
player. Source music is never modified or duplicated into the library checkout.

## Hosting

Unzip the artifact into any static directory, including `/player/`. Keep its
files together. HTTPS is required externally; localhost HTTP works. Serve `.js`
as JavaScript and `.wasm` as `application/wasm`. Use `Cache-Control: no-cache`
on the entry HTML and other assets; replace the bundle together. JS/CSS/WASM
references carry a content-derived build query, and example configuration/song
fetches bypass caches. A mismatched example digest is refused, never mislabeled. There is no service worker. `file://` does not work.
No cross-origin isolation headers are required. See bundled `HOSTING.txt`.

The host and generated library music are public domain / CC0. An optional
host-owned example retains its own rights: obtain permission before shipping it.

## Playback and editing

Open or drop `.ntrk`, ProTracker MOD or FastTracker XM files. Input, converted
output and import scratch each have a 16 MiB cap. Import uses the library API;
not every foreign tracker dialect/effect is supported, and conversion follows
ntrk's semantics rather than promising an exact original-replayer emulation.
Unsupported/truncated/over-budget files leave the current song and playback
intact. A converted `song.mod` or `song.xm` saves as `song.ntrk`.

Play resumes browser audio through a user gesture. Pause holds voices/timing;
Stop silences tails and rewinds playback. Order/row/BPM/speed are actual engine
state, polled every 40 ms, plus browser/device output latency. The song loops at
its restart order. Click an order to seek to its first row. Select a cell to
browse; **double-click a cell to seek to its row**, stopped or playing. These
operations disable Follow; enable it to follow playback again.

Output volume is 0–100%, initially unity, applied by a browser GainNode after
the unmodified engine. The level bar measures post-volume output peaks, with
red indicating values above unity. Volume and channel-header monitoring mutes
are host settings and are never saved in the module.

Enable Edit, select a cell, then use the keyboard. Successful entries advance
one row. Arrows/Page Up/Page Down/Home/End navigate. Tab leaves the canvas.
Use the module's instrument list and octave controls for note entry. The drawn
**? Help** overlay (F1) lists piano keys and two-digit hexadecimal entry for
instrument/effect/parameter. Enter writes note-off; Delete clears a field.
Notes are not auditioned while stopped. Edits, navigation, save and replacement
loads are queued in input order, so rapid typing is retained while the worklet
responds. Unsaved replacement has a drawn discard/cancel dialog.

The grid displays primary command mnemonics and parameters, every per-channel
FXPL column and every meta lane. Formatting/descriptions come from ntrk's note,
FXPL and macro APIs. Meta `M01` means macro 1; `00` is no invocation, not PAN.
Empty cells show dots; a valid command's zero parameter remains `00`. Arrows,
hit testing and scrolling use the same fixed character geometry and true channel
IDs. Only the displayed pattern is formatted (at most 256 rows ×149 command
cells, with a 512-entry text cache), fetched in 8-row windows so the audio
worklet renders between messages instead of formatting a whole pattern in one
handler call; selected descriptions are fetched lazily.
Order browsing and playback follow clear old command labels while fetching the
next pattern, and stale replies cannot replace a newer selection or loaded song.
Loads stay pending until acknowledged; a ten-second wait never leaves the old
song UI controlling a newly loaded engine.
Extra columns/meta lanes are labeled read-only when selected; typing there
cannot write primary bytes. The selected instrument shows its persisted pan.

Save uses `module_save` and downloads a complete `.ntrk`. Samples, extra FXPL
columns, instrument parameters, macros, mixer settings and editor metadata are
preserved. Exports add critical IPAN to protect the centered pan semantics from
older readers; older input files load absent instrument pans centered. Only the four primary cell fields are editable here. No undo,
instrument creation, structural song/pattern editing or persistent browser
storage. A hardware keyboard is needed for editing on touch screens.

Only tracks with primary cells or channel FXPL data in any stored pattern are
shown, including unreferenced patterns, with their true IDs. Default mixer
settings/meta lanes do not mark tracks used. The visible list stays fixed through
edits until the next load, and an empty song keeps track 01 editable. Full stored
channels survive saves. Adding an unused track is outside this editor's scope.

Wheel scrolls rows; Shift+wheel scrolls channels. Drag the drawn grid scrollbars
or touch-drag to pan. Orders/instruments scroll independently by wheel or touch,
even when a drag starts on a row; dragging does not select it. Narrow windows
stack these lists in a rail while preserving fixed cell pitch. Names clip to
the available width. Every visible control/status/dialog is drawn on one canvas;
HTML hosts only that canvas and hidden file plumbing. Canvas focus is retained
after pointer operations; cell-level screen-reader navigation is not implemented.

## Interactive scores

A file with a required interactive score (IMUS, see FORMAT.md) never plays as an
ordinary song. The ordinary bridge load still refuses it; the host then takes
the explicit interactive path, which binds the capability-bound runtime
([INTERACTIVE.md](../INTERACTIVE.md)) with held controls, layer ramps, safe
entry, cut/carry (2-second tail deadline), pause freeze and rate conversion.
Unsupported required profiles/capabilities and device rates other than
44.1/48/96 kHz are refused with their reason, leaving the current song intact.

While stopped, click a section in the **INTERACTIVE SCORE** panel to choose the
initial section; Play stays disabled until one is chosen (a score without
sections binds over its ordinary arrangement at load). While playing, a click
requests that section at its next row/beat/bar boundary; `→` marks the pending
request, `▶` the current one. Controls step held values in eighths and show the
byte actually applied at the latest row. Layer On/Off uses the authored fade and
shows current → target gain and remaining time. Pause freezes the score; Stop
cuts, and the next Play re-enters the chosen section with default controls and
layers. Cues list the latest markers with traversal and musical frame clock (not
speaker time). Interactive scores are read-only and cannot be seeked; Save keeps
their bytes. All shown values are audio-owned runtime feedback.

Commands are coalesced per control/layer (latest value wins; one pending section
request), at most 49 per batch and one batch in flight, tagged with the bound
generation and a strictly increasing sequence. Each load or section choice
compiles a new generation; commands for a replaced generation are refused as
stale and counted. The worklet applies a batch in its message handler, so it
takes effect before the next render quantum.
Score IDs are any u32 and cross the WASM boundary as exact numbers. Before
anything reaches WASM, IDs must be integers in u32 range, values finite in 0–1,
fades integer milliseconds up to 60,000 and generations/sequences positive safe
integers; a refused command changes nothing and consumes no sequence number.

## Boundary and verification

`bridge.cc` builds natively and as standalone WASM, with fixed 128 MiB memory:
two file backing buffers, bounded conversion/scratch buffers, mutable primary
cells and a bounded 40 MiB effect arena. File/mixer validation precedes adopting
a candidate. The bridge owns all live state in the worklet; rendering allocates
nothing. Parsing, conversion and serialization run in message handlers and can
interrupt audio for large modules: pause before file operations.
Interactive decode, validation, score description and binding compilation also
run in message handlers, never inside `process()`; the render call only renders
and the handler reads feedback afterwards. An AudioWorklet's handlers share the
audio rendering thread, and moving that work to another thread would need shared
memory (cross-origin isolation), which this host deliberately does not require.
The runtime binding and score table are fixed-size; a load retires the old
binding before its backing buffer is reused.

These handlers are measured audio interruptions, not glitch-free operations
(2026-10-25, Apple arm64, load averages 2.5–5.2). For the bundled procedural
score (2 channels, 8 records) WASM load takes about 0.5 ms and section choice
0.15 ms in node. For the largest-shaped procedural score (`interactive-large`:
16 channels, 256 orders × 256 rows, 8 FXPL columns, 64 whole-song sections, 16
layers, 256 cues; 91,756 bytes):

| Handler | native `-O2` | WASM in node (min/max of 3) | Chromium worklet round trip (3 runs) |
|---|---:|---:|---:|
| load (parse, compile check, describe) | 861 ms | 1,149 / 1,963 ms | 4,150–4,386 ms |
| describe alone | 286 ms | — | — |
| choose initial section | 594 ms | 766 / 774 ms | 2,177–3,118 ms |

The cost is IMUS span validation: every section re-checks every row of its span
across all channels and FXPL lanes (here 64 × 65,536 rows × 144 lanes), and it
runs on each full decode: three times per load (`module_load_ex`, the compile
check, describe) and twice per section choice (the refusal-preserving probe and
the published binding). Bounding it needs ntrk core changes, not host changes:
compile and describe from an already validated, generation-tagged view (making
section choice O(records)); validate each stored pattern row once rather than
once per overlapping section (bounded by pattern storage); or a resumable
validation cursor spread across messages, with Play withheld until it finishes.
The 8-row pattern window is likewise an interruption: 3.6–4.6 ms here, 9.1 ms
once under load, against a 2.67 ms quantum at 48 kHz.

`make test` includes the native bridge, importer, renderer/mixer and library
checks. `test-web` generates sampled eight-channel MOD/XM/PCM8/PCM16 fixtures,
sparse/wide/multi-column FXPL, meta-only empty, long-list, distinct-order and
maximal 16 MiB geometry fixtures. Checks
include refused-load preservation, exact save/reopen, bounds/defaults,
transport, effect memory, true channel IDs and finite/non-silent output.
A procedural interactive score (no sample files) is played through the bridge
and through direct runtime calls with one command script (layer fade, control,
section requests, pause, stop/restart); audio and cues must be identical, with
zero heap allocation across bridge commands/render/feedback. It also covers the
explicit initial section, sequencing/normalization, stale reload, unsupported
profile and rate, a section that stops at its end, section-less scores and
retirement by an ordinary load, plus record IDs at 0x7fffffff, 0x80000000,
0x80000003 and 0xfffffffe, refused aliasing/out-of-range IDs and durations, and
handler timing for the largest-shaped score. `engine_tests.mjs` replays the same script in
WASM against the native standalone-runtime output.

After building WASM, verify bounded pattern formatting, cache/payload limits and
exact large-file load/edit/save without an audio device:

```sh
node web/engine_tests.mjs out/web out/web-fixtures
```

With an existing Playwright/Chromium installation (no install step):

```sh
NTRK_PLAYWRIGHT=/absolute/path/to/node_modules/playwright \
  NTRK_SCREENSHOT=/tmp/ntrk-player node web/browser_test.mjs
```

The script owns and terminates its server/browser. It tests the actual worklet
and canvas at `/player/`, generic startup, optional/broken examples, sampled
local files/drop, burst keys and saved bytes, volume/real output levels, seek,
sparse IDs, modal help/discard, desktop/narrow rendering and CDP touch events on
long lists. With the interactive fixture it chooses the initial section, requests
another, fades a layer to measured silence and back, steps a control, and checks
cues, frozen pause clock, seek/edit/profile refusals and stale reload commands. It also delays and reverses pattern replies, replaces the song with
old replies pending, and holds a successful load acknowledgement beyond ten
seconds. Set `NTRK_WEB_ROOT` to an extracted bundle to test the actual ZIP.
Logs/screenshots belong outside Git. js-seq's `tools/tracker/check_web_audio.sh`
rebuilds WASM before comparing desktop/native bridge/WASM audio at 44.1/48/96 kHz
and removes temporary float audio files afterwards.

Requires WebAssembly, ES modules, Web Audio and AudioWorklet. Only Chromium is
automated; Firefox/Safari and physical mobile/audio devices need manual checks.
The compact bands, order rail, instrument list and fixed-pitch raster follow
js-seq; no2 informed the palette. No desktop/no2 code is linked into the host.
