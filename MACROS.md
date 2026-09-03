# Macros and meta lanes

*For whoever is building the tracker. The encoding is defined by `Macro` /
`MacroTarget` in `ntrk_types.h`, `fxpl_macro` in `ntrk_mix.cc` and the `MACR`
loader in `ntrk.h`, and those are normative — this is only what an editor has to
get right on top of them.*

## What a macro is

A macro turns **one byte the user types into up to eight parameter changes**. A
cell in a *meta lane* says "run macro 3 with input 200"; macro 3 is a table of up
to eight targets, each with its own scale and offset, so one input can open a
send a little while closing a filter a lot, or sweep one cutoff across four
channels from a single cell.

Without it, "sweep the filter on all four channels" is four columns of identical
slides kept in step by hand — four places to typo a step size and get a sweep
that drifts apart over sixteen bars.

A one-target macro is therefore also a **named automation lane**: give macro 2
the single target "send 1, channel 3" at unity scale and that meta lane's cells
read as the send level directly. `meta_columns` goes up to 4, so four such lanes.

## Geometry, and what a column costs

```
lanes = channels * fx_columns + meta_columns
cell  = ((pattern * rows + row) * lanes) + lane
```

Channel `c` owns lanes `c * fx_columns` upward, contiguous; the meta lanes follow
every channel lane. `module_lanes()` in `ntrk_types.h` is the one place this is
computed — use it rather than a second copy.

**A column is not free, and the file pays for empty cells.** Two bytes a cell a
row: 16 channels x 64 rows x 19 patterns is ~39 KB per `fx_columns` step, ~311 KB
at the ceiling of eight. Raise `fx_columns` when the user needs the column, not
on the way in.

## The trap: a duplicated command does not mean one thing

Two of a channel's columns carrying the same command behave differently across
the two halves of the command range, and an editor showing the user which cell
"wins" has to know which half it is in:

- **Mixer slides sum.** Two columns stepping a pan opposite ways cancel — that is
  what a second column is *for*.
- **Player commands are last column wins.** Two `0x31` glides on one row is one
  glide, the rightmost.

So greying out a `0x30`/`0x31` to the left of another is showing the truth;
doing the same to a duplicated `0x02` is a lie.

**Effect memory is per `(channel, command)`, not per column.** A zero parameter
in column 1 picks up what column 0 wrote for the same command *on the same row*.
An editor that previews "the value this cell will actually use" must resolve it
left to right across the whole row, not per column in isolation.

## A meta cell

```
{u8 macro, u8 input}
```

**The macro index is one-based**; `macro == 0` is an empty cell. `input` cannot
carry emptiness because 0 is a real input, so the other byte does — same
convention as `Note.instrument`.

A cell naming a macro past `macro_count` is **inert, not invalid**: ignored when
played, not refused at load. Do not make the editor refuse it, and do not erase
it behind the user's back — a user who deletes macro 8 should find those cells
grey, not the song unloadable. Grey, not red.

## The arithmetic, if you preview it

```
value = (scale * input + offset) / 256      // 32-bit ints, clamped 0..255
```

**Do the same integer arithmetic, not float.** Nothing on the format path may
depend on libm or on a target's floating-point contraction — the ARM64/wasm
render match is a *measured* property here (`tools/check_ntrk_crosstarget.sh`).
A float preview shows numbers the player does not produce.

**Use 32 bits.** An 8.8 scale of `0x7FFF` times an input of 255 is 8,355,585; a
16-bit intermediate wraps, and a wrap here is not a clipped macro but a loud one
going silent at some arbitrary input.

`scale` is 8.8, so 256 is unity, 128 halves, `-256` inverts about zero (and
clamps at 0). An unsigned input can only climb — **the offset is what lets it go
both ways**: `scale = -256, offset = 255 * 256` mirrors (input 200 → 55), and
`scale = 256, offset = -6 * 256` makes the value `input - 6`, so input 10 steps
+4 and input 2 steps −4. That last shape is what delta macros are usually for.

## Two things to say out loud in the UI

**Absolute and delta are different things, not a flag.** Absolute is "put it
here" — a set, on the row's first tick. Delta is "move it by this much every
tick" — a slide, on ticks 1..speed−1, so a row of speed 6 applies its step five
times. The kind is per macro, not per target, and the two-digit cell does not say
which one it is: mark deltas in the grid the way a tracker distinguishes a slide
from a set.

**Meta lanes are stored last and run first.** That gives *specific beats general*
— a macro sweeps cutoff across four channels and one channel's own column then
trims its own. The first time a user's channel column appears to be ignored, this
is why it is not, so a tooltip or status line saying "meta lanes run first" earns
its space. It decides sets only; deltas sum.

## Drawing meta lanes

They are whole-row and belong to the pattern, not to any channel — so draw them
as a group at the right-hand end, after the last channel, with a visible
separator, where they sit in the file.

```
   ┌── channel 0 ──┐┌── channel 1 ──┐┌─ meta ─┐
   note  ins  fx fx  note  ins  fx fx   M   in
00 C-4   01   01c8 ----  ---   --  ----  ----  03 c8
01 ---   --   02 14 ----  ---   --  ----  ----  -- --
```

Edit the two fields as two. The macro field's empty state is `--`, not `00`; the
input field is a plain byte and `00` is a meaningful cell. Show the macro's
**name** near the cursor — nobody remembers which one `03` is eight macros in,
and `ntrk::mix::macro_describe` will write the line for you, targets and all,
rather than an editor keeping a second copy of the target table.

## Saving

`module_save` refuses exactly what `module_load` refuses, so validating against
it is validating against the loader. Catch it at save time so the user finds out
while they can still fix it. Two rules the loader enforces that an editor can
easily get wrong on the way in:

- **Zero every target slot from `target_count` upward.** Shortening a macro's
  list must *clear* the slots behind it, not merely stop counting them —
  otherwise two byte sequences encode one song, which is a canonicalisation
  hazard in a format whose safety net is a pair of fingerprints over rendered
  audio. Saving a loaded file twice must give identical bytes.
- **Meta lanes with no `MACR` block are refused**, because every meta cell would
  name nothing. Deleting the last macro means either keeping an empty macro
  record or dropping the meta lanes; do not write the half-state. The converse —
  a macro table with no meta lanes — is legal, and is what an editor holds most
  of the time it is open.

And one shape that looks harmless: **a geometry with no plane to put it in.** The
counts live in the `FXPL` prefix, so a module carrying `fx_columns = 2` and no
`FXPL` block loads back as a one-column module. Write the plane, or write
`fx_columns = 1, meta_columns = 0`.

## Two dead ends, on purpose

- **`0xFE` (the invoking channel) does nothing today.** A macro can only be
  invoked from a meta lane, and a meta lane belongs to no channel. It is reserved
  for the day a *channel* lane gains an invocation command. Hide it, or show it
  disabled with that explanation — offering it as a working option produces
  macros that silently do nothing.
- **`AUTO` (block id `0x0010`) is spent and stays spent.** Macros replaced it.
  Giving the id to something else would let a file written against the older
  sketch load as whatever took its place, which is the one failure a block
  directory exists to prevent. Do not write it, do not reuse it.
