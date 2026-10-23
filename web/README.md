# ntrk in the browser

A static tracker page: the actual ntrk replayer and mixer run inside an
AudioWorklet, with a single Canvas2D tracker and basic keyboard cell editor.
No desktop code, framework, CDN, service, filesystem shim or extra library is
linked. The C++ core is unchanged. Everything stays in the browser.

## Build and run

Use an existing Emscripten SDK (tested with 6.0.8), a C++11 compiler for the
native checks, and Python 3.8+ for demo staging and a local static server:

```sh
source ~/emsdk/emsdk_env.sh
make web
make test test-web
python3 -m http.server 8080 --bind 127.0.0.1 --directory out/web
# Open http://127.0.0.1:8080, then press Play.
# Stop the server with Ctrl-C when finished.
```

Alternatively, `make web EMXX=$HOME/emsdk/upstream/emscripten/em++`.
The target emits `out/web/{index.html,style.css,app.js,canvas.js,engine.js,worklet.js,ntrk.wasm,demo.json}`.
Copy that directory as a unit to any static host; HTTPS is required outside
localhost. Serve `.js` as JavaScript and `.wasm` as `application/wasm`.
No cross-origin isolation headers are needed. `file://` does not work.
`make clean` removes generated output. The SDK is a build tool, never a runtime
dependency; its version and compiler flags determine the generated WASM bytes.

To ship a host-owned song instead of the generated demo:

```sh
make web WEB_DEMO=/absolute/path/to/song.ntrk WEB_DEMO_NAME='Song name'
```

This copies the input to `out/web/demo.ntrk` and writes its identity to
`demo.json`. The name defaults to the input filename without its extension.
Startup and **Demo** load this same asset; failed configuration/song loads show
an error, with no generated-song fallback. A later plain `make web` explicitly
returns to the generated demo and removes the staged song. No parent repository
or music asset is required for the standalone build.

## Use

By default the original, generated **Night circuit** demo opens without
shipping a music asset. It has two 32-row patterns, a bass, kick and hat, with
only those three populated tracks shown. Press Play to resume the browser's
audio context through a user gesture. Pause holds voices and timing; Stop
silences tails and rewinds. Playback loops at the module's restart order.
The order/row display follows the sequencer with a 40 ms polling interval,
plus the browser/device's output latency. Channel headers toggle monitoring
mutes, which survive Stop but are not saved.

Click an order row to browse and seek; this turns Follow off. Enable Follow
to track playback again. Select a cell with the pointer or arrows, enable
**Edit**, and type notes using the piano keys listed in the page's
drawn **? Help** overlay (also F1). Choose an octave and instrument first, using the ± controls or the module-named instrument list. Instrument, effect and parameter
fields accept two hexadecimal digits; Enter in a note field writes note-off,
Delete clears a field. Successful entries advance one row. Note entry edits the
song; it does not audition a note while stopped.

Open reads `.ntrk` files locally, with a 16 MiB limit and transactional
validation: a rejected file leaves the current song intact. Save downloads a
complete `.ntrk` using `module_save`; reopening it preserves the edits.
Sample data, secondary effect planes, instrument parameters, macros, mixer
settings and editor metadata are carried through even though this small page
only edits the four primary cell fields. Files using unsupported format
versions are refused by the same loader as native ntrk. MOD/XM import,
instrument creation, undo, structural pattern/order editing and persistent
browser storage are outside this editor's scope. Track visibility comes from
primary note, instrument,
effect or parameter cells and channel FXPL data across all stored patterns,
including patterns outside the order list. Mixer gain/pan defaults and meta
lanes do not mark tracks used. The grid compacts used tracks while preserving
their original numbers and full stored channel data, regardless of visibility
settings from other editors. The visible list stays fixed until loading a file
or demo, so clearing a track's last cell leaves it editable. Reopening recomputes
the list and places the cursor on its first track. An empty song shows track 01.
Adding another unused track is outside this editor's scope.

All visible controls, lists, status, help and discard confirmation are drawn on one
canvas; HTML only hosts the canvas and hidden local file plumbing. The row gutter
stays pinned while channels scroll horizontally. Use the wheel for rows and
Shift + wheel for channels, or drag the drawn scrollbars. Touch-drag pans the
pattern. Orders and instruments have their own scroll regions. Narrow windows
stack the instrument list below the order list; channel widths stay fixed.
Rendering uses a dirty flag at browser refresh and a DPR-sized backing store.
Clicking controls keeps keyboard focus on the canvas. There are no native text
inputs or on-screen piano: a hardware keyboard is required for note editing.
The canvas currently provides only a host accessibility label; screen-reader
navigation of individual cells and controls is not implemented.

## Host boundary and checks

`bridge.cc` is platform-free C++ and is compiled both natively and to a
standalone WASM module. It owns two bounded file buffers, mutable primary cells,
the player, mixer and effect memory in a fixed 128 MiB WASM memory. It
initializes delay/reverb storage for
every configured insert, send and master slot at the audio context's rate
(8–96 kHz). Memory allocation, parsing and serialization happen only in worklet
message handlers, separate from `process()`. Loading/saving large modules can
still interrupt audio; pause before file operations for uninterrupted output.
Rendering uses preallocated buffers, and all edits and playback share one
worklet owner. The canvas host never touches live WASM state.

`make test-web` checks the same bridge natively: bounds and defaults, failed
load preservation, repeated save/load, note-off, transport/seek, mute, effect
memory, and finite non-silent generated audio. `make test` runs ntrk's existing
native suite. `make test-web` also generates sparse, wide and meta-only empty
module fixtures under `out/web-fixtures/` for the browser checks. With a local Playwright installation and Chromium, run the
browser smoke test (the script starts and terminates its own server/browser):

```sh
NTRK_PLAYWRIGHT=/absolute/path/to/node_modules/playwright node web/browser_test.mjs
```

It checks real worklet audio, editing, transport, order selection, mutes,
downloads and reopening, rejected files, canvas pixels/DPR, actual pointer hit
geometry, sparse channel IDs, automation-only tracks, empty songs, help, focus,
scrolling and narrow-window resizing. Test logs and
screenshots belong outside Git. The page requires a modern browser with
WebAssembly, ES modules, Web Audio and AudioWorklet. Only Chromium is covered
by the automated browser check; Safari/Firefox and mobile audio need manual
verification. There is no independent review verdict in these tests.

The compact header bands, order rail, instrument list and fixed-pitch raster follow
js-seq's tracker design; no2's tracker drawing and theme informed the palette;
the browser host here does not depend on no2. New web sources and generated
demo music are public domain / CC0, matching ntrk.
