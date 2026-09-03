// ntrk_unity -- every effect module and the mixer, in one include.
//
// Two ways to build this directory, and both are supported and both are tested:
//
// - **Single translation unit.** Include this file from exactly *one* `.cc` of
//   your own and compile nothing else. This is what test_ntrk_fx.cc and
//   test_ntrk_mix.cc do, and it is what keeps the "one compiler invocation, no
//   build system" claim in README.md true.
// - **Multiple translation units.** Include the plain `.h` files where you need
//   the API and add `ntrk_fx_delay.cc`, `ntrk_fx_reverb.cc` and `ntrk_mix.cc`
//   to your build. `make test-ntrk-fx-tu` compiles each of them separately, on
//   its own and under `-Werror`, as the check on this.
//
// **The replayer is not here, and does not need to be.** `ntrk.h` is header-only
// and carries `ntrk_synth.h` and `ntrk_fx_shape.h` with it; a caller that only
// plays modules includes that and links none of the code below.
//
// The importer *is* here, and it is the one file below that pulls `ntrk.h` in
// with it: it builds a `Module` and hands it to `module_save` rather than
// writing `.ntrk` bytes of its own, which is what keeps one writer of the
// format. A caller that never opens a `.mod` pays a header it already has.
//
// **Do not do both.** The definitions here are not `inline`, so a project that
// includes this file *and* compiles the `.cc` files gets duplicate symbols at
// link time — the same trap as including a `.cc` from two places, which is how
// this project's own unity build is arranged (see CLAUDE.md). Including this
// header from two translation units is the same mistake wearing a `.h`.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_UNITY_H_
#define NTRK_UNITY_H_

#include "ntrk_fx_delay.cc"
#include "ntrk_fx_reverb.cc"
#include "ntrk_import.cc"
#include "ntrk_mix.cc"

#endif  // NTRK_UNITY_H_
