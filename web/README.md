# ntrk in the browser

A static tracker page: the actual ntrk replayer and mixer run inside an
AudioWorklet, with an HTML pattern viewer and basic keyboard cell editor.
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
The target emits `out/web/{index.html,style.css,app.js,engine.js,worklet.js,ntrk.wasm,demo.json}`.
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
the remaining channels free for editing. Press Play to resume the browser's
audio context through a user gesture. Pause holds voices and timing; Stop
silences tails and rewinds. Playback loops at the module's restart order.
The order/row display follows the sequencer with a 40 ms polling interval,
plus the browser/device's output latency. Channel headers toggle monitoring
mutes, which survive Stop but are not saved.

Click an order button to browse and seek; this turns Follow off. Enable Follow
to track playback again. Select a cell with the pointer or arrows, check
**Record edits**, and type notes using the piano keys listed in the page's
help. Choose an octave and instrument first. Instrument, effect and parameter
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
browser storage are outside this editor's scope. All sixteen channels are
shown, including channels hidden by other editors.

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
worklet owner. The DOM never touches live WASM state.

`make test-web` checks the same bridge natively: bounds and defaults, failed
load preservation, repeated save/load, note-off, transport/seek, mute, effect
memory, and finite non-silent generated audio. `make test` runs ntrk's existing
native suite. With a local Playwright installation and Chromium, run the
browser smoke test (the script starts and terminates its own server/browser):

```sh
NTRK_PLAYWRIGHT=/absolute/path/to/node_modules/playwright node web/browser_test.mjs
```

It checks real worklet audio, editing, transport, order selection, mutes,
downloads and reopening, rejected files, and small-screen overflow. Test logs
and screenshots belong outside Git. The page requires a modern browser with
WebAssembly, ES modules, Web Audio and AudioWorklet. Only Chromium is covered
by the automated browser check; Safari/Firefox and mobile audio need manual
verification. There is no independent review verdict in these tests.

The visual and input conventions were informed by no2's web shell and tracker;
the browser host here does not depend on no2. New web sources and generated
demo music are public domain / CC0, matching ntrk.
