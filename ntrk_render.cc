// Render a .ntrk to a WAV, and report what came out.
//
//     c++ -std=c++11 -O2 -o ntrk_render ntrk_render.cc
//     ./ntrk_render tune.ntrk 30 tune.wav
//
// The arguments are the module, how many seconds to render, and optionally a
// file to write. Without the third it renders and reports without writing
// anything, which is the quick way to ask whether an import is playable.
//
// **`--mix` renders through the mixer rather than through `render_add`.** The
// replayer alone cannot hear half of the second effect plane: the sends, the
// per-channel filter and every macro on a meta lane are the mixer's, so a file
// carrying them plays through `render_add` as though they were not written.
// Without the flag nothing changes at all — the mixer's own `master_gain`
// defaults to the same 0.7 the player's `gain` does, precisely so that the two
// paths agree on a module with no plane.
//
// This is how a converted tune gets checked. The importer's own tests can only
// say that a file matches the format; whether it *plays* — that the notes are
// notes, the samples are the right way up and the tune does not fall silent
// thirty seconds in — is a question about audio, and the answer is either these
// numbers or your ears.
//
// stdio, one call to sqrt for the RMS, and the library. The tool may lean on
// libm; ntrk.h itself still does not.
//
// `ntrk_unity.h` rather than `ntrk.h`, so that one compiler invocation still
// builds the whole tool with the mixer in it. That is the one arrangement that
// header forbids combining with compiling the `.cc` files separately, and this
// tool links neither.

#include "ntrk_unity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int kRate = 48000;
static const int kChannels = 2;
static const int kBlock = 1024;      // frames per render call

// A deliberately awkward block size is used below as well: an audio callback
// does not hand over neat multiples of anything, and a player that only works
// on round numbers works only in tests.
static const int kOddBlock = 373;

static void
write_le32(FILE *f, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    fputc((int) ((v >> (8 * i)) & 0xffu), f);
}

static void
write_le16(FILE *f, uint16_t v) {
  fputc((int) (v & 0xffu), f);
  fputc((int) ((v >> 8) & 0xffu), f);
}

// Not on the stack: a Mixer carries the whole plane's state and its scratch
// buffers, which is tens of kilobytes, and this is the only one there is.
static ntrk::mix::Mixer g_mixer;

int
main(int argc, char **argv) {
  // The flag may sit anywhere, so the positional arguments are gathered rather
  // than indexed. Three of them, and a fourth would be ignored silently — which
  // is what `arg_count`'s ceiling says.
  const char *arg[4] = { argv[0], NULL, NULL, NULL };
  int arg_count = 1;
  bool use_mixer = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--mix") == 0)
      use_mixer = true;
    else if (arg_count < 4)
      arg[arg_count++] = argv[i];
  }
  if (arg_count < 3) {
    fprintf(stderr, "usage: %s <module.ntrk> <seconds> [out.wav] [--mix]\n",
            argv[0]);
    return 2;
  }

  FILE *in = fopen(arg[1], "rb");
  if (in == NULL) {
    fprintf(stderr, "cannot open %s\n", arg[1]);
    return 1;
  }
  fseek(in, 0, SEEK_END);
  long size = ftell(in);
  fseek(in, 0, SEEK_SET);
  if (size <= 0) {
    fprintf(stderr, "%s is empty\n", arg[1]);
    fclose(in);
    return 1;
  }
  uint8_t *bytes = (uint8_t *) malloc((size_t) size);
  if (bytes == NULL || fread(bytes, 1, (size_t) size, in) != (size_t) size) {
    fprintf(stderr, "cannot read %s\n", arg[1]);
    fclose(in);
    free(bytes);
    return 1;
  }
  fclose(in);

  // **A `.mod` or an `.xm` is imported on the way in.** The importer is already
  // linked -- `ntrk_unity.h` carries it -- and rendering a foreign module was
  // always a two-step through a converted file, which is one more place for the
  // wrong bytes to be the ones under test. `import_scratch_needed` returns 0
  // for anything it will refuse, so `need != 0 && need <= cap` is the whole
  // check and the buffers are the caller's, as they are everywhere else.
  uint8_t *scratch = NULL;
  uint8_t *converted = NULL;
  if (ntrk::import_detect(bytes, (size_t) size) !=
      ntrk::ImportFormat::kUnknown) {
    const size_t need = ntrk::import_scratch_needed(bytes, (size_t) size);
    size_t out_bytes = 0;
    scratch = need != 0 ? (uint8_t *) malloc(need) : NULL;
    if (scratch == NULL ||
        !ntrk::import_convert(bytes, (size_t) size, scratch, need, NULL, 0,
                              &out_bytes) ||
        (converted = (uint8_t *) malloc(out_bytes)) == NULL ||
        !ntrk::import_convert(bytes, (size_t) size, scratch, need, converted,
                              out_bytes, &out_bytes)) {
      fprintf(stderr, "%s looks like a module this importer refuses\n", arg[1]);
      free(scratch);
      free(converted);
      free(bytes);
      return 1;
    }
    printf("  imported %ld bytes to %zu\n", size, out_bytes);
    free(bytes);
    free(scratch);
    bytes = converted;
    size = (long) out_bytes;
  }

  ntrk::Module module;
  if (!ntrk::module_load(&module, bytes, (size_t) size)) {
    fprintf(stderr, "%s is not a module this player will take\n", arg[1]);
    free(bytes);
    return 1;
  }

  printf("%s: %d channels, %d rows, %d orders, %d patterns, %d instruments, "
         "speed %d, %d bpm%s\n",
         arg[1], module.channels, module.rows, module.order_count,
         module.pattern_count, module.instrument_count, module.speed,
         module.bpm, use_mixer ? ", through the mixer" : "");

  const int seconds = atoi(arg[2]);
  const long frames = (long) kRate * (seconds > 0 ? seconds : 1);

  ntrk::Player player;
  ntrk::player_start(&player, &module);

  // Reset and nothing else: no send carries an effect and no knob is turned
  // from out here, so everything the mixer does to this render came out of the
  // module's own plane. A tool that dialled in a reverb would be measuring its
  // own settings as much as the file.
  // **The mixer with every send empty renders the same audio the player does**,
  // which is the trap this tool sat in: `--mix` ran, the plane's send commands
  // moved levels, and the levels fed slots whose kind was still
  // `FxKind::kNone`. The cross-target check therefore exercised none of the
  // effects -- a libm call or a contraction inside the reverb or the delay
  // could not have been caught by the one script whose whole job is catching
  // them.
  //
  // A reverb on send 0 and a delay on send 1, so a module that sends to either
  // is actually processed. `ntrk` never allocates, so the tool owns the memory.
  float *reverb_tank = nullptr;
  float *delay_line = nullptr;
  if (use_mixer) {
    ntrk::mix::mixer_reset(&g_mixer);

    const size_t rv = ntrk::fx::reverb_bytes_needed(
        (float) kRate, ntrk::mix::kSlotPredelayMaxSeconds);
    const size_t dl = ntrk::fx::delay_bytes_needed(
        ntrk::mix::kSlotDelayMaxSeconds, (float) kRate);
    reverb_tank = (float *) malloc(rv);
    delay_line = (float *) malloc(dl);
    if (reverb_tank == nullptr || delay_line == nullptr) {
      fprintf(stderr, "out of memory for the send effects\n");
      return 1;
    }
    if (!ntrk::fx::reverb_init(&g_mixer.send[0].reverb, reverb_tank, rv,
                               (float) kRate,
                               ntrk::mix::kSlotPredelayMaxSeconds) ||
        !ntrk::fx::delay_init(&g_mixer.send[1].delay, delay_line, dl,
                              (float) kRate,
                              ntrk::mix::kSlotDelayMaxSeconds)) {
      fprintf(stderr, "a send effect refused the memory it asked for\n");
      return 1;
    }
    // Settings before the kind: `slot_set_kind` is where `Slot::base` is taken.
    g_mixer.send[0].param[0] = 0.74f;   // size
    g_mixer.send[0].param[1] = 0.38f;   // damping
    g_mixer.send[0].param[2] = 0.02f / ntrk::mix::kSlotPredelayMaxSeconds;
    g_mixer.send[0].param[3] = 1.f;     // width
    g_mixer.send[0].param[4] = 1.f;     // mix -- all wet, it is a send
    ntrk::mix::slot_set_kind(&g_mixer.send[0], ntrk::mix::FxKind::kReverb);

    g_mixer.send[1].param[0] = 0.25f / ntrk::mix::kSlotDelayMaxSeconds;
    g_mixer.send[1].param[1] = 0.42f;   // feedback
    g_mixer.send[1].param[2] = 0.30f;   // damping in the feedback path
    g_mixer.send[1].param[3] = 1.f;     // mix
    g_mixer.send[1].param[4] = 1.f;     // ping-pong
    ntrk::mix::slot_set_kind(&g_mixer.send[1], ntrk::mix::FxKind::kDelay);
  }

  double block[kBlock * kChannels];
  short *pcm = (short *) malloc(sizeof(short) * (size_t) frames * kChannels);

  double peak = 0.0, energy = 0.0;
  long silent = 0, done = 0;
  int clipped = 0;

  // FNV-1a over the *quantised* stream, which is the same thing a WAV would
  // contain -- so the number below is a fingerprint of the audio rather than of
  // the float noise underneath it. Printed always rather than behind a flag:
  // it is one more number on a line that is already there, and a check nobody
  // has to remember to ask for is a check that gets used.
  uint64_t hash = 1469598103934665603ULL;

  while (done < frames) {
    // Alternate the block size, so a tick boundary lands inside a buffer and
    // at its edge over the course of a render rather than always in one place.
    int want = ((done / kBlock) & 1) ? kOddBlock : kBlock;
    if (want > (int) (frames - done))
      want = (int) (frames - done);
    for (int i = 0; i < want * kChannels; ++i)
      block[i] = 0.0;
    if (use_mixer)
      ntrk::mix::mixer_render_add(&g_mixer, &player, block, want, kChannels,
                                  (float) kRate);
    else
      ntrk::render_add(&player, block, want, kChannels, (float) kRate);

    for (int f = 0; f < want; ++f) {
      const double l = block[f * kChannels];
      const double a = l < 0.0 ? -l : l;
      if (a > peak)
        peak = a;
      energy += l * l;
      if (a < 1.0e-6)
        ++silent;
      if (a >= 1.0)
        ++clipped;
      for (int c = 0; c < kChannels; ++c) {
        double v = block[f * kChannels + c] * 32767.0;
        if (v > 32767.0)
          v = 32767.0;
        if (v < -32768.0)
          v = -32768.0;
        const short q = (short) v;

        // Through a uint16_t on the way in: a right shift of a negative signed
        // value is implementation-defined, and this number has to be the same
        // on every target or it is worth nothing.
        const uint16_t u = (uint16_t) q;
        hash ^= (uint64_t) (u & 0xffu);
        hash *= 1099511628211ULL;
        hash ^= (uint64_t) ((u >> 8) & 0xffu);
        hash *= 1099511628211ULL;

        if (pcm != NULL)
          pcm[(done + f) * kChannels + c] = q;
      }
    }
    done += want;

    if (!player.playing) {
      printf("  the tune ended after %.1f s\n", (double) done / kRate);
      break;
    }
  }

  printf("  peak %.3f, rms %.4f, %.1f%% silent, %d clipped frames\n", peak,
         done > 0 ? sqrt(energy / (double) done) : 0.0,
         done > 0 ? 100.0 * (double) silent / (double) done : 0.0, clipped);
  printf("  hash %016llx over %ld frames at %d Hz\n",
         (unsigned long long) hash, done, kRate);

  if (arg[3] != NULL && pcm != NULL) {
    FILE *out = fopen(arg[3], "wb");
    if (out == NULL) {
      fprintf(stderr, "cannot write %s\n", arg[3]);
      free(pcm);
      free(bytes);
      return 1;
    }
    const uint32_t data_bytes = (uint32_t) (done * kChannels * 2);
    fwrite("RIFF", 1, 4, out);
    write_le32(out, 36u + data_bytes);
    fwrite("WAVEfmt ", 1, 8, out);
    write_le32(out, 16u);
    write_le16(out, 1u);                                  // PCM
    write_le16(out, (uint16_t) kChannels);
    write_le32(out, (uint32_t) kRate);
    write_le32(out, (uint32_t) (kRate * kChannels * 2));   // byte rate
    write_le16(out, (uint16_t) (kChannels * 2));           // block align
    write_le16(out, 16u);                                  // bits
    fwrite("data", 1, 4, out);
    write_le32(out, data_bytes);
    fwrite(pcm, 1, data_bytes, out);
    fclose(out);
    printf("  wrote %s, %.1f s\n", arg[3], (double) done / kRate);
  }

  free(pcm);
  free(bytes);
  return 0;
}
