# ntrk — what is open

Finished work is not listed here. The git log is the record; this file is only
the part that is still open, so that its length is a fact about the remaining
work rather than about how long the project has been going.

**Two policies govern anything added below.**

- **No fast-math, no flush-to-zero.** The product is one translation unit, so a
  per-file flag would land on the simulation, and `-ffp-contract=off` exists to
  keep wasm and ARM64 computing identical floats. wasm has no FTZ mode at all,
  and on web `product_audio` runs on the main thread beside `product_update`.
  Denormals are handled in the algorithms. **No path in the player today can
  produce a subnormal** — `pos`/`step`, `volume` and `trem_offset` are integers
  or bounded well above it, and `channel_sample` bottoms out near 2^-45 against
  a float subnormal at 2^-126. The exposure arrives entirely with feedback:
  delay-line elements on write-back, one-pole damping state, the feedback
  accumulator, and any DC blocker's history. Flush at the **store**, not the
  load, with a compare-and-select — bit-exact and identical on ARM64 and wasm.
  Never inject DC or dither: the reference hashes would become hashes of the
  anti-denormal noise.
- **No oversampling.** Where a model needs it, it goes behind `NTRK_OVERSAMPLE`,
  off by default, and the non-oversampled path must stand on its own.

**Every port is minimal and self-contained:** a state struct and two free
functions in one file, reaching for nothing but `<cmath>`. If it cannot be
reduced to that, it goes on the not-planned list instead.

## Open

- [ ] **C3 Denormal timing.** Thirty seconds of reverb tail decaying into
      silence, timed, to show the flush-at-the-store actually costs nothing.
      **This cannot be run on the machine the library is developed on** — the
      denormal cliff is an x86 effect and Apple Silicon does not reproduce it,
      which is precisely why it is worth checking somewhere else rather than
      assuming. It needs an x86 host or a CI runner; until then the flushes are
      argued from the code and unmeasured.

- [ ] **C10 One tune that uses everything at once.** Every feature on a real
      row, rendered and fingerprinted. The cross-target coverage module built by
      `ntrk_gen.cc` is most of the way there and is the place to grow it, not a
      second file — it already carries both effect planes, a macro lane, the
      voice filter and both send effects.

- [ ] **T41 Multi-sample XM instruments.** An XM instrument maps its 96 notes
      across up to sixteen samples; an ntrk instrument holds one, so the
      importer takes the sample the majority of mapped notes point at and drops
      the rest. Splitting into separate ntrk instruments and renumbering the
      cells that reach them fits comfortably — `kMaxInstruments` is 64 and the
      corpus uses at most 17 — but **no file in the corpus needs it yet**: the
      one multi-sample instrument there has an all-zero note map. Do it when a
      tune actually loses something.

- [ ] **T50 Swing.** Five of the eight shipped 303 tunes carry a shuffle figure
      — 0.08 to 0.19 — and the row grid is straight, so the number is recorded
      beside each tune and thrown away at playback. It is the only thing those
      tunes ask for that the player cannot say.
      **Shape:** a per-row tick offset. `frames_per_tick` is already the one
      place a row's length is decided (`ntrk.h`), so delaying the odd rows of a
      pair is a bounded change there rather than a new clock. It must not move
      a straight tune by a sample: swing zero has to be bit-identical, which is
      the check that makes it safe to add at all.
      **Where it lives:** a module field is the honest home — swing belongs to
      the tune, not to a row — and the header has no room (`a header change is
      version 3`), so it is an optional block or an FXPL meta command. Decide
      that before writing any of it.
      **Verified by:** a straight module rendering bit-identically before and
      after, and a swung one placing its odd rows late by the frames the figure
      asks for, counted rather than heard.

- [ ] **T51 Player-side pattern loop.** The editor loops a pattern by watching
      the order index and rewinding it, which is fine for the editor and is not
      something the library offers a caller. A loop flag on the player — loop
      this order entry, or this row range — is a small piece of state and one
      branch where the order advances.
      **Verified by:** rendering N rows past the end of a looped pattern and
      requiring the audio to equal the first N rows of it.

- [ ] **T52 Seek without rendering.** Reaching row R today means rendering
      everything before it, because the only way to advance the player is to ask
      it for audio. A tick-without-render path — run the sequencer, skip the
      mixer — makes an editor's "play from here" instant and is what an accurate
      seek is made of. The risk is that it becomes a second sequencer that
      drifts from the first, so it must be the *same* function with the audio
      half skipped, not a copy.
      **Verified by:** seeking to row R and rendering, against rendering from
      the top and discarding — the two must be sample-identical.

- [ ] **XM volume envelopes are dropped, and that is now the largest measured
      gap.** Checking every rendered file against libopenmpt put a number on it:
      clearing the envelope bit on libopenmpt's own copy moves the envelope
      correlation from 0.554 to 0.888 on brothomstates, 0.760 to 0.815 on radix
      and 0.975 to 0.982 on Jallabert — so envelopes are essentially the whole
      remaining difference. Six of eleven, eleven of seventeen and seven of
      fourteen instruments carry one. XM's envelopes are point lists rather than
      ADSR, so this is not a field-mapping job; it needs a real envelope
      evaluator in the player.

## Not planned

**Format version 1.** Removed. The surviving format keeps the version byte at
2 on purpose: had it claimed 1, every existing v1 file would be accepted and
then misparsed, since a v1 instrument entry is 20 bytes where this one is 32.
Refusing them by version is the point, and it is tested with real v1 bytes.

**`.s3m` and `.it` importers.** Both would slot in behind `import_detect` /
`import_scratch_needed` / `import_convert` without a caller changing, which is
what that interface is for. Nothing is waiting on them; add one if a tune worth
having turns out to exist only in that format.

**Automation lanes.** The FXPL plane plus macro tables already reach every knob
a tune has asked for. Revisit if a real module runs out of columns.

**Extracting a shared DSP core.** `ntrk_dsp.h` already is one — `soft_clip` and
`flush_denorm` live there and the effects are three self-contained files with no
duplication between them. There is nothing left to extract.

**A `.mod` writer.** The import direction exists because tunes already exist as
`.mod`; export would only feed other trackers, and not pretending to be a 1988
Amiga module is the point of having a format at all.

**Plate, spring and shimmer reverb.** Character models with oversampling, which
is a different thing from reverb — the FDN is a real reverb without them.
