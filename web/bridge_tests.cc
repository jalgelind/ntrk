#include "bridge.cc"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>

void save_fixture(const char *directory, const char *name, int mask) {
  assert(web_used_channels() == mask);
  const int size = web_save(); assert(size > 0);
  const std::vector<uint8_t> before(web_input(), web_input() + size);
  if (directory) {
    const std::string path = std::string(directory) + "/" + name + ".ntrk";
    FILE *file = std::fopen(path.c_str(), "wb"); assert(file);
    assert(std::fwrite(before.data(), 1, before.size(), file) == before.size());
    assert(std::fclose(file) == 0);
  }
  assert(web_load(size) && web_info(0) == 16 && web_used_channels() == mask);
  assert(web_save() == size && !std::memcmp(before.data(), web_input(), size));
}

void test_channel_usage(const char *directory) {
  assert(web_demo(48000) && web_used_channels() == 7);
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  assert(web_used_channels() == 0); // Default gain/pan are not musical use.
  // Every primary byte matters, including a parameter with no command.
  for (int field = 0; field < 4; ++field) {
    assert(web_edit(1, 31, 8, field, 1)); assert(web_used_channels() == (1 << 8));
    assert(web_edit(1, 31, 8, field, 0)); assert(web_used_channels() == 0);
  }
  song.fx_columns = 2; song.meta_columns = 1; song.macro_count = 1;
  std::vector<ntrk::FxCell> fx(2 * 32 * ntrk::module_lanes(&song));
  song.fx = fx.data();
  // Row-wide macro automation must not turn every track into a used track.
  fx[16 * 2].cmd = 1; fx[16 * 2].param = 64;
  assert(web_used_channels() == 0);
  save_fixture(directory, "empty", 0);

  // The second pattern is stored but not referenced by the order list.
  assert(web_demo(48000));
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  song.order_count = 1; song.hidden_channels = 0xffff;
  notes[0].note = 25;
  notes[3].instrument = 1;
  notes[(1 * 32 + 31) * 16 + 11].param = 127;
  song.fx_columns = 2; song.meta_columns = 1; song.macro_count = 1;
  std::fill(fx.begin(), fx.end(), ntrk::FxCell()); song.fx = fx.data();
  const int automation = (1 * 32 + 31) * ntrk::module_lanes(&song) + 14 * 2 + 1;
  fx[automation].param = 64; assert(web_used_channels() == ((1<<0)|(1<<3)|(1<<11)|(1<<14)));
  fx[automation].param = 0; fx[automation].cmd = ntrk::mix::kFxplSendSet;
  fx[16 * 2].cmd = 1; fx[16 * 2].param = 64;
  save_fixture(directory, "sparse", (1<<0)|(1<<3)|(1<<11)|(1<<14));

  assert(web_demo(48000));
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  int mask = 0;
  for (int channel : {0, 1, 3, 4, 6, 7, 9, 11, 13, 15}) {
    notes[channel].note = 25; mask |= 1 << channel;
  }
  save_fixture(directory, "wide", mask);
  std::puts("web channel usage: primary fields, sparse IDs, unreferenced patterns, multi-column FXPL, meta exclusion, empty/default mixer, exact save/load passed");
}

int main(int argc, char **argv) {
  assert(!web_demo(0));
  assert(web_demo(48000));
  assert(web_info(0) == ntrk::kMaxChannels && web_info(1) == 32 && web_info(3) == 2);
  assert(!std::strcmp(web_instrument_name(1), "Night circuit / bass"));
  assert(web_instrument_volume(1) == 40);
  assert(!std::strcmp(web_instrument_name(0), "") && !std::strcmp(web_instrument_name(4), ""));
  assert(web_instrument_volume(0) == -1 && web_instrument_volume(4) == -1);
  assert(web_render(128));
  for (double x : audio) assert(x == 0);
  assert(!web_render(129) && !web_render(0));
  assert(!web_edit(-1, 0, 0, 0, 25));
  assert(!web_edit(0, 32, 0, 0, 25));
  assert(!web_edit(0, 0, ntrk::kMaxChannels, 0, 25));
  assert(!web_edit(0, 0, 0, 0, 98));
  assert(!web_edit(0, 0, 0, 1, 4));
  assert(!web_edit(0, 0, 0, 2, ntrk::kFxLast + 1));
  assert(!web_edit(0, 0, 0, 3, 256));
  assert(web_edit(0, 0, 0, 0, 96));
  assert(web_edit(0, 1, 0, 0, ntrk::kNoteOff));
  assert(web_edit(0, 2, 0, 2, ntrk::kFxLast));
  int size = web_save(); assert(size > 0);
  std::vector<uint8_t> saved(web_input(), web_input() + size);
  assert(web_load(size));
  assert(notes[0].note == 96 && notes[ntrk::kMaxChannels].note == ntrk::kNoteOff);
  assert(web_save() == size);
  assert(!std::memcmp(saved.data(), web_input(), size));
  // Repeated loads exercise both backing buffers and failed-load preservation.
  assert(web_load(size));
  web_input()[0] = 0;
  assert(!web_load(size) && !web_load(-1) && !web_load(kFileCap + 1));
  assert(web_save() == size);
  assert(!std::memcmp(saved.data(), web_input(), size));
  assert(!web_transport(3, 2, 0) && !web_transport(3, 0, 32));
  assert(web_transport(3, 1, 16));
  assert(web_info(6) == 1 && web_info(7) == 16);
  assert(web_demo(48000));
  assert(web_transport(1, 0, 0));
  double energy = 0;
  for (int block = 0; block < 1000; ++block) {
    assert(web_render(128));
    for (double x : audio) { assert(std::isfinite(x) && std::abs(x) <= 1.0); energy += x * x; }
  }
  assert(energy > 1 && web_info(7) > 0);
  assert(web_transport(2, 0, 0));
  const int paused = web_info(7);
  web_render(128); assert(web_info(7) == paused);
  for (double x : audio) assert(x == 0);
  for (int c = 0; c < ntrk::kMaxChannels; ++c) assert(web_mute(c));
  assert(!web_mute(ntrk::kMaxChannels));
  assert(web_transport(0, 0, 0));
  assert(web_info(6) == 0 && web_info(7) == 0 && web_info(11) == 65535);
  web_transport(1, 0, 0); web_render(128);
  for (double x : audio) assert(x == 0);
  // Exercise MIXR allocation/configuration with delay and reverb and PCM data.
  assert(web_demo(96000));
  int8_t sample[32];
  for (int i = 0; i < 32; ++i) sample[i] = static_cast<int8_t>(i * 8 - 128);
  auto &pcm = song.instruments[2];
  pcm = ntrk::Instrument(); pcm.data = sample; pcm.length = 32; pcm.loop_len = 32;
  uint8_t slices[] = {0, 0, 0, 0, 16, 0, 0, 0};
  pcm.slices = slices; pcm.slice_count = 2;
  std::vector<ntrk::FxCell> plane(song.pattern_count * song.rows * song.channels);
  plane[0].cmd = ntrk::mix::kFxplSendSet; plane[0].param = 64;
  song.fx = plane.data();
  song.hidden_channels = 0xfff8;
  ntrk::slot_set_muted(&song, 1, 2, true);
  mixer.send[0].param[4] = 1;
  ntrk::mix::slot_set_kind(&mixer.send[0], ntrk::mix::FxKind::kReverb);
  mixer.send[1].param[0] = .1f; mixer.send[1].param[3] = 1;
  ntrk::mix::slot_set_kind(&mixer.send[1], ntrk::mix::FxKind::kDelay);
  ntrk::mix::mixer_config_write(&mixer, &song);
  size = web_save(); assert(size > 0 && web_load(size));
  assert(mixer.send[0].reverb.buffer != nullptr);
  assert(mixer.send[1].delay.buffer != nullptr);
  assert(song.hidden_channels == 0xfff8 && ntrk::slot_muted(&song, 1, 2));
  assert(song.fx[0].cmd == plane[0].cmd && song.fx[0].param == 64);
  assert(song.instruments[2].slice_count == 2);
  assert(!std::memcmp(song.instruments[2].data, sample, sizeof(sample)));
  std::vector<uint8_t> with_samples(web_input(), web_input() + web_save());
  assert(web_load(static_cast<int>(with_samples.size())));
  assert(web_save() == static_cast<int>(with_samples.size()));
  assert(!std::memcmp(with_samples.data(), web_input(), with_samples.size()));
  test_channel_usage(argc > 1 ? argv[1] : nullptr);
  std::puts("web bridge: bounds, roundtrip, transactional load, transport, mute, finite audio, effect allocation passed");
}
