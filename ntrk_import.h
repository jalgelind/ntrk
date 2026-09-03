// ntrk_import -- foreign tracker modules, in, as `.ntrk` bytes.
//
//     const size_t need = ntrk::import_scratch_needed(mod, mod_size);
//     if (need != 0 && need <= sizeof(scratch)) {
//       size_t written = 0;
//       if (ntrk::import_convert(mod, mod_size, scratch, sizeof(scratch),
//                                out, sizeof(out), &written))
//         ntrk::module_load(&module, out, written);
//     }
//
// **Bytes in, bytes out. No filesystem, no allocation, no stdio, no libm.**
// That is what lets the same code serve the desktop player, the tracker editor
// and the wasm build, where there is no filesystem at all to read a file from.
// The caller owns every buffer, exactly as `module_save` already works.
//
// **The output goes through `module_save` rather than being written by hand.**
// One source of layout truth: an importer that emitted `.ntrk` bytes itself
// would be a second writer of the format, and two writers of one format drift.
// It also means this file inherits every field check `module_save` makes, for
// free -- a module this builds and cannot save is refused here rather than
// written out as something `module_load` will not open.
//
// **A trust boundary.** This parses a file that came from somewhere else, and
// every read below is bounds-checked against what remains of the buffer, by
// subtraction rather than addition so nothing can wrap. A malformed file
// returns false; there is no half-built module and no guess. The discipline is
// `module_load`'s, deliberately, because it is the same job.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_IMPORT_H_
#define NTRK_IMPORT_H_

#include <stddef.h>
#include <stdint.h>

#include "ntrk.h"

namespace ntrk {

// What a buffer holds. `.s3m` and `.it` slot in here as further values; the
// three functions below are the whole interface a caller sees, so adding a
// format changes nothing on the calling side.
//
// Nothing stores one -- it is what `import_detect` returns and nothing else --
// so this is the only one of these enums with no byte on the other side of it.
enum class ImportFormat : uint8_t {
  kUnknown = 0,
  kMod,
  kXm,
};

// What this buffer is, from its signature alone. Cheap, and it makes no promise
// that the file is well formed -- only `import_convert` does that.
ImportFormat
import_detect(const uint8_t *data, size_t size);

// Bytes of scratch `import_convert` needs for this exact file, or **0 when the
// file cannot be imported at all**. Zero is never a valid answer for a real
// module, so the one return value carries both, and a caller that checks
// `need != 0 && need <= capacity` has checked everything.
//
// Scratch exists because a `.mod`'s cells are packed the other way round --
// period and split sample number, not note and instrument -- so they have to
// become a `Note` array somewhere before `module_save` can walk them. **The
// samples need none of it**: a `.mod`'s are 8-bit signed, which is what PCM8
// already is, so the instruments point straight into `data` and `module_save`
// copies the blob out from there.
//
// **An `.xm` needs scratch for its samples as well as its cells, and that is
// the one structural difference between the two formats here.** XM sample data
// is delta encoded -- each byte is the step from the previous frame, not the
// frame -- so it is not playable until it has been accumulated, and `data` is
// const. The frames are decoded into scratch, the instruments point at that,
// and `module_save` copies the decoded bytes into `out` exactly as it copies a
// `.mod`'s undecoded ones.
size_t
import_scratch_needed(const uint8_t *data, size_t size);

// Convert, into `out`. `out == nullptr` asks for the size without writing, the
// same call shape `module_save` has; `written` is set on both paths.
//
// `data` must outlive the call but not the result -- `out` is a complete file
// with the samples copied into it.
//
// Returns false for anything that is not a module this can read, including
// every truncation: the buffers are untouched past whatever was written before
// the refusal, and `*written` is 0.
bool
import_convert(const uint8_t *data, size_t size, uint8_t *scratch,
               size_t scratch_size, uint8_t *out, size_t out_cap,
               size_t *written);

}  // namespace ntrk

#endif  // NTRK_IMPORT_H_
