// Reuse the importer's fixture builders so there is one writer of foreign bytes.
#define main ntrk_import_test_main
#include "../test_ntrk_import.cc"
#undef main
#include <cassert>
#include <string>

int main(int argc, char **argv) {
  assert(argc == 2);
  for (int format = 0; format < 2; ++format) {
    XmSpec xm; xm.wide[1] = true; xm.channels = 8;
    ModSpec mod = spec_typical(); mod.sig = "8CHN";
    const size_t size = format ? xm_build(g_mod, xm) : mod_build(g_mod, mod);
    const std::string name = std::string(argv[1]) + (format ? "/sampled.xm" : "/sampled.mod");
    FILE *file = std::fopen(name.c_str(), "wb"); assert(file);
    assert(std::fwrite(g_mod, 1, size, file) == size); assert(!std::fclose(file));
  }
}
