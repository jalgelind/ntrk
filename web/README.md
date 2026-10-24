# ntrk module player

A static, fully Canvas2D player and small cell editor. The actual ntrk replayer,
importer and mixer run inside an AudioWorklet; the C++ core is unchanged.
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

Save uses `module_save` and downloads a complete `.ntrk`. Samples, extra FXPL
columns, instrument parameters, macros, mixer settings and editor metadata are
preserved, though only the four primary cell fields are editable here. No undo,
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

## Boundary and verification

`bridge.cc` builds natively and as standalone WASM, with fixed 128 MiB memory:
two file backing buffers, bounded conversion/scratch buffers, mutable primary
cells and a bounded 40 MiB effect arena. File/mixer validation precedes adopting
a candidate. The bridge owns all live state in the worklet; rendering allocates
nothing. Parsing, conversion and serialization run in message handlers and can
interrupt audio for large modules: pause before file operations.

`make test` includes the native bridge, importer, renderer/mixer and library
checks. `test-web` generates sampled eight-channel MOD/XM/PCM8/PCM16 fixtures,
sparse/wide/multi-column FXPL, meta-only empty and long-list fixtures. Checks
include refused-load preservation, exact save/reopen, bounds/defaults,
transport, effect memory, true channel IDs and finite/non-silent output.

With an existing Playwright/Chromium installation (no install step):

```sh
NTRK_PLAYWRIGHT=/absolute/path/to/node_modules/playwright \
  NTRK_SCREENSHOT=/tmp/ntrk-player node web/browser_test.mjs
```

The script owns and terminates its server/browser. It tests the actual worklet
and canvas at `/player/`, generic startup, optional/broken examples, sampled
local files/drop, burst keys and saved bytes, volume/real output levels, seek,
sparse IDs, modal help/discard, desktop/narrow rendering and CDP touch events on
long lists. Set `NTRK_WEB_ROOT` to an extracted bundle to test the actual ZIP.
Logs/screenshots belong outside Git. js-seq's `tools/tracker/check_web_audio.sh`
rebuilds WASM before comparing desktop/native bridge/WASM audio at 44.1/48/96 kHz
and removes temporary float audio files afterwards.

Requires WebAssembly, ES modules, Web Audio and AudioWorklet. Only Chromium is
automated; Firefox/Safari and physical mobile/audio devices need manual checks.
The compact bands, order rail, instrument list and fixed-pitch raster follow
js-seq; no2 informed the palette. No desktop/no2 code is linked into the host.
