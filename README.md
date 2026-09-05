# ntrk

A small tracker replayer, and a format for it.

The replayer is `ntrk.h`; the effects beside it are separate modules with their
own headers. See [Layout](#layout) for which of them is a header and which is a
translation unit, and why.

Header-only, dependency-free, platform-free. `<stddef.h>` and `<stdint.h>` are
the whole of what it includes: no libm, no allocation, no I/O, no threads, no
standard library containers, no exceptions, no RTTI. It renders a module into a
buffer you hand it and does nothing else.

```cpp
#include "ntrk.h"

ntrk::Module module;
if (!ntrk::module_load(&module, bytes, size))
  return;                          // a bad file is refused, never read past

ntrk::Player player;
ntrk::player_start(&player, &module);
ntrk::render_add(&player, buffer, frames, 2, 48000.f);
```

Three rules a caller has to keep:

- **The buffer must outlive the module.** A loaded module is a *view* over the
  file — patterns and sample data are pointed at in place rather than copied,
  which is why nothing here allocates and why unloading is forgetting.
- **`render_add` adds.** A tune can share an output with whatever else is
  playing. Zero the buffer yourself if it is yours alone.
- **Set a slot's parameters, then choose its kind** (mixer only). Every
  `Slot::param` is normalised 0..1 and the effect's own setter scales it;
  `slot_set_kind` takes what is there as the slot's *configuration*, which is
  what a pattern's automation is undone back to on a seek. A parameter written
  after the kind is one the first resync throws away — `slot_hold` is the escape
  hatch. See MACROS.md for reaching those parameters from a pattern.

## Layout

| File | Kind | What it is |
|---|---|---|
| `ntrk.h` | header | the replayer: loading, the player, `render_add` |
| `ntrk_types.h` | header | `Note`, `Instrument`, `Module`, `Channel`, `Player` |
| `ntrk_synth.h` | header | the SYNTH instrument: the drum kit and the 303 |
| `ntrk_303.h` | header | the 303's ladder: measured poles, realised as ZDF |
| `ntrk_dsp.h` | header | `flush_denorm` and `soft_clip`, defined once |
| `ntrk_fx_shape.h` | header | the waveshapers and the state-variable filter |
| `ntrk_fx_delay.h` / `.cc` | split | stereo delay |
| `ntrk_fx_reverb.h` / `.cc` | split | a Gardner diffuser into an eight-line FDN |
| `ntrk_mix.h` / `.cc` | split | slots, sends, the effect plane, the limiter |
| `ntrk_unity.h` | header | includes the `.cc` files, for a single-TU build |
| `ntrk_render.cc` | tool | renders a module to a WAV; not part of the library |
| `ntrk_gen.cc` | tool | writes the coverage module the cross-target check renders |
| `ntrk_bench.cc` | tool | what each layer costs; not part of the library |

**One rule decides which a module is: how often the compiler has to see through
the call.** Code that runs *per sample* — the SVF, the shapers, the replayer's
voice path, the synth voices — lives in a header so it inlines into the loop that
calls it, where a call would cost more than the arithmetic does. Code that
processes a *whole block* — the delay, the reverb, the mixer — lives in a
translation unit, because its one call is amortised over a bufferful of samples
and buys nothing by being visible.

The rule also explains `ntrk_dsp.h`. Those two primitives are per-sample, so they
stay inline, and one definition each rather than a guarded copy per module: a
`#ifndef` guard makes whichever header is reached first win, which is a silent
way for two copies edited apart to disagree by include order.

`ntrk_types.h` is why there is no third rule. The synth voices read a `Channel`
and an `Instrument`, and those structs are the bottom of the dependency order —
types, then voices, then the loader and player in `ntrk.h`. Splitting them out is
what lets every file here be an ordinary header.

**What a consumer includes depends on what it wants.**

- **The replayer alone.** `#include "ntrk.h"`, and that is all: it is header-only
  and carries `ntrk_types.h`, `ntrk_synth.h`, `ntrk_fx_shape.h` and `ntrk_dsp.h`
  with it. None of the `.cc` files is compiled and none of that code reaches the
  binary — which is what this project's own product build does.
- **The mixer or the effects, in one translation unit.** Include `ntrk_unity.h`
  from exactly one `.cc` of your own and compile nothing else — one compiler
  invocation, which is what `test_ntrk_fx.cc` and `test_ntrk_mix.cc` do.
- **The mixer or the effects, across several.** Include the plain `.h` files
  where you need the API and add `ntrk_fx_delay.cc`, `ntrk_fx_reverb.cc` and
  `ntrk_mix.cc` to your build.

**Do one of the last two, not both**: those definitions are not `inline`, so a
project that does both gets duplicate symbols at link time.

Two checks exist because their failures are invisible from inside this
directory. **A header that cannot be compiled on its own passes the whole
suite**, because every `.cc` here reaches the synth through `ntrk.h` —
`ntrk_synth.h` was once an include fragment valid at exactly one insertion point
and nothing reported it. And **a `.cc` that came to depend on something an
earlier include in the unity header dragged in** still builds the single-TU way
and fails for the first person to try the multi-TU one.
`make test-ntrk-hdr` and `make test-ntrk-fx-tu` compile each `.h` and each `.cc`
alone under `-Werror`, by wildcard, so a file added tomorrow is covered tomorrow.

## The reverb

`ntrk_fx_reverb` is a **Gardner diffuser feeding an eight-line feedback delay
network**: four series allpasses smear the input into noise, eight delay lines
mixed by an 8x8 Hadamard matrix carry it, each loop holds a two-band shelf and
Jot's decay gain, thirty-two taps read the output back out of those same lines,
and one short allpass per channel pulls left and right apart. It replaced a
Schroeder/Freeverb comb bank, and the reason is measurable rather than a matter
of taste.

**Echo density.** Abel and Huang's normalised echo density — the fraction of
impulse-response samples in a 20 ms window exceeding that window's standard
deviation, scaled so that 1.0 is a fully diffuse Gaussian tail — at size 0.6,
damping 0.2, 48 kHz:

| | 20 ms | 30 ms | 50 ms | 75 ms | 100 ms | reaches 0.9 |
|---|---|---|---|---|---|---|
| eight combs | 0.01 | 0.09 | 0.48 | 0.64 | 0.73 | never inside 1 s |
| diffuser + FDN | 0.64 | 0.78 | 0.90 | 0.97 | 0.98 | 42 ms |

**The difference is structural.** A comb bank puts no signal at all into the
output until its shortest comb has wrapped — 25.3 ms at 48 kHz, measured, and
silence before it. Here the tank is fed noise from the first sample and the taps
sit part way along each line, so the first wet sample is at 7.4 ms (size 0.5;
8.1 ms at 0.6) and the first 20 ms already holds a hundred-odd events above 1e-4
where the comb bank held none — which is what `test_ntrk_fx.cc` pins, because a
regression to a comb bank scores zero on it rather than merely less. The
impulse-response crest factor falls with it: 33.9–38.1 dB against the comb bank's
35.3–41.0 dB, the same fact measured as smoothness instead of as density.

**Modal density**, the total delay inside the recursion over the sample rate:
0.298 modes/Hz at size 0.5 and 0.446 at size 1.0, against 0.250 for the comb
bank, whose knob did not move it at all. That is the point of `size` moving the
delay lengths rather than only a feedback gain.

**RT60** at 48 kHz, damping 0.2, sizes 0.2 / 0.6 / 1.0: 0.61 s, 1.84 s, 5.09 s
— monotone, which is tested. The eight-knob `reverb_set_full` puts RT60 on its
own control, logarithmic from 0.15 s to 30 s, and Jot's normalisation makes it
independent of size: at a fixed decay the tail measures 2.04 / 1.97 / 1.93 s
across the whole size sweep. The five-knob `reverb_set` therefore *derives* a
decay from `size`, because a size knob whose tail did not move would not be
wired to the thing its label promises.

**A mixer slot reaches all eight**, decay at `param[5]`, LF damping at 6 and
diffusion at 7 — the three indices that were spare. Zero on any of them means
"as `reverb_derived` derives it from size", the same three values `reverb_set`
supplies, because zero is what every slot configured before those knobs existed
holds there. That is the same rule a cutoff of zero already followed, and it is
what makes the change inaudible for a module that does not drive them: the
cross-target fingerprints did not move.

**Level.** The wet path can still exceed full scale — attenuate at the send.
Peak / RMS against ten seconds of continuous full-scale noise at `mix = 1`,
written out rather than summarised because a single number here was wrong once
already:

| | damp 0.0 | damp 0.5 | damp 1.0 |
|---|---|---|---|
| size 0.0 | 1.70 / 0.34 | 1.34 / 0.25 | 1.20 / 0.23 |
| size 0.5 | 2.23 / 0.46 | 1.21 / 0.27 | 1.20 / 0.24 |
| size 1.0 | 3.69 / 0.74 | 1.43 / 0.30 | 1.23 / 0.27 |

Every corner is quieter than the comb bank's, whose worst was 4.23 / 0.90.

**What it costs.** 108 KB at 48 kHz with 100 ms of predelay, against the comb
bank's 127 KB — the FDN needs one set of lines rather than one per channel, so
the extra structure is paid for out of the saving. On the machine `make
bench-ntrk` was run on, a reverb send costs 2.09 ms per second of audio against
1.22, so 1.7x for the density above. That is 0.21% of real time.

**Two things it does not do**, both deliberate and both cheap to add later: the
delay lines are not modulated (the reference modulates the read pointers by
±0.08 samples at 0.4 Hz, which is what keeps a sparser FDN from sounding
metallic — this one is dense enough not to need it yet), and there is no
separate early-reflection tap list. See the `ponytail:` markers in
`ntrk_fx_reverb.cc`.

## Editing

These exist for an editor and nothing on the replay path calls any of them. See
`ROADMAP.md` for what is still missing.

```cpp
size_t need = 0;
ntrk::module_save(&module, nullptr, 0, &need);      // ask the size
ntrk::module_save(&module, buffer, cap, &need);     // then write it

ntrk::player_seek(&player, order, row);             // play from here
ntrk::player_preview(&player, ch, note, ins, rate); // audition one note
player.muted[2] = true;                             // mute a channel
```

**A `Module` can point at memory you are editing.** `patterns` is a
`const Note *` — const *pointer*, not const data — so an editor keeps its own
mutable arrays and points a `Module` at them.

**Cells, yes; counts, no.** Writing notes under a running player is safe by
construction — the player reads a cell and never remembers one. `rows`,
`channels`, `order_count` and `pattern_count` are what index the block, and the
player carries a position built from them between calls. Stop the transport
before any of them moves. `player_tick` clamps its position against the module it
is holding rather than trusting it, so breaking the rule costs wrong notes
instead of a read outside the pattern block — but the notes mean nothing, and it
is not a licence.

Two traps:

- **`player_seek` resets speed and tempo to the module's own.** Those are things
  a tune *sets* with Fxx as it plays, so where they stand at a given row is a
  function of every row before it — which seeking is precisely skipping. A tune
  that changes tempo part way plays at the wrong speed after a seek into it,
  until the next Fxx.
- **A pattern loop built on `player_seek` has to carry speed and bpm across the
  snap-back itself.** A loop has just played the rows that set them, so the reset
  above is wrong for it — and wrong every time round, which reads as a pattern
  that will not hold its own tempo. `audio_pump` in the tracker mode is the
  worked example; a loop inside the player would not need it, and does not exist
  yet.

`module_save` never allocates: pass a null buffer to be told the size, then call
again. It refuses exactly what `module_load` refuses, so anything it writes will
load, and it rebuilds the sample blob from what the instruments point at — so two
instruments sharing one buffer are written out twice.

### Describing a cell

Every command number has a *representation* and a *meaning*. Both are here rather
than in an editor, because otherwise every editor keeps its own copy of the
command table, and a copy that drifts from the format is exactly the failure a
format document cannot catch.

```cpp
char b[128];
ntrk::note_fx_repr(0xC, 0x40, b, sizeof b);          // "C40"
ntrk::note_fx_describe(0xC, 0x40, b, sizeof b);      // "Set volume 64"

ntrk::fxpl_repr(0x49, 0x80, b, sizeof b);            // "49 80"
ntrk::mix::fxpl_describe(&mixer, &module, ch, cell, b, sizeof b);
//   channel lane, a reverb in send 2:  "Send 2 (Reverb) Damping -> 0.50"
//   ...with no mixer to hand:          "Send 2 param 1 -> 0.50"
//   meta lane, cell {1, 128}:          "Macro 1 (absolute), input 128: ..."
```

`mix::fxpl_describe` is the one entry point for a plane cell, and **a negative
`channel` means a meta lane** — the whole discriminator, because a meta cell's
first byte is a macro index where a channel cell's is a command.

**The split across the two headers is the format's opacity, not tidiness.**
`param[0]` is delay time on a delay and drive on a shaper; the discriminator is
`Slot::kind`, which is runtime state no file carries. So `ntrk.h` describes what
the format fixes and `ntrk_mix.h` resolves a slot parameter against the kind
currently loaded. **A null `Mixer *` is legal** and degrades to the index form —
not a placeholder, but precisely what the format itself knows.

Nothing allocates, calls stdio or touches libm. Each writes into the caller's
buffer, truncates rather than overruns, always NUL-terminates, and returns the
characters written excluding that NUL — so a `cap` of zero writes nothing.

### Enumerating them

Describing answers "what does this cell say". Autocompletion asks the other
question — **what may go in this column** — and an editor that had to answer it
itself would keep its own list of the commands, which is the drift the describe
functions exist to prevent arriving one door along.

```cpp
ntrk::mix::CommandInfo ci;
for (int i = 0, n = ntrk::mix::command_count(Lane::PlaneMeta); i < n; ++i) {
  ntrk::mix::command_at(Lane::PlaneMeta, i, &mixer, &ci);
  // ci.mnemonic "S17", ci.name "Send 2 (Reverb) Diffusion"
}
ntrk::mix::command_lookup(Lane::NoteEffect, 0x08, nullptr, &ci);
ntrk::mix::command_value_text(&ci, 128, b, sizeof b);      // "centre"
```

Three lanes — the note effect column, an FXPL channel lane and an FXPL meta
lane — because **a command byte does not name its own column**: `0x08` is `8xx`
set panning in one and a resonance slide in the other.

**There is no static sentence per command and no min/max/unit**, both
deliberately. A hundred hand-written summaries would duplicate what the describe
functions already *generate*, and the copy is the half that goes stale;
`ParamShape` — continuous, a choice, two nibbles, or read by nothing — plus
`command_value_text` says what a byte is without pretending a nibble pair has a
unit. `Choice` is what lets an editor offer a dropdown, and its alternatives are
resolved: which four strings a slot's parameter 0 names depends on the kind
loaded in that slot, which is why `command_at` takes the mixer and degrades the
same way `fxpl_describe` does without one.

**Each command has exactly one row**, and the mnemonic tables *are* those rows —
`note_fx_mnemonic` and `fxpl_mnemonic` read the same table these enumerate, so
the name in a completion list and the mnemonic in the grid cannot come to mean
different commands. A meta lane's macro invocations are not in it: a macro's
name and targets belong to the module, and `macro_describe` is what reads one.

### Instrument parameters

The same question again, one struct along: **what may go in this field.** An
editor that answered it itself would keep its own table of bounds, and that
table has already been written and has already gone wrong — `transpose` was
narrowed to ±48 by the loader, the check turned out to be a guess and was
deleted, and three copies of the old number outlived it, one under a comment
claiming it was the format's.

```cpp
for (int id = 0; id < ntrk::instrument_param_count(); ++id) {
  if (ntrk::instrument_param_state(&ins, id) == ntrk::ParamState::Absent)
    continue;                       // not in this instrument's file at all
  const ntrk::InsParamInfo *p = ntrk::instrument_param_at(id);
  int lo, hi;
  ntrk::instrument_param_range(&ins, id, &lo, &hi);     // may be the sample's
  draw(p->name, ntrk::instrument_param_get(&ins, id), lo, hi);
}
ntrk::instrument_param_set(&ins, id, typed);            // clamps; never refuses
```

`ParamShape` is the command table's, unchanged — an instrument field is a
magnitude or one of a named list, and reusing the enum means one control draws
an FXPL filter-type dropdown and an instrument's. What the row adds is `lo`/`hi`,
which a command deliberately has not got: a command parameter is always a byte,
so a range would have said nothing, while an instrument field is an `int8`, a
`uint8`, a `uint16` or a flag bit.

**`instrument_param_set` clamps rather than rejects**, and that is the contract:
no sequence of calls can author an instrument `module_save` refuses. Three rows
carry a companion derivation, each copied from a rule the loader already applies
— a one-frame loop becomes a one-shot, a loop start moved forward shortens the
loop, switching the filter on carries a zero cutoff up to 20 Hz, and changing
the type to a built-in shape takes the generated geometry with it. The filter
one is the whole argument in miniature: the cutoff's 20..20000 applies only
behind its flag, so ticking a checkbox brings a value that was fine under a
bound that refuses it, and the module then plays all afternoon and will not
save.

**Three states, not two.** `Absent` means the format has no opinion — a SYNP
record exists only for a SYNTH instrument, a wave index only means something on
a built-in shape, a loop start means nothing without a loop — so an editor omits
the row. `Inert` means the byte is stored, range-checked and round-tripped but
the voice selected does not read it, so an editor **greys it and never hides
it**: hiding a hihat's `tune` loses the kick setting the moment someone
auditions a hihat and does not give it back. `instrument_param_state` answers
both, which is also what keeps `kSynthVoiceSpecs[ins.synth_voice]` out of
callers — that table has `kSynthDrumCount` rows and `synth_voice` reaches
`kBass`.

**`module_load` and `module_save` walk this table and spell no bound of their
own.** That is the point of it rather than a side effect: the two used to
duplicate the same six ranges, and the writer's comment admitted it twice. A
writer and a reader that disagree about a range produce a save nobody can open.

## Tests

```sh
c++ -std=c++11 -Wall -Wextra -ffp-contract=off -o /tmp/test_ntrk test_ntrk.cc && /tmp/test_ntrk

c++ -std=c++11 -Wall -Wextra -ffp-contract=off -O2 -I. -o /tmp/test_ntrk_fx test_ntrk_fx.cc && /tmp/test_ntrk_fx

c++ -std=c++11 -Wall -Wextra -ffp-contract=off -O2 -I. -o /tmp/test_ntrk_mix test_ntrk_mix.cc && /tmp/test_ntrk_mix
```

One compiler invocation each, no build system and no test framework. **This
passing on its own is the evidence that the library is standalone**, which a
suite run from inside a host project cannot give however green it is. The last
two reach the delay, the reverb and the mixer through `ntrk_unity.h`, which is
why each is still one invocation and why the `-I.` is needed.

The multi-TU route is checked from the Makefile instead, since proving that
several objects each compile alone is not something one command does:

```sh
make            # from third_party/ntrk — everything below
make test-ntrk test-ntrk-fx test-ntrk-fx-tu test-ntrk-hdr test-ntrk-import test-ntrk-mix
```

`test-ntrk-import` puts every `.mod` and `.xm` in `testdata/` through a
conversion and asserts the result loads and is not silence. Drop real modules in
there and they are checked; with none present the synthetic ones still run. A
file the importer *refuses* is printed and skipped rather than failing the run —
a directory of somebody's own modules may hold anything — but it is printed, so a
refusal cannot pass quietly.

**There is no reference implementation to lean on, so that file is the whole
check on the parsing**, and every branch is reached by a synthetic file built to
reach it — including a loop that truncates the file one byte at a time from the
end and requires a refusal at every one of them.

## Checking an import

`ntrk_render.cc` renders a module and says what came out. It takes a `.mod` or an
`.xm` as readily as a `.ntrk` and imports it on the way in — a two-step through a
converted file is one more place for the wrong bytes to be the ones under test:

```sh
c++ -std=c++11 -O2 -o ntrk_render ntrk_render.cc
./ntrk_render tune.ntrk 30            # report only
./ntrk_render tune.xm 30 tune.wav     # imported, then written as a WAV
./ntrk_render tune.ntrk 30 --mix      # ...through the mixer, not `render_add`
```

**`--mix` is what makes half of the effect plane audible.** The replayer cannot
hear a send: the `0x01`–`0x17` and `0x40`–`0xBF` commands, the per-channel filter
and every macro on a meta lane belong to the mixer, so a file carrying them
renders through `render_add` as though they were not written. Nothing is dialled
in from outside — the mixer is reset and left alone — so everything the flag
changes came out of the module. Without it the two paths agree, because the
mixer's `master_gain` defaults to the same 0.7 the player's `gain` does.

It deliberately alternates the render block size so tick boundaries land inside a
buffer as well as at its edge, because an audio callback does not hand over neat
multiples of anything.

**Test with real modules.** The synthetic ones in `test_ntrk.cc` exercise one
thing each and are no evidence that a tune somebody wrote plays correctly. Put a
few `.mod` files in `testdata/` — gitignored, because other people's music is not
this repository's to carry — and run `./ntrk_render testdata/tune.mod 60`. Two
things a real module can do that the unit tests cannot:

- **Run it under sanitizers.** The load path is a trust boundary and a real
  module is the input that actually exercises it:
  `c++ -std=c++11 -O1 -g -fsanitize=address,undefined -o ntrk_asan
  ntrk_render.cc`. It is one file, so this costs seconds.
- **Count the effects it uses.** A tune leaning on something in the unimplemented
  list plays wrong rather than failing, which is the one bug class no amount of
  rendering will announce.

Both Super Cars II tunes from JOTD's `supercars3` repository were used this way:
4-channel, 19 and 18 patterns, between them effects 0, 1, 2, 3, 4, A, C and F —
every one implemented. Address and UB sanitizers clean over a minute of each, and
two renders of the same module bit-identical.

## Checking the import against libopenmpt

`tools/check_ntrk_openmpt.sh` (in the parent project, since it needs a package
that is not in `DEPS`) compares this importer against **libopenmpt**, the
OpenMPT/ModPlug engine. **It is the only reference implementation this directory
has ever been held against.** `test_ntrk_import.cc` builds a synthetic file to
reach every branch, which proves the parser is not read past — not that it read
the right thing; and `ntrk_render` proves a tune plays, not that it plays what
was written. OpenMPT has been fed every module on the internet for twenty-five
years, so its cell table is a fact about the file in a way nothing in here can
be.

```sh
brew install libopenmpt              # or apt install libopenmpt-dev
tools/check_ntrk_openmpt.sh          # every module in testdata/
tools/check_ntrk_openmpt.sh a.xm     # or just these
```

It skips with a message where libopenmpt is absent, and where `testdata/` is
empty — which is the normal state of a fresh checkout. Two levels:

**Exact: every `(pattern, row, channel)` cell.** Not raw equality, because the
importer transforms as it converts. Each deliberate divergence is a named
category — `xm.note-shift`, `xm.volcol->effect`, `xm.global-volume-bake`,
`xm.pad-break` and the rest — and the counts are printed even on a pass, because
**a category far commoner than its description predicts is a finding too**.
Anything outside them is printed with its position and fails the run.

The per-instrument note shift is the one thing checked against the file rather
than against libopenmpt: `relative_note` is read out of the XM instrument block
by the tool itself, because deriving the shift from the importer's own output
would hide any consistent error in it. The residue must be whole octaves.

**Approximate: the shape of the audio.** Both renders at 48 kHz, compared as a
normalised RMS **energy** envelope over 50 ms windows — energy rather than the
mono sum, so the comparison measures what was played rather than the two panning
laws against each other. Duration, and correlation in eighths **each at its own
best alignment**, with the alignments printed beside the scores. Sample-exactness
is unreachable and is not the goal; a wrong tempo, a mishandled `Bxx`/`Dxx`/`EEx`
or a tune that ends early are, and those are what this sees.

What it found, over five `.mod` and three `.xm`:

| | cells | exact | audio, at lag zero |
|---|---|---|---|
| `.mod` (5 files) | 18944 | **18943** | 0.96 – 0.998, and see the tempo note |
| `.xm` (3 files) | 71504 | 58353 + 13151 categorised | 0.55 – 0.98 |

**No unexplained cell in either format**, and the one `.mod` cell that is not
exact is libopenmpt's: a `D02` in the file that it reports as `D00`, where ntrk
stores what is written. The tool re-reads the `.mod` cell out of the file to say
so rather than trusting either reader.

**The riskiest transform came out right.** `brothomstates-4b33.xm` has patterns
of 20, 20, 22, 20, 40, 20 and 4 rows, all padded to 40 and closed with a `D00` —
which is why 48% of its cells are padding — and its render still ends within
0.08 s of libopenmpt's. Six patterns stretched to twice or ten times their
length reproduce the tune's structure exactly, which is the claim the padding
rests on and was never measured before.

The script also **checks itself**: it hands libopenmpt a copy of one `.mod` with
a single cell changed and requires the comparison to name that cell. Every real
file passes, so nothing else exercises the reporting path, and a comparison that
cannot fail means nothing.

Two things the numbers mean, both worth knowing before reading them as bugs:

- **A lag that grows linearly is libopenmpt's tick, not ours.** It renders a
  tick as a whole number of output samples; this player keeps the fraction in a
  double. Where `48000 * 2.5 / bpm` is not an integer the two slide apart — at
  148 bpm it is 810.81 against 810, which is a tenth of a percent, a millisecond
  a second, and a hundred milliseconds across `jogeir-last_v8.mod`. At 125 bpm
  it is 960 exactly and the lag is zero. Aligned, that tune's eighths score 0.77
  to 0.99; at lag zero the same tune scores 0.39, which is the drift and nothing
  else.
- **The `.xm` audio gap is the volume envelopes.** Six of eleven instruments in
  `brothomstates-4b33.xm` carry one, seven of fourteen in `Jallabert`, eleven of
  seventeen in `radix` — and they are not imported. Playing the same files
  through libopenmpt with the envelope bit cleared moves the correlation from
  0.55 to 0.89, 0.975 to 0.982 and 0.76 to 0.81. The tool counts them, along with
  ping-pong loops and multi-sample instruments, so the number can be read
  against what the file actually asks for.

### Fuzzing it

```sh
tools/check_ntrk_openmpt.sh --fuzz 1000
```

Mutates the seed modules — a few random bytes, biased at the front of the file
where the counts and offsets live, and sometimes a truncation — and runs the
same comparison on the wreckage, with the tool rebuilt under the address and
undefined-behaviour sanitizers. **libopenmpt is what makes this a differential
fuzzer rather than a crash fuzzer**: on a mutant it still opens, the two cell
tables must *still* agree, so a branch reached only through a malformed header
is a named difference and not merely an absence of a crash. The exit code says
which: 42 a sanitizer report, 1 a cell disagreement, 0 fine — including the
common case of one or both readers refusing the file.

**No sanitizer report and no importer bug in 3000 mutants**, over three
seeds. What it did find was
four bugs in the *comparison*, each a place this file's model of the importer was
wrong in a way no well-formed module reaches:

- a `.mod` sample number above 31 becomes "no instrument", and a cell with no
  instrument leaves the channel on its previous one — so tracking libopenmpt's
  masked number diverges from the importer;
- an `.xm` effect that is dropped leaves the slot **free**, and the volume column
  then takes it — `xm_effect_for` tests `e == 0 && p == 0` after the effect, not
  instead of it;
- libopenmpt reports an XM `Dxx` parameter BCD-decoded where ntrk keeps the byte
  and decodes it in `player_tick` — the same row, two representations;
- XM's volume-column panning is `(byte - 0xC0) * 4` on libopenmpt's side, so the
  *nibble* has to be recovered rather than the two ranges rescaled onto each
  other; rescaling rounds hard right down a step.

The `.mod` note check is stronger for it: ProTracker's period table is now in the
tool, so a period that *is* an exact entry must produce that note on both sides,
and only a period between two entries may differ by a semitone.

One thing about the importer did come out of it, and it is cosmetic:
**`xm_scale_pattern` writes into the padding.** It walks the padded height, so a
channel whose level needs restating gets a `Cxx` written into rows after the
`D00` that only exist to fill the pattern out — bytes in the file that nothing
plays. No file in `testdata/` reaches it, because the one that uses `Gxx` has no
pattern shorter than the rest; it took a fuzzed `G` in a module with short
patterns. Worth a bound on the loop if the bake is ever touched again.

`NTRK_FUZZ_SEED=7 tools/check_ntrk_openmpt.sh --fuzz 1500` for a different set;
the default seed is fixed so a failure is reproducible.

## The cross-target check

`tools/check_ntrk_crosstarget.sh` (in the parent project, since it needs emsdk)
renders the same bytes with the native and the wasm build and requires the two
fingerprints to be equal. **Every libm decision in this directory is justified by
"ARM64 and wasm compute identical floats", and this is the only thing that turns
that from an argument into a measurement.**

It checks **two** modules, and the second is why `ntrk_gen.cc` exists:

- `assets/music/circuit.ntrk` — a `.mod` import, and so a `.mod`-shaped program:
  sample playback and the sixteen classic effects, and nothing else.
- a module written by `ntrk_gen`, rendered `--mix` — the 303 with accents and
  glides, two drum voices, a wavetable, an envelope, a per-voice filter, two
  effect columns, a meta lane and two macros. That is the newest and most
  table-driven arithmetic here, and until this module existed none of it was
  covered.

**The coverage module is generated once, natively, and then handed to both
targets.** Generating it separately on each would compare two copies of the
generator rather than two renderers. Its hash is compared only against the other
target's and never against a stored value, so editing `ntrk_gen.cc` costs nothing
to re-bless.

## What it costs

`ntrk_bench.cc` measures each layer separately, so the price of one is
attributable: the replayer alone, the mixer with nothing switched on, each effect
on a send, per-channel inserts, the FXPL plane against the same module without
it, each synth voice against a PCM instrument, and a realistic everything-on
configuration. It builds its modules in memory, so it needs nothing from
`testdata/`, and reports the **real-time factor** as a percentage — the form a
budget is actually spent in. The median of nine runs, so one scheduler hiccup
cannot move a number.

```sh
c++ -std=c++11 -Wall -Wextra -ffp-contract=off -O2 -I. -o ntrk_bench ntrk_bench.cc
./ntrk_bench

make bench-ntrk                     # from the project root, the same thing
```

**`-O2` or nothing.** Every per-sample thing here is a header precisely so it
inlines into the loop that calls it (see [Layout](#layout)); at `-O0` none of it
does, and a debug build's figures describe a program that is never shipped.

Two limits on what a run tells you:

- **One machine, one compiler, one set of flags.** A number here is comparable
  against another number here and against nothing else. The header line prints
  the sample rate, the block cap and the architecture for that reason.
- **The x86 denormal cliff does not reproduce on Apple Silicon** (ROADMAP C3), so
  a reverb tail measured on ARM is a *lower bound* for x86 rather than an answer
  for it. The one cost this cannot settle is the one the denormal work exists for.

Deliberately not part of `make test`: a timing check in a correctness suite is a
slow test that fails on a busy machine.

## The format

Little-endian throughout, `.ntrk`, positional blocks in this order and then an
optional directory. Every count is a `u16`, so nothing in the layout can overflow
a `size_t`. **`module_load` and `module_save` in `ntrk.h` are normative**; this
is a summary, and where the two disagree the code is right.

| Block | Size | Contents |
|---|---|---|
| header | 32 | see below |
| orders | `order_count` | one `u8` pattern index each |
| instruments | `instrument_count * 32` | see below |
| patterns | `pattern_count * rows * channels * 4` | cells, row-major |
| samples | `blob_bytes` | signed PCM, concatenated |
| directory | `block_count * 12` | `u16` id, `u16` flags, `u32` offset, `u32` bytes |
| payloads | — | one per directory entry, in the directory's order |

**Header**, at offset 0: `"NTRK"`, `u16` version (**2**), channels (1..16), rows
per pattern (1..256), speed (1..31), bpm (32..255), order count, pattern count,
instrument count (0..64), restart order; then `u32` `blob_bytes` at 22, `u16`
`block_count` at 26, `u8` `note_max` (36 or 96) at 28, `u8` flags at 29 and a
`u16` reserved at 30. The last two must be zero. Every byte of the 32 is spent,
so **a header change is version 3** — anything else that wants room goes in the
directory.

**There is one version and the byte is 2, not 1.** A version 1 existed: the same
magic with a 20-byte instrument entry, no `blob_bytes`, no directory, 8 channels,
32 instruments and no note above 36. Every version-1 file was converted; the
reader refuses them. **Renumbering this format to 1 is the one change that must
not be made**: every one of those files would then pass the version check and be
read at a 20-byte stride against today's 32 — garbage that loads, instead of a
clean "too old".

**Instrument**, 32 bytes: `u32` offset into the sample blob, `u32` length in
frames, `u32` loop start, `u32` loop length (0 means one-shot), `u8` volume
(0..64), `i8` finetune (-8..7), `u8` type, `u8` flags, `u16` attack / decay /
release in ms, `u8` sustain (0..64), `i8` transpose (-48..48), `u16` filter
cutoff in Hz, `u8` filter resonance, `u8` built-in wave index. An imported `.mod`
leaves everything from `type` on at zero, which is PCM8 with no envelope, no
filter and no transpose — so every feature is a flag test rather than a test of
what kind of module this is.

**The ADSR applies to a SYNTH instrument like any other**, because it lives one
layer above the voice: `channel_gain_apply` runs it over whatever
`channel_sample` produced. That is what a 303 gate is made of — attack 0, decay
0, sustain 64, and a release of some tens of milliseconds. Sustain 64 is a level
of exactly 1.0 and `out * 1.0f` is exact, so a held note renders bit for bit
what it did with no envelope at all; the release is the whole of the difference,
and without it `^^^` on a synth is a hard cut in the middle of a cycle.

**Cell**, 4 bytes: `u8` note (0 for none, 97 for note-off, else 1..`note_max`),
`u8` instrument (0 for none, else 1-based), `u8` effect (0..15), `u8` parameter.
**The cell is four bytes and cannot grow**: a `Module` is a view over the
caller's bytes with no allocation anywhere, so widening it would need a
conversion pass that cannot exist. New per-cell commands go in the FXPL block's
parallel plane instead — which is forced too, since all sixteen top-level effect
numbers are taken.

**Directory blocks**: `0x0001` FXPL (the second effect plane, with a `u16`
`fx_columns` / `u16` `meta_columns` prefix), `0x0003` SYNP (one parameter record
per SYNTH instrument), `0x0004` MACR (the macro table), `0x0005` MIXR (the
mixer's configuration — see below). `0x0002` NAME and `0x0010` AUTO are reserved
and stay spent rather than reused. Bit 0 of an entry's
flags marks it **critical**: an id the reader does not know is skipped when
optional and refuses the file when critical, which is how this reader stays
honest against one written by a later version.

**MIXR carries what the effect plane cannot say.** Every level, pan, filter and
slot *parameter* is an FXPL command and has always been saved as automation; a
slot's *kind* is deliberately not one — a row that swapped a shaper for a reverb
would point the tank at memory the caller laid out for something else — and the
stereo width has no command at all. So the block is 98 fixed bytes: `u16`
version, `u16` slots (5), `u16` master gain and `u16` width in Q12, then per
slot a `u8` kind, a reserved byte and eight `u16` parameters normalised 0..1.
The five slots are the four sends and then the master; inserts are per channel
and are not in it.

It is **optional**, and `ntrk.h` never reads a byte of it — a mixer's
configuration is not something a replayer can act on, so the format's own layer
still does not know what a send is. `ntrk_mix` decodes it, through
`mixer_config_read` / `mixer_config_write`, and what those write is `Slot::base`
rather than `Slot::param`: the setting automation departs from, not wherever a
slide happened to leave it. A module gains the block only when something puts
one there, so a tune opened and written back is still the same file.

A note is an index rather than an Amiga period because the period is derivable
and the index is not ambiguous about finetune. The player converts, and holds
periods internally, because the effects move periods — portamento and vibrato
step through a table, and a semitone is not a fixed number of Hz.

### Why not just play `.mod`

`ntrk_import` converts a ProTracker module into this, and `ntrk_render` will do
it on the way in. Everything 1988 put in a `.mod` that a player should not be
doing at load — big-endian fields, lengths in words, periods instead of notes,
thirty-one instrument slots whether or not the tune uses them — is undone once,
at build time. Dropping the trailing unused instrument slots is 600 bytes off a
file that ships over a network. The importer **re-reads its own output applying
the player's validation rules**, so a converter bug is an error with a line
number rather than a tune that silently does not play.

It reads the 31-instrument variants — M.K., M!K!, FLT4, 4CHN, 6CHN, 8CHN. **The
15-instrument format that predates them has no signature to check and is not
read**, deliberately: guessing at a headerless file is how an importer silently
produces noise.

The usual route for *new* music is still a tracker — write it, export a `.mod`,
import it. That is why the importer exists instead of an authoring format of our
own invention that no editor can write.

### The same conversion, at runtime

`ntrk_import.h` is the importer as a library, so the desktop player, the tracker
editor and the wasm build all open a `.mod` the same way:

```c++
const size_t need = ntrk::import_scratch_needed(data, size);
size_t written = 0;
if (need != 0 && need <= sizeof(scratch) &&
    ntrk::import_convert(data, size, scratch, sizeof(scratch),
                         out, sizeof(out), &written))
  ntrk::module_load(&module, out, written);
```

Three properties, and each one is load-bearing:

- **Bytes in, bytes out — no filesystem, no allocation, no stdio, no libm.**
  There is no filesystem in a browser, so a converter that wanted one would work
  in exactly two of the three places it is needed.
- **The output goes through `module_save`**, never through bytes written by hand.
  One writer of the format: an importer that emitted `.ntrk` itself would be a
  second, and the two would drift the first time the layout moved.
- **A `.mod`'s samples are not copied.** They are 8-bit signed, which is what
  PCM8 already is, so the instruments point straight into the input buffer and
  `module_save` writes the blob out from there. Scratch is only for the cells. An
  `.xm`'s samples *are* copied, and that is the one structural difference: XM
  sample data is delta encoded and the input is `const`, so the frames are
  accumulated into scratch and the instruments point at that.

`.s3m` and `.it` slot in behind the same `import_detect` / `import_convert` pair
without callers changing.

### FastTracker 2 `.xm`

The same three functions read an `.xm`. What does not cross over cleanly is worth
knowing before you pick a tune.

**The note space moves down three octaves.** An XM sample with
`relative_note == 0` plays at its recorded rate on note 49; a ProTracker sample
does so on period 428, which is ntrk's note 13. So 36 comes off every note and
the sample's `relative_note` goes on — folded into the cells rather than into
`Instrument::transpose`, because **no player here reads that field**. It is
written, validated and round-tripped, and then ignored. An instrument whose notes
will not fit 1..96 after the shift is moved by whole octaves until its span does;
one tune in the corpus lands at note −11 without it.

The residue is a **uniform 14 cents flat** (worst 18, best 10): the Amiga clock
puts period 428 at 8287 Hz where XM puts note 49 at 8363, and the spread is
ProTracker's period table, which is not exactly equal tempered. It is the tuning
every `.mod` here already has and the tune stays in tune with itself. The
**linear frequency flag is not read at all** — both of XM's tables put note *n*
at the same semitone, and where they differ is inside a portamento.

**One row count for the module, and XM has one per pattern.** The longest sets
`rows`; the short ones are padded and given a `D00` on their last row, in the
first channel with no effect of its own. Stretching a four-row pattern to forty
instead would be thirty-six rows of silence mid-tune.

**The volume column becomes an effect when the effect column is empty**, which is
most of it — one tune's entire dynamic range is the volume column. Four ranges
map exactly: `0x10..0x50` set volume is `Cxx`, `0x60`/`0x70` volume slide down
and up are `Axy`, `0xC0..0xCF` set panning is `8xx`. The rest is skipped rather
than approximated. **The effect column wins when both are set**: a tone
portamento with a volume beside it is a glide, and keeping the volume instead
retriggers the note — a different thing happening, not a quieter version of the
same one.

Effects `0x00..0x0F` are ProTracker's, same numbers and parameters; `R0y` is
exactly `E9y` and is mapped. **`G` and `H` — global volume and its slide — are
dropped**, because ntrk's global gain lives on the FXPL plane and only the
*mixer* reads it, so mapping them would produce a fade nobody hears through
`render_add`. The cost is measured: two of the three test tunes use `G` only to
set global volume to full, and the third fades out over its last two and a half
patterns and now ends at full volume instead.

**One sample per instrument** — XM's note map can send notes to sixteen; the
majority one is taken. **Volume envelopes are not imported**; XM's point list is
not the same shape as ntrk's ADSR. Refused rather than approximated: more than
`kMaxChannels` channels (XM allows 32), more than `kMaxInstruments` instruments,
a packing type other than 0, and every truncation. **Ping-pong loops are imported
as forward loops** — the one place a sound is quietly different rather than
absent.

**A truncated sample is refused, never padded.** This is a library a browser
hands an arbitrary file to, and inventing the tail of a sample is a guess. The
offline Python tool this was ported from padded and warned instead, which is why
the port was checked byte-for-byte only on files both accepted.

## Effects

Implemented, with ProTracker's numbering:

| | | | |
|---|---|---|---|
| `0xy` arpeggio | `1xx` portamento up | `2xx` portamento down | `3xx` tone portamento |
| `4xy` vibrato | `5xy` tone porta + volume slide | `6xy` vibrato + volume slide | `7xy` tremolo |
| `8xx` set panning | `9xx` sample offset | `Axy` volume slide | `Bxx` position jump |
| `Cxx` set volume | `Dxx` pattern break | `Fxx` speed / tempo | |

And the extended set: `E1x`/`E2x` fine portamento, `E3x` glissando, `E4x`
vibrato waveform, `E5x` finetune, `E6x` pattern loop, `E7x` tremolo waveform,
`E8x` coarse panning, `E9x` retrigger, `EAx`/`EBx` fine volume slide, `ECx` note
cut, `EDx` note delay, `EEx` pattern delay.

**Not implemented**, and skipped rather than approximated — a tune that uses one
plays without it, which is an audible gap rather than a wrong note:

- `E0x` filter — an Amiga hardware low-pass with no meaning here.
- `EFx` invert loop, a funk-repeat effect almost nothing uses.

## Known limitations

- **Panning is a position, not a field.** Output is stereo: `player_start` fills
  `pan[]` with ProTracker's LRRL, `separation` scales it, and `8xx`/`E8x` move a
  channel from a pattern. What there is no room for is a *per-sample* stereo
  source — an instrument is one mono stream that gets placed, which is what an
  Amiga voice is.
- **Volume clamps at 64**, as ProTracker's does. Worth knowing when a tremolo on
  an already-loud note appears to do nothing: its whole upward half is clipped.
- **Linear interpolation**, not the Amiga's nearest-neighbour. Chosen because a
  few kilobytes of sample played back at an arbitrary rate aliases audibly, and
  it costs two multiplies a frame. It is not bit-exact against a real Amiga and
  is not trying to be.
- Up to 16 channels and 64 instruments, at 256 rows a pattern.

## Licence

Public domain / CC0. Written for the no2 project. The format, the player and the
importer are original work; ProTracker's effect numbering and period table are
facts about a file format rather than anything borrowed.
