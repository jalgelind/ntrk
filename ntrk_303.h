// ntrk_303 -- the TB-303 ladder: a1k0n's measured poles, realised as ZDF.
//
// This is the filter, not *a* filter. A 303's voice is a saw through a diode
// ladder and the ladder is most of what the machine sounds like, so the two-pole
// state-variable filter this voice used to run was never going to get there: a
// 303 is an 18 dB/oct ladder with a highpass in its feedback path, and the
// highpass is specifically what keeps a resonant low note from whistling
// fifths at you instead of squelching.
//
// **Poles from measurement, not from a topology.** `kNtrk303Poles` is a1k0n's
// fit to a real x0xb0x (https://github.com/a1k0n/303): for each of 64
// resonance settings it gives five poles as `constant + slope * w`, where
// `w = 2*pi*fc/sr`. The slopes are copied unchanged; the constants are
// per-sample, fitted at 44.1 kHz, and are stored rescaled to 48 kHz. One first-order pole for the feedback highpass and
// two conjugate pairs for the ladder -- five poles, and the pair-of-pairs is
// where the 18 dB comes from. Nothing here derives a cutoff from a circuit; the
// table already did.
//
// **ZDF/TPT realisation, and the reason is modulation.** The poles could be run
// as two direct-form biquads, which is what the measurement is naturally
// expressed as. But this voice sweeps the cutoff *constantly* -- that is what
// `env_mod` is -- and a direct-form biquad's coefficients carry the assumption
// that they did not move since the last sample. A trapezoidal-integrated SVF
// carries no such assumption. So each conjugate pair is realised as one TPT SVF
// whose poles are placed to equal the table's, with the three taps mixed so the
// *zeros* come out at the origin as well:
//
//     g = sqrt((1 + a1 + a2) / (1 - a1 + a2))       [places the poles]
//     R = (1 + g^2)(1 - a2) / (g (1 + a2))
//     c = (1 - a1 + a2) * (1 + R g + g^2) / 4       [lowpass tap]
//     bandpass tap = 2 g c,  highpass tap = g^2 c
//
// The tap mix is not a fit: a raw SVF's taps have numerators (z+1)^2, (z^2-1)
// and (z-1)^2, and that combination cancels the z^1 and z^0 terms exactly,
// leaving the constant numerator a measured all-pole section has. The transfer
// function is the direct form's by construction, not by approximation.
//
// **Coefficients every sixteenth sample, glided linearly in between.** The pole
// maths costs three exponentials, two cosines and two square roots; the
// per-sample filter costs a dozen adds. Running the first at the rate of the
// second would make the coefficient update the whole cost of the voice. So it
// runs once per `kLadder303Stride` samples and the live coefficients walk
// toward the new ones an increment at a time -- which is also smoother than
// stepping them, and is why this is a glide and not a hold. The boundary is the
// caller's sample counter (`synth_age`, zeroed at the trigger), so the same
// note renders the same samples whatever buffer size the host asks for.
//
// **No libm, and that is a determinism rule rather than a size one.** This
// project pins hashes over rendered audio and requires ARM64 and wasm to
// produce identical floats; `std::exp` and `std::cos` are not correctly rounded
// and Apple's libm and emscripten's musl are free to disagree in the last bits.
// The three primitives below are arithmetic -- range reduction and a Taylor
// series, Newton's method from a bit-twiddled guess -- so they cannot disagree.
// Measured against `<cmath>` over the whole operating range the filter can
// reach: exp 2.6e-6 relative, cos 6.2e-7 absolute, sqrt 1.2e-7 relative (about
// one ulp). That is four orders better than the 2.8% error that ruins a
// high-Q pole radius, which is the accuracy bar this path actually has.
//
// Public domain / CC0. Written for the no2 project. The pole table is from
// a1k0n's 303.js, which is MIT.

#ifndef NTRK_303_H_
#define NTRK_303_H_

#include "ntrk_dsp.h"

#include <stdint.h>

namespace ntrk {
namespace fx {

// ----------------------------------------------------------------------------
// -- Arithmetic that would otherwise be libm
// ----------------------------------------------------------------------------

// **exp(x) - 1, not exp(x), and the minus one is the whole point.** A resonant
// pole sits at a radius like 0.9975, so `1 - exp(pr)` is 0.0025 computed as the
// difference of two numbers near one: in float that is 1.3% error before any
// approximation is involved, and the sections' prewarp and damping are both
// built out of exactly that difference. Every formula below is arranged so the
// small quantity is produced small rather than subtracted out, and this is the
// primitive that makes it possible.
//
// Range reduced by 32 and unwound with `expm1(2x) = t(t + 2)` five times, so
// the series only ever runs on [-0.25, 0]. Defined for x <= 0, which is the
// only sign a stable pole has, and clamped at -8 -- a radius of 0.0003, well
// past the point a section has stopped resonating.
inline float
ntrk303_expm1(float x) {
  if (!(x < 0.f))             // false for NaN as well as for positives
    return 0.f;
  if (x < -8.f)
    x = -8.f;
  const float u = x * (1.f / 32.f);
  float t = u * (1.f + u * (1.f / 2.f) * (1.f + u * (1.f / 3.f) *
            (1.f + u * (1.f / 4.f) * (1.f + u * (1.f / 5.f) *
            (1.f + u * (1.f / 6.f))))));
  t = t * (t + 2.f);
  t = t * (t + 2.f);
  t = t * (t + 2.f);
  t = t * (t + 2.f);
  t = t * (t + 2.f);
  return t;
}

// sin(y) and cos(y) for a *half* pole angle, y in [0, 1.4] -- half of the
// table's own 2.8 clamp, just under pi. Half angles because the two quantities
// the section needs are `(1-r)^2 + 4 r sin^2(y)` and `(1-r)^2 + 4 r cos^2(y)`,
// which are the same two numbers `1 + a1 + a2` and `1 - a1 + a2` with the
// cancellation taken out of them.
//
// Plain Taylor, which over this range is shorter than a minimax fit and needs
// no coefficients anyone has to trust: nine terms of sine and ten of cosine
// leave under 1e-7.
inline void
ntrk303_sincos_half(float y, float *sin_out, float *cos_out) {
  if (y < 0.f)
    y = -y;
  if (!(y < 1.4f))            // false for NaN, which must not reach the series
    y = 1.4f;
  const float y2 = y * y;
  *sin_out = y * (1.f + y2 * (-(1.f / 6.f) + y2 * ((1.f / 120.f) +
             y2 * (-(1.f / 5040.f) + y2 * (1.f / 362880.f)))));
  *cos_out = 1.f + y2 * (-0.5f + y2 * ((1.f / 24.f) +
             y2 * (-(1.f / 720.f) + y2 * ((1.f / 40320.f) +
             y2 * (-(1.f / 3628800.f))))));
}

// sqrt(x) for x > 0. Newton-Raphson from the classic halve-the-exponent guess,
// which is good to about 3.5% and squares its own error every step -- four of
// them reach the float's last bit from anywhere in range.
//
// **The union is a deliberate type pun and not a lapse.** Reading the member
// that was not written is what makes the exponent halvable with a shift; both
// compilers this project targets document it as supported, and the alternative
// (`memcpy`) would need `<string.h>` in a header that is meant to need nothing.
inline float
ntrk303_sqrt(float x) {
  if (!(x > 0.f))
    return 0.f;
  union { float f; uint32_t u; } v;
  v.f = x;
  v.u = 0x1fbd1df5u + (v.u >> 1);
  float y = v.f;
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  return y;
}

// ----------------------------------------------------------------------------
// -- The measured poles
// ----------------------------------------------------------------------------

// Rows, in pairs of (constant, slope in w): the feedback highpass pole, then
// the first ladder pair's real and imaginary parts, then the second pair's.
// Sixty-four resonance settings across; the fit stops there because the real
// circuit stops there -- past index 63 the diode ladder self-oscillates and the
// poles do not move any further, which the table records by simply ending.
//
// **The constant rows are per sample at 48 kHz.** a1k0n fitted them at 44.1
// kHz (303.js's `f_smp`); they are stored pre-multiplied by 44100/48000 so the
// common rate needs no rescale, and `ladder303_set` scales from here.
const int kNtrk303ResoLevels = 64;
const float kNtrk303TableRate = 48000.f;
const float kNtrk303Poles[10][kNtrk303ResoLevels] = {
    // [0] feedback highpass pole, constant
    { -0.01308997f, -0.010157549f, -0.008802519f, -0.007934033f, -0.0073035364f, -0.0068131797f,
      -0.0064145946f, -0.006080467f, -0.005793931f, -0.005543879f, -0.0053226277f, -0.0051246546f,
      -0.004945861f, -0.004783128f, -0.004634028f, -0.0044966373f, -0.0043694056f, -0.004251068f,
      -0.0041405763f, -0.004037056f, -0.003939768f, -0.003848082f, -0.0037614582f, -0.0036794294f,
      -0.0036015885f, -0.0035275798f, -0.0034570894f, -0.00338984f, -0.003325585f, -0.0032641045f,
      -0.0032052007f, -0.0031486962f, -0.0030944305f, -0.0030422588f, -0.0029920484f, -0.0029436795f,
      -0.0028970418f, -0.002852035f, -0.0028085664f, -0.002766551f, -0.0027259104f, -0.0026865718f,
      -0.0026484684f, -0.0026115375f, -0.0025757214f, -0.002540966f, -0.0025072214f, -0.0024744403f,
      -0.0024425788f, -0.0024115962f, -0.0023814535f, -0.0023521148f, -0.002323546f, -0.002295715f,
      -0.002268592f, -0.0022421482f, -0.0022163566f, -0.0021911922f, -0.0021666307f, -0.0021426498f,
      -0.0021192273f, -0.0020963433f, -0.0020739783f, -0.0020521136f },
    // [1] feedback highpass pole, slope
    { 1.6332367e-16f, -0.0161447133f, -0.019993207f, -0.0209872f, -0.0209377795f, -0.020447015f,
      -0.0197637613f, -0.0190036975f, -0.0182242987f, -0.0174550383f, -0.0167110053f, -0.0159995606f,
      -0.0153237941f, -0.0146844019f, -0.0140807436f, -0.0135114504f, -0.0129747831f, -0.0124688429f,
      -0.0119916965f, -0.0115414484f, -0.0111162818f, -0.0107144801f, -0.0103344362f, -0.00997465446f,
      -0.00963374867f, -0.00931043725f, -0.0090035371f, -0.00871195702f, -0.00843469084f, -0.00817081077f,
      -0.00791946102f, -0.00767985179f, -0.00745125367f, -0.00723299254f, -0.00702444481f, -0.00682503313f,
      -0.00663422244f, -0.0064515164f, -0.00627645413f, -0.00610860728f, -0.0059475773f, -0.00579299303f,
      -0.00564450848f, -0.00550180082f, -0.00536456851f, -0.0052325297f, -0.00510542063f, -0.00498299431f,
      -0.00486501921f, -0.00475127814f, -0.00464156716f, -0.00453569463f, -0.00443348032f, -0.00433475462f,
      -0.00423935774f, -0.00414713908f, -0.00405795659f, -0.00397167614f, -0.00388817107f, -0.00380732162f,
      -0.00372901453f, -0.00365314257f, -0.0035796042f, -0.00350830319f },
    // [2] ladder pair 1, real part, constant
    { -1.6863252e-06f, -0.0012403865f, -0.001392162f, -0.001483209f, -0.0015484308f, -0.0015992218f,
      -0.0016407743f, -0.0016758997f, -0.0017062944f, -0.0017330614f, -0.0017569585f, -0.0017785291f,
      -0.0017981759f, -0.0018162053f, -0.0018328568f, -0.0018483201f, -0.0018627486f, -0.0018762681f,
      -0.0018889826f, -0.0019009794f, -0.0019123327f, -0.0019231056f, -0.0019333523f, -0.00194312f,
      -0.0019524499f, -0.001961378f, -0.001969936f, -0.0019781527f, -0.0019860526f, -0.0019936585f,
      -0.0020009903f, -0.0020080667f, -0.0020149038f, -0.0020215167f, -0.0020279188f, -0.0020341228f,
      -0.00204014f, -0.0020459807f, -0.0020516545f, -0.0020571703f, -0.002062536f, -0.0020677596f,
      -0.002072848f, -0.0020778072f, -0.0020826438f, -0.0020873633f, -0.002091971f, -0.0020964716f,
      -0.0021008698f, -0.00210517f, -0.0021093762f, -0.0021134922f, -0.0021175218f, -0.002121468f,
      -0.0021253345f, -0.0021291235f, -0.0021328388f, -0.0021364824f, -0.0021400573f, -0.0021435656f,
      -0.0021470098f, -0.002150392f, -0.002153714f, -0.0021569782f },
    // [3] ladder pair 1, imaginary part, constant
    { -2.7221884e-06f, 0.00062028377f, 0.00063998386f, 0.0006472206f, 0.00065032573f, 0.0006515496f,
      0.00065177545f, 0.00065142266f, 0.0006507152f, 0.00064978306f, 0.0006487059f, 0.00064753497f,
      0.00064630416f, 0.0006450366f, 0.00064374827f, 0.0006424505f, 0.0006411515f, 0.00063985697f,
      0.00063857104f, 0.00063729676f, 0.0006360362f, 0.0006347909f, 0.0006335619f, 0.0006323498f,
      0.0006311551f, 0.00062997796f, 0.0006288184f, 0.0006276764f, 0.00062655174f, 0.0006254443f,
      0.0006243537f, 0.0006232798f, 0.0006222221f, 0.00062118034f, 0.00062015414f, 0.00061914325f,
      0.0006181472f, 0.00061716564f, 0.00061619823f, 0.0006152447f, 0.00061430456f, 0.0006133776f,
      0.0006124634f, 0.0006115617f, 0.00061067217f, 0.0006097945f, 0.00060892844f, 0.0006080736f,
      0.0006072298f, 0.0006063967f, 0.0006055741f, 0.0006047617f, 0.0006039593f, 0.00060316664f,
      0.00060238346f, 0.0006016096f, 0.0006008448f, 0.00060008885f, 0.0005993415f, 0.00059860275f,
      0.0005978722f, 0.0005971497f, 0.0005964352f, 0.0005957284f },
    // [4] ladder pair 1, real part, slope
    { -1.00014774f, -1.35336624f, -1.42048887f, -1.46551548f, -1.50035433f, -1.52916086f,
      -1.55392254f, -1.57575858f, -1.59536715f, -1.61321568f, -1.62963377f, -1.64486333f,
      -1.6590876f, -1.67244897f, -1.68506052f, -1.69701363f, -1.70838333f, -1.71923202f,
      -1.72961221f, -1.73956855f, -1.74913935f, -1.75835773f, -1.76725258f, -1.77584919f,
      -1.7841699f, -1.79223453f, -1.80006075f, -1.80766437f, -1.81505964f, -1.8222594f,
      -1.8292753f, -1.83611794f, -1.84279698f, -1.84932127f, -1.85569892f, -1.8619374f,
      -1.8680436f, -1.87402388f, -1.87988413f, -1.88562983f, -1.89126607f, -1.8967976f,
      -1.90222885f, -1.90756395f, -1.91280679f, -1.91796101f, -1.92303002f, -1.92801704f,
      -1.93292509f, -1.93775705f, -1.94251559f, -1.94720328f, -1.95182252f, -1.95637561f,
      -1.96086471f, -1.96529188f, -1.96965908f, -1.97396817f, -1.97822093f, -1.98241904f,
      -1.98656411f, -1.99065768f, -1.99470122f, -1.99869613f },
    // [5] ladder pair 1, imaginary part, slope
    { 0.000130592376f, 0.354780202f, 0.422050344f, 0.467149412f, 0.502032084f, 0.530867858f,
      0.55565017f, 0.577501296f, 0.597121154f, 0.614978238f, 0.631402872f, 0.64663744f,
      0.660865515f, 0.674229755f, 0.686843408f, 0.698798009f, 0.710168688f, 0.721017938f,
      0.731398341f, 0.741354603f, 0.750925074f, 0.760142923f, 0.769037045f, 0.777632782f,
      0.785952492f, 0.794016007f, 0.801841009f, 0.809443333f, 0.816837226f, 0.824035549f,
      0.831049962f, 0.837891065f, 0.844568531f, 0.851091211f, 0.857467223f, 0.86370404f,
      0.869808551f, 0.875787123f, 0.881645657f, 0.887389629f, 0.893024133f, 0.898553916f,
      0.903983409f, 0.909316756f, 0.914557836f, 0.919710291f, 0.92477754f, 0.9297628f,
      0.934669099f, 0.939499296f, 0.94425609f, 0.94894203f, 0.953559531f, 0.958110882f,
      0.96259825f, 0.967023698f, 0.971389181f, 0.975696562f, 0.979947614f, 0.984144025f,
      0.988287408f, 0.992379299f, 0.996421168f, 1.00041442f },
    // [6] ladder pair 2, real part, constant
    { -2.7214276e-06f, -0.000225824f, -0.0007515628f, -0.0010947591f, -0.0013447857f, -0.0015391731f,
      -0.001696913f, -0.0018288515f, -0.0019417249f, -0.0020399839f, -0.0021267121f, -0.0022041283f,
      -0.0022738783f, -0.0023372155f, -0.002395114f, -0.002448346f, -0.002497533f, -0.0025431828f,
      -0.002585714f, -0.0026254773f, -0.0026627681f, -0.0026978382f, -0.0027309032f, -0.00276215f,
      -0.0027917405f, -0.0028198168f, -0.0028465039f, -0.002871912f, -0.0028961396f, -0.0029192741f,
      -0.002941394f, -0.00296257f, -0.0029828656f, -0.0030023388f, -0.0030210416f, -0.0030390222f,
      -0.0030563239f, -0.0030729866f, -0.003089047f, -0.003104539f, -0.0031194934f, -0.003133939f,
      -0.0031479027f, -0.0031614087f, -0.00317448f, -0.0031871384f, -0.0031994032f, -0.0032112931f,
      -0.0032228255f, -0.0032340167f, -0.0032448818f, -0.003255435f, -0.0032656898f, -0.0032756592f,
      -0.0032853545f, -0.003294787f, -0.0033039676f, -0.003312906f, -0.003321612f, -0.0033300943f,
      -0.0033383612f, -0.003346421f, -0.0033542814f, -0.0033619497f },
    // [7] ladder pair 2, imaginary part, constant
    { -7.128533e-06f, 0.0028600153f, 0.0031400986f, 0.0032354735f, 0.003270355f, 0.0032790522f,
      0.0032747143f, 0.00326335f, 0.003248029f, 0.0032304446f, 0.003211585f, 0.0031920513f,
      0.0031722188f, 0.0031523283f, 0.0031325354f, 0.0031129413f, 0.0030936112f, 0.003074587f,
      0.0030558943f, 0.003037547f, 0.0030195517f, 0.0030019102f, 0.0029846197f, 0.0029676757f,
      0.0029510718f, 0.0029348f, 0.0029188525f, 0.00290322f, 0.0028878937f, 0.002872865f,
      0.0028581247f, 0.002843664f, 0.0028294744f, 0.0028155476f, 0.0028018756f, 0.0027884503f,
      0.0027752642f, 0.0027623104f, 0.0027495816f, 0.002737071f, 0.0027247723f, 0.002712679f,
      0.0027007856f, 0.0026890861f, 0.0026775748f, 0.0026662466f, 0.0026550966f, 0.0026441193f,
      0.0026333106f, 0.0026226658f, 0.0026121805f, 0.0026018505f, 0.002591672f, 0.0025816404f,
      0.0025717528f, 0.002562005f, 0.002552394f, 0.0025429162f, 0.0025335685f, 0.0025243475f,
      0.0025152506f, 0.0025062743f, 0.0024974165f, 0.002488674f },
    // [8] ladder pair 2, real part, slope
    { -0.999869423f, -0.638561407f, -0.56951453f, -0.523990915f, -0.48917678f, -0.460615628f,
      -0.436195579f, -0.414739573f, -0.395520699f, -0.378056805f, -0.362010728f, -0.347136887f,
      -0.333250504f, -0.320208824f, -0.307899106f, -0.296230641f, -0.285129278f, -0.274533563f,
      -0.264391946f, -0.254660728f, -0.245302512f, -0.236285026f, -0.227580207f, -0.219163487f,
      -0.211013226f, -0.203110249f, -0.195437482f, -0.187979648f, -0.180723016f, -0.173655197f,
      -0.166764971f, -0.160042136f, -0.153477393f, -0.147062234f, -0.140788856f, -0.13465008f,
      -0.128639289f, -0.122750366f, -0.116977645f, -0.111315866f, -0.105760138f, -0.1003059f,
      -0.094948896f, -0.0896851464f, -0.0845109223f, -0.079422726f, -0.0744172709f, -0.0694914651f,
      -0.0646423954f, -0.0598673139f, -0.055163625f, -0.0505288741f, -0.0459607376f, -0.0414570134f,
      -0.0370156122f, -0.0326345497f, -0.0283119399f, -0.024045988f, -0.0198349851f, -0.0156773019f,
      -0.0115713843f, -0.00751574873f, -0.00350897732f, 0.000450285508f },
    // [9] ladder pair 2, imaginary part, slope
    { 0.000113389002f, 0.350509549f, 0.419971782f, 0.46683576f, 0.50305379f, 0.532907131f,
      0.558475931f, 0.580942937f, 0.601050219f, 0.619296203f, 0.636032925f, 0.651518847f,
      0.665949666f, 0.67947733f, 0.692222311f, 0.704281836f, 0.715735567f, 0.726649641f,
      0.737079603f, 0.747072578f, 0.756668915f, 0.765903438f, 0.774806427f, 0.783404383f,
      0.791720644f, 0.799775871f, 0.80758845f, 0.815174821f, 0.822549745f, 0.829726527f,
      0.836717208f, 0.84353272f, 0.850183021f, 0.856677208f, 0.863023619f, 0.869229911f,
      0.875303138f, 0.881249811f, 0.887075954f, 0.892787154f, 0.8983886f, 0.903885123f,
      0.909281227f, 0.914581119f, 0.919788738f, 0.924907772f, 0.929941684f, 0.934893728f,
      0.939766966f, 0.944564285f, 0.949288407f, 0.953941905f, 0.958527211f, 0.96304663f,
      0.967502344f, 0.971896424f, 0.976230838f, 0.980507456f, 0.984728057f, 0.988894335f,
      0.993007906f, 0.99707031f, 1.00108302f, 1.00504744f },
};

// ----------------------------------------------------------------------------
// -- The ladder
// ----------------------------------------------------------------------------

// How many samples one coefficient computation is spread over. A power of two,
// so the caller's test is a mask; sixteen is where the pole maths stops
// dominating the voice without the glide lagging a sweep audibly.
const uint32_t kLadder303Stride = 16u;

// One conjugate pole pair, as a TPT state-variable filter with a tap mix that
// puts the zeros back at the origin. `_inc` is the per-sample walk toward the
// coefficients the last `ladder303_set` computed.
struct Section303 {
  float s1 = 0.f, s2 = 0.f;              // integrator state
  float g = 0.f, r = 0.f, dr = 1.f;      // live: prewarp, damping, 1/denominator
  float thp = 0.f, tbp = 0.f, tlp = 0.f; // live: the tap mix
  float g_i = 0.f, r_i = 0.f, dr_i = 0.f;
  float thp_i = 0.f, tbp_i = 0.f, tlp_i = 0.f;
};

struct Ladder303 {
  Section303 a;      // table pair 1
  Section303 b;      // table pair 2
  float hp_s = 0.f;  // feedback highpass state
  float hp_g = 0.f;  // its resolved integrator gain, g/(1+g)
  float hp_g_i = 0.f;
  float gain = 1.f;  // the resonance gain compensation
  float gain_i = 0.f;
  bool primed = false;  // false until the first `ladder303_set`, which snaps
};

// The coefficients of one section: what `ladder303_set` computes and what the
// live section walks toward.
struct Coef303 {
  float g, r, dr, thp, tbp, tlp;
};

// Place a section's poles at `exp(pr) * e^(+-i*pim)` -- the same reading of the
// table the direct form takes -- and solve the tap mix that cancels the SVF's
// own zeros.
//
// **Written in `1 - radius` and the half angle throughout.** The direct form's
// two numerators are `1 + a1 + a2` and `1 - a1 + a2` with `a1 = -2 r cos(th)`
// and `a2 = r^2`; both are differences of numbers near one for the resonant
// poles that matter, and in float both lose most of their significant digits
// there. The identities
//
//     1 + a1 + a2 = (1 - r)^2 + 4 r sin^2(th/2)
//     1 - a1 + a2 = (1 - r)^2 + 4 r cos^2(th/2)
//
// are the same two quantities with every subtraction removed -- a sum of
// non-negative terms, so nothing cancels at any radius or angle. `1 - a2`
// factors the same way, as `(1 - r)(1 + r)`. This is worth about four decimal
// digits exactly where the filter is sharpest, and it is why the coefficients
// here are closer to the exact poles than a float reference computing them the
// obvious way.
inline Coef303
coef303_from_pole(float pr, float pim) {
  const float m = -ntrk303_expm1(pr);        // 1 - radius, produced small
  const float rad = 1.f - m;
  float sh, ch;
  ntrk303_sincos_half(pim * 0.5f, &sh, &ch);

  const float m2 = m * m;
  const float four_r = 4.f * rad;
  const float num_dc = m2 + four_r * sh * sh;
  float num_nyq = m2 + four_r * ch * ch;
  if (num_nyq < 1.0e-20f)                    // unreachable: m > 0 always
    num_nyq = 1.0e-20f;

  float g = ntrk303_sqrt(num_dc / num_nyq);
  if (g < 1.0e-6f)
    g = 1.0e-6f;
  const float g2 = g * g;
  const float one_minus_a2 = m * (1.f + rad);          // (1 - r)(1 + r)
  const float one_plus_a2 = 1.f + rad * rad;
  const float r = (1.f + g2) * one_minus_a2 / (g * one_plus_a2);
  const float d_lead = 1.f + r * g + g2;     // >= 1 for any stable pole
  const float c = num_nyq * d_lead * 0.25f;
  Coef303 out;
  out.g = g;
  out.r = r;
  out.dr = 1.f / d_lead;
  out.thp = g2 * c;
  out.tbp = 2.f * g * c;
  out.tlp = c;
  return out;
}

inline void
section303_target(Section303 *s, const Coef303 &c, bool snap) {
  if (snap) {
    s->g = c.g; s->r = c.r; s->dr = c.dr;
    s->thp = c.thp; s->tbp = c.tbp; s->tlp = c.tlp;
    s->g_i = 0.f; s->r_i = 0.f; s->dr_i = 0.f;
    s->thp_i = 0.f; s->tbp_i = 0.f; s->tlp_i = 0.f;
    return;
  }
  const float k = 1.f / (float) kLadder303Stride;
  s->g_i = (c.g - s->g) * k;
  s->r_i = (c.r - s->r) * k;
  s->dr_i = (c.dr - s->dr) * k;
  s->thp_i = (c.thp - s->thp) * k;
  s->tbp_i = (c.tbp - s->tbp) * k;
  s->tlp_i = (c.tlp - s->tlp) * k;
}

inline float
section303_process(Section303 *s, float x) {
  s->g += s->g_i; s->r += s->r_i; s->dr += s->dr_i;
  s->thp += s->thp_i; s->tbp += s->tbp_i; s->tlp += s->tlp_i;

  const float g = s->g;
  const float hp = (x - (s->r + g) * s->s1 - s->s2) * s->dr;
  const float v1 = g * hp;
  const float bp = v1 + s->s1;
  const float v2 = g * bp;
  const float lp = v2 + s->s2;
  // Flushed at the store, as everywhere else here: a resonant tail decaying
  // into denormal range must not go on costing denormal cycles, and a
  // compare-and-select never perturbs a value that was not already rounding
  // to zero.
  s->s1 = flush_denorm(bp + v1);
  s->s2 = flush_denorm(lp + v2);
  return s->thp * hp + s->tbp * bp + s->tlp * lp;
}

inline void
ladder303_reset(Ladder303 *f) {
  f->a.s1 = 0.f; f->a.s2 = 0.f;
  f->b.s1 = 0.f; f->b.s2 = 0.f;
  f->hp_s = 0.f;
  f->primed = false;
}

// Read the table at `(cutoff_hz, reso)` and start the sixteen-sample walk
// toward the coefficients it names. `reso` is the *mapped* resonance the table
// is indexed by, 0..2.52 across its 64 rows; see `synth303_reso` for the knob
// law that produces it.
//
// The first call after a reset snaps rather than glides -- there is nothing to
// glide from, and a note that spent its first sixteen samples walking out of
// whatever the last one left would open with a click.
inline void
ladder303_set(Ladder303 *f, float cutoff_hz, float reso, float sample_rate) {
  const float sr = sample_rate > 1.f ? sample_rate : 1.f;
  float fc = cutoff_hz;
  const float fc_max = sr * 0.45f;
  if (!(fc > 20.f))            // false for NaN, which must not reach the table
    fc = 20.f;
  if (fc > fc_max)
    fc = fc_max;
  const float w = 6.28318531f * fc / sr;

  float rs = reso;
  if (!(rs > 0.f))
    rs = 0.f;
  if (rs > 10.f)
    rs = 10.f;
  float rf = rs * 25.f;
  const float rf_max = (float) (kNtrk303ResoLevels - 1);
  if (rf > rf_max)
    rf = rf_max;
  const int i0 = (int) rf;
  const int i1 = i0 < kNtrk303ResoLevels - 1 ? i0 + 1 : i0;
  const float t = rf - (float) i0;
  const float u = 1.f - t;

  // Every pole is `constant + w * slope`, both ends interpolated between the
  // two resonance rows the setting falls between -- without that interpolation
  // the 64 levels step audibly under a resonance sweep.
  //
  // **The constants are per sample at `kNtrk303TableRate`**, so they are
  // rescaled to this rate; the slopes ride on `w`, which already is. Unscaled,
  // the feedback highpass's corner doubles with the voice's 2x and thins the
  // bottom octave by ten decibels.
  const float k = kNtrk303TableRate / sr;
  const float p0 = k * (u * kNtrk303Poles[0][i0] + t * kNtrk303Poles[0][i1]) +
                   w * (u * kNtrk303Poles[1][i0] + t * kNtrk303Poles[1][i1]);
  float p1r = k * (u * kNtrk303Poles[2][i0] + t * kNtrk303Poles[2][i1]) +
              w * (u * kNtrk303Poles[4][i0] + t * kNtrk303Poles[4][i1]);
  float p1i = k * (u * kNtrk303Poles[3][i0] + t * kNtrk303Poles[3][i1]) +
              w * (u * kNtrk303Poles[5][i0] + t * kNtrk303Poles[5][i1]);
  float p2r = k * (u * kNtrk303Poles[6][i0] + t * kNtrk303Poles[6][i1]) +
              w * (u * kNtrk303Poles[8][i0] + t * kNtrk303Poles[8][i1]);
  float p2i = k * (u * kNtrk303Poles[7][i0] + t * kNtrk303Poles[7][i1]) +
              w * (u * kNtrk303Poles[9][i0] + t * kNtrk303Poles[9][i1]);

  // The fit is linear in w from measurements taken at w far below one. Opened
  // right up -- which `env_mod` does -- the extrapolation walks the pole angle
  // toward pi, where it folds and rings inharmonically, and the real parts
  // lose their stability margin. Both guards are no-ops inside the fitted
  // range and only bite where the table is being asked to extrapolate.
  const float im_max = 2.8f;
  if (p1i > im_max) p1i = im_max;
  if (p2i > im_max) p2i = im_max;
  const float re_max = -1.0e-6f;
  float ph = p0 < re_max ? p0 : re_max;
  if (p1r > re_max) p1r = re_max;
  if (p2r > re_max) p2r = re_max;

  const bool snap = !f->primed;
  section303_target(&f->a, coef303_from_pole(p1r, p1i), snap);
  section303_target(&f->b, coef303_from_pole(p2r, p2i), snap);

  // The feedback highpass, as a TPT one-pole placed at the same pole. Its
  // integrator gain is `(1 - exp(p0)) / (1 + exp(p0))`, another difference of
  // near-ones -- so it too is written through `expm1`, as `-t / (2 + t)`, and
  // the resolved `g/(1+g)` is what gets glided rather than `g` itself, which
  // keeps a divide out of the per-sample path.
  const float t0 = ntrk303_expm1(ph);
  const float hg = -t0 / (2.f + t0);
  const float hp_g = hg / (1.f + hg);

  // Resonance gain compensation, the same law the measurement was taken with:
  // the ladder's passband loses level as the feedback rises, and this is what
  // puts it back without letting a resonant peak run away.
  const float gain = 1.3f / (1.f + rs * 4.f) + 0.3f * rs;

  if (snap) {
    f->hp_g = hp_g; f->hp_g_i = 0.f;
    f->gain = gain; f->gain_i = 0.f;
    f->primed = true;
  } else {
    const float k = 1.f / (float) kLadder303Stride;
    f->hp_g_i = (hp_g - f->hp_g) * k;
    f->gain_i = (gain - f->gain) * k;
  }
}

// Input -> feedback highpass -> gain -> the two ladder pairs. Eighteen dB an
// octave and a highpass in the loop, which between them are the sound.
inline float
ladder303_process(Ladder303 *f, float x) {
  f->hp_g += f->hp_g_i;
  f->gain += f->gain_i;

  const float v = (x - f->hp_s) * f->hp_g;
  const float lp = v + f->hp_s;
  f->hp_s = flush_denorm(lp + v);
  const float y = f->gain * (x - lp);

  return section303_process(&f->b, section303_process(&f->a, y));
}

// ----------------------------------------------------------------------------
// -- Two times oversampling
// ----------------------------------------------------------------------------

// The voice runs at twice the output rate: the oscillator's edges, the
// resonant peak near the top of the band and both shapers all put energy above
// the output Nyquist, and at 1x every bit of it folds back as inharmonic
// grit. At 2x the ladder's `w` also halves, which keeps the pole fit -- linear
// in `w`, measured at small `w` -- inside the range it was measured over.
//
// **A 47-tap halfband FIR, Kaiser beta 8**: flat (0.001 dB) to 0.1875 of the
// oversampled rate, -81 dB from 0.3125 -- at a 48 kHz output, flat to 18 kHz,
// and whatever folds back lands above 18 kHz and 81 dB down. Every other tap of
// a halfband is zero, so the even-phase sub-samples meet twelve symmetric pairs
// and the odd phase meets only the centre tap. Latency 11.5 output frames.
const int kDecim303Taps = 12;   // one side of the non-zero even-phase taps
const float kDecim303Coef[kDecim303Taps] = {
    0.316062936f, -0.0995345836f, 0.0532395992f, -0.031906212f,
    0.0195116826f, -0.0116853841f, 0.00667084758f, -0.00353946785f,
    0.00169065103f, -0.000690003601f, 0.000214604257f, -3.23680878e-05f,
};
const float kDecim303Centre = 0.499995397f;

struct Decim303 {
  float even[2 * kDecim303Taps] = {};  // the second sub-sample of each frame
  float odd[kDecim303Taps] = {};       // the first; only the centre reads it
  uint32_t at = 0u;
};

inline void
decim303_reset(Decim303 *d) {
  for (int i = 0; i < 2 * kDecim303Taps; ++i)
    d->even[i] = 0.f;
  for (int i = 0; i < kDecim303Taps; ++i)
    d->odd[i] = 0.f;
  d->at = 0u;
}

// One output frame from its two sub-samples, `first` earlier in time.
inline float
decim303_process(Decim303 *d, float first, float second) {
  const uint32_t n = 2u * (uint32_t) kDecim303Taps;   // 24, a multiple of 12
  d->at = (d->at + 1u) % n;
  d->even[d->at] = second;
  d->odd[d->at % (uint32_t) kDecim303Taps] = first;
  // The coefficients run from the centre outward: tap i pairs the frames
  // 11 - i and 12 + i back, which straddle the centre symmetrically.
  float y = 0.f;
  for (uint32_t i = 0; i < (uint32_t) kDecim303Taps; ++i)
    y += kDecim303Coef[i] * (d->even[(d->at + n - (11u - i)) % n] +
                             d->even[(d->at + n - (12u + i)) % n]);
  // The centre is the first sub-sample eleven frames back.
  return y + kDecim303Centre * d->odd[(d->at + 1u) % (uint32_t) kDecim303Taps];
}

}  // namespace fx
}  // namespace ntrk

#endif  // NTRK_303_H_
