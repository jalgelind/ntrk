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
// fit to a real x0xb0x (https://github.com/a1k0n/303), copied unchanged: for
// each of 64 resonance settings it gives five poles as `constant + slope * w`,
// where `w = 2*pi*fc/sr`. One first-order pole for the feedback highpass and
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
const int kNtrk303ResoLevels = 64;
const float kNtrk303Poles[10][kNtrk303ResoLevels] = {
    // [0] feedback highpass pole, constant
    { -0.0142475857f, -0.0110558351f, -0.00958097367f, -0.00863568249f, -0.00794942757f, -0.0074157056f,
      -0.00698187179f, -0.00661819537f, -0.00630631927f, -0.00603415378f, -0.00579333654f, -0.00557785533f,
      -0.00538325013f, -0.00520612558f, -0.00504383985f, -0.00489429884f, -0.00475581571f, -0.00462701254f,
      -0.00450674977f, -0.0043940746f, -0.00428818259f, -0.00418838855f, -0.00409410427f, -0.00400482112f,
      -0.00392009643f, -0.00383954259f, -0.00376281836f, -0.00368962181f, -0.00361968451f, -0.00355276681f,
      -0.00348865386f, -0.00342715236f, -0.00336808777f, -0.00331130196f, -0.00325665127f, -0.00320400476f,
      -0.00315324279f, -0.00310425577f, -0.00305694308f, -0.00301121207f, -0.00296697733f, -0.00292415989f,
      -0.00288268665f, -0.00284248977f, -0.00280350622f, -0.00276567732f, -0.00272894836f, -0.00269326825f,
      -0.00265858922f, -0.00262486654f, -0.00259205824f, -0.00256012496f, -0.00252902967f, -0.00249873752f,
      -0.0024692157f, -0.00244043324f, -0.00241236091f, -0.00238497108f, -0.00235823762f, -0.00233213577f,
      -0.00230664208f, -0.0022817343f, -0.0022573913f, -0.00223359302f },
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
    { -1.83545593e-06f, -0.00135008051f, -0.00151527847f, -0.00161437715f, -0.00168536679f, -0.00174064961f,
      -0.00178587681f, -0.00182410854f, -0.00185719118f, -0.00188632533f, -0.00191233586f, -0.00193581405f,
      -0.00195719818f, -0.00197682215f, -0.00199494618f, -0.002011777f, -0.00202748155f, -0.00204219657f,
      -0.00205603546f, -0.00206909331f, -0.00208145062f, -0.00209317612f, -0.00210432901f, -0.00211496056f,
      -0.00212511553f, -0.00213483321f, -0.00214414822f, -0.00215309131f, -0.00216168985f, -0.0021699683f,
      -0.00217794867f, -0.00218565078f, -0.00219309254f, -0.00220029023f, -0.00220725864f, -0.0022140113f,
      -0.00222056055f, -0.00222691775f, -0.00223309332f, -0.00223909688f, -0.0022449373f, -0.0022506228f,
      -0.00225616099f, -0.00226155896f, -0.00226682328f, -0.0022719601f, -0.00227697514f, -0.00228187376f,
      -0.00228666097f, -0.00229134148f, -0.0022959197f, -0.00230039977f, -0.00230478562f, -0.00230908091f,
      -0.00231328911f, -0.00231741351f, -0.00232145721f, -0.00232542313f, -0.00232931406f, -0.00233313263f,
      -0.00233688133f, -0.00234056255f, -0.00234417854f, -0.00234773145f },
    // [3] ladder pair 1, imaginary part, constant
    { -2.96292613e-06f, 0.000675138822f, 0.00069658105f, 0.000704457808f, 0.000707837502f, 0.000709169651f,
      0.00070941548f, 0.000709031433f, 0.000708261454f, 0.000707246872f, 0.000706074484f, 0.000704799978f,
      0.000703460301f, 0.000702080606f, 0.000700678368f, 0.000699265907f, 0.000697852005f, 0.000696442963f,
      0.000695043317f, 0.000693656323f, 0.000692284301f, 0.000690928882f, 0.000689591181f, 0.000688271928f,
      0.000686971561f, 0.0006856903f, 0.000684428197f, 0.000683185182f, 0.000681961088f, 0.00068075568f,
      0.000679568668f, 0.000678399727f, 0.000677248505f, 0.000676114631f, 0.000674997722f, 0.000673897392f,
      0.000672813249f, 0.000671744904f, 0.000670691972f, 0.000669654071f, 0.000668630828f, 0.000667621875f,
      0.000666626854f, 0.000665645417f, 0.000664677222f, 0.00066372194f, 0.000662779248f, 0.000661848835f,
      0.000660930398f, 0.000660023644f, 0.00065912829f, 0.000658244058f, 0.000657370684f, 0.000656507909f,
      0.000655655483f, 0.000654813164f, 0.000653980718f, 0.000653157918f, 0.000652344545f, 0.000651540387f,
      0.000650745236f, 0.000649958895f, 0.000649181169f, 0.000648411873f },
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
    { -2.96209812e-06f, -0.000245794824f, -0.000818027564f, -0.00119157447f, -0.00146371229f, -0.00167529045f,
      -0.00184698016f, -0.00199058664f, -0.00211344205f, -0.00222039065f, -0.00231478873f, -0.00239905115f,
      -0.00247496962f, -0.00254390793f, -0.00260692676f, -0.00266486645f, -0.00271840346f, -0.00276809003f,
      -0.00281438252f, -0.00285766225f, -0.00289825096f, -0.00293642247f, -0.00297241172f, -0.00300642174f,
      -0.00303862912f, -0.00306918837f, -0.00309823546f, -0.00312589065f, -0.00315226077f, -0.00317744116f,
      -0.00320151726f, -0.00322456591f, -0.00324665644f, -0.00326785166f, -0.00328820859f, -0.00330777919f,
      -0.00332661092f, -0.00334474723f, -0.003362228f, -0.00337908995f, -0.0033953669f, -0.00341109012f,
      -0.00342628855f, -0.00344098902f, -0.00345521647f, -0.0034689941f, -0.00348234354f, -0.00349528498f,
      -0.00350783728f, -0.00352001812f, -0.00353184405f, -0.00354333061f, -0.00355449241f, -0.0035653432f,
      -0.0035758959f, -0.00358616273f, -0.0035961552f, -0.00360588419f, -0.00361536f, -0.00362459235f,
      -0.00363359049f, -0.00364236316f, -0.00365091867f, -0.00365926491f },
    // [7] ladder pair 2, imaginary part, constant
    { -7.7589475e-06f, 0.00311294169f, 0.00341779455f, 0.00352160375f, 0.00355957019f, 0.00356903631f,
      0.00356431495f, 0.0035519457f, 0.00353526954f, 0.00351613008f, 0.00349560287f, 0.00347434152f,
      0.00345275527f, 0.00343110577f, 0.00340956242f, 0.0033882354f, 0.00336719598f, 0.00334648945f,
      0.00332614343f, 0.00330617351f, 0.00328658692f, 0.00326738515f, 0.00324856568f, 0.0032301233f,
      0.00321205091f, 0.00319434023f, 0.00317698219f, 0.00315996727f, 0.00314328577f, 0.00312692791f,
      0.003110884f, 0.00309514449f, 0.00307970007f, 0.00306454165f, 0.00304966043f, 0.0030350479f,
      0.00302069585f, 0.00300659636f, 0.0029927418f, 0.00297912486f, 0.00296573849f, 0.0029525759f,
      0.00293963061f, 0.00292689635f, 0.00291436713f, 0.00290203718f, 0.00288990095f, 0.00287795312f,
      0.00286618855f, 0.00285460234f, 0.00284318974f, 0.00283194618f, 0.00282086729f, 0.00280994883f,
      0.00279918673f, 0.00278857707f, 0.00277811607f, 0.00276780009f, 0.00275762559f, 0.00274758919f,
      0.00273768761f, 0.00272791768f, 0.00271827634f, 0.00270876064f },
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
  const float p0 = u * kNtrk303Poles[0][i0] + t * kNtrk303Poles[0][i1] +
                   w * (u * kNtrk303Poles[1][i0] + t * kNtrk303Poles[1][i1]);
  float p1r = u * kNtrk303Poles[2][i0] + t * kNtrk303Poles[2][i1] +
              w * (u * kNtrk303Poles[4][i0] + t * kNtrk303Poles[4][i1]);
  float p1i = u * kNtrk303Poles[3][i0] + t * kNtrk303Poles[3][i1] +
              w * (u * kNtrk303Poles[5][i0] + t * kNtrk303Poles[5][i1]);
  float p2r = u * kNtrk303Poles[6][i0] + t * kNtrk303Poles[6][i1] +
              w * (u * kNtrk303Poles[8][i0] + t * kNtrk303Poles[8][i1]);
  float p2i = u * kNtrk303Poles[7][i0] + t * kNtrk303Poles[7][i1] +
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

}  // namespace fx
}  // namespace ntrk

#endif  // NTRK_303_H_
