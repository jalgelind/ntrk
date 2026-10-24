# Interactive runtime API

`ntrk_interactive.h` is a C++11 header over the ordinary mixer. Include it and
link the same mixer/effect translation units as `ntrk_mix.h` (or use
`ntrk_unity.h`). No allocator, thread library, JSON, host application, or strings
are needed. [FORMAT.md](FORMAT.md#imus--required-interactive-music-score-0x0012)
is the sole authority for score semantics and wire limits.

Off the audio thread, load with explicit preservation/schema permission and
call `interactive::RuntimeBinding::compile(module, generation, capabilities,
initial_section_id)`. This validates against the module, refuses opaque required
semantics and unsupported capabilities, and copies numerical records into fixed
tables. It sorts cues once. Optional future records remain in the borrowed file;
they are never decoded during rendering. Compilation allocates nothing. Failed
compilation invalidates that binding. Never recompile a published binding.

Keep the module, its complete backing storage, and the const binding at stable
addresses until the host acknowledges retirement. Generations are nonzero and
must never be reused. The host must publish a new generation for any change to
the module or payload, even when their addresses remain the same. Pointer/view
checks catch replaced views; they cannot detect mutation of supposedly immutable
borrowed storage. Parsing permission and `LinearPreview` are separate contracts.

Keep Runtime, Player and Mixer alive together on one audio owner. Quiesce and
call `unbind()` before reusing/destroying that state; never share one Player
between active runtimes. No destructor manages these borrowed lifetimes.

On the single audio owner, `Runtime::bind(binding, player, mixer, generation,
rate)` initializes held controls and gains once. An existing Player must name
that module, or have a null module. Initial section selection comes from the
compiled binding and is mandatory whenever sections exist. `render` adds into
interleaved output. Direct player render/skip/mixer calls cannot use this runtime's
permission: it is granted temporarily only within `Runtime::render` and checks
the exact Player, Module, payload view and generation. Ordinary and deliberate
linear-preview players retain their existing paths.

`control`, `layer`, `request_section` and `set_pause` take generation and strictly
increasing producer sequence. Failed commands do not consume sequence numbers.
Normalize host floats using `interactive::normalized`; it rejects nonfinite or
out-of-range values before integer conversion. `layer` takes an explicit fade
in milliseconds (use the authored fade when desired). Commands take effect at
the host's declared command boundary, before that render call's first frame.
Section requests cannot use a row boundary at the same continuous clock as
receipt. No section request implicitly starts a stopped runtime.

The wrapper observes `until_tick <= 0` and pre-tick `tick == 0`, never apparent
raw order/row changes during final-tick audio. It renders one frame at a new tick
so the mixer owns the pre-tick snapshot and authored FXPL evaluation; subsequent
runs are bounded by the actual remaining tick and `kMaxBlock`. Held macros are
queued before that first frame; the mixer applies them after authored cells.
Its optional frame-major host gain span has `kMaxChannels` stride, and multiplies
after the channel fader/insert, before both dry output and send taps. A null span
preserves the ordinary arithmetic. There is only one sequencer.

Entry restores score MIXR configuration, channel/command state and entry tempo.
Same-kind slot buffers survive `mixer_config_read`; carry never calls
`mixer_reset`. Loop wraps only replace order/row and traversal offset, retaining
voices, command state, tick residual, gain ramps and shared buffers. Stops cut
voices and command latches; carry renders only existing effects to the declared
bounded deadline, then clears them. No new authored row is processed during drain.

Pause freezes the entire musical renderer and its clock; queued layer values
coalesce until resume. Accepted rates are exactly 44100, 48000 and 96000 Hz.
`change_rate` requires quiescence, retains gain/target, converts remaining ramp
time, tick residual and tail deadline, and updates active voice rate state. The
host owns rate-dependent effect storage: rebind/reinitialize those buffers for
the new device rate before resuming. This clears their old-rate tails; seamless
resampling of effect buffers across device changes is not provided. Invalid rates
leave runtime state unchanged. A render rate mismatch is refused.

`position()` and `pop_cue()` are audio-owner APIs, not concurrent observation
methods. Cues carry generation, notification sequence, continuous unpaused frame
clock, traversal, section/ID/type/strength and offset within the current render
call. Internal feedback holds 256 cues; unread entries are never overwritten and
its drop counter saturates at `UINT32_MAX`. The host must forward these through
its own bounded transport. `Position.row_offset` describes the row whose audio
actually began, unlike the base Player's lookahead position.

The tracker host provides `InteractivePublication` to retain an immutable E1
Published snapshot plus its compiled binding off audio. `setInteractiveModule`
borrows those stable pointers through `moduleAdopted()`. Publish only after the
previous publication is acknowledged; retain both current and retiring owners
until then. No shared ownership operation or owned-buffer destruction occurs in
the callback. The publication itself must also remain at a stable address.

`InteractiveCommands` is one UI producer and one audio consumer: stage/coalesce
by kind/ID, then `flush()` in sequence order once per UI transaction. At most 50
pending identities and 128 queued commands are retained. A failed flush visibly
counts pressure and retains pending entries for an explicit later flush; it never
overwrites unread commands. The callback drains at most 128 commands. Stop and
reload use independent existing engine mailboxes/adoption, so input saturation
cannot prevent them. Stale commands are counted as refusals. Separate SPSC rings
forward 256 cues and 128 position snapshots; drain snapshots to the latest one.
`interactiveCueDrops()` and `interactivePositionDrops()` expose host ring pressure;
position snapshots also acknowledge the last accepted command sequence.
These APIs are available to later UI/host work; ordinary Play does not opt in.

Tests are procedural and require no sample files. The standalone mixer suite
covers runtime behavior, and tracker RT tests cover publication, queues, zero
allocation and compiler storage. `tools/tracker/check_interactive_runtime.sh`
runs the same C++11 suite under native and WASM and compares trace hashes and
check totals. `tools/tracker/check_web_audio.sh` checks the unchanged authored
demo through desktop/native bridge/WASM. Run both through the repository build slot.

## Measured budget (2026-10-24)

Apple native C++11 `-O2 -ffp-contract=off`; 2,000 calls of 512 stereo frames per
rate, 16 sounding PCM voices, held control, layer, two-row looping section and
256 simultaneous cues. Cue draining is included. Reproduce with
`tools/tracker/interactive_budget.cc`, built with `-Iexternal/ntrk`, through a
build slot; run `uptime` first. This measures the core wrapper/mixer, not device
or GUI overhead, and does not enable all expensive effects simultaneously.

| Rate | Median thread CPU µs | p99 thread CPU µs | Max thread CPU µs | Max wall µs |
|---|---:|---:|---:|---:|
| 44100 | 51.67 | 60.33 | 166.50 | 281.50 |
| 48000 | 51.17 | 53.29 | 59.46 | 64.04 |
| 96000 | 50.38 | 53.25 | 57.42 | 62.21 |

Load averages before this run were 14.26/18.41/16.03 (uptime over five days).
An earlier run at load 22.73/20.29/16.55 had wall maxima 2.52/4.88/8.29 ms:
those exceed the existing 2 ms callback budget and are recorded, not removed.
The subsequent thread CPU measurements separate work from scheduler preemption;
they do not establish an end-to-end device deadline guarantee under that load.
The callback budget remains 2 ms. Bindings occupy 5,896 bytes and runtime state
17,296 bytes on this build; neither allocates. Mixer state is 28,152 bytes plus
host-owned effect tanks (the tracker's pre-existing MixMemory allocation).
