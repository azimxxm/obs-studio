#pragma once

#include <cstdint>
#include <vector>

/* Offline voice effects for the audio editor. Every function works on
 * interleaved float samples at 48 kHz and changes them in place. */
namespace VoiceEffects {

constexpr int kSampleRate = 48000;

struct Range {
	int64_t start = 0; // first frame, inclusive
	int64_t end = 0;   // last frame, exclusive
};

/* Removes rumble below cutoffHz (desk knocks, air conditioner, mic handling). */
void HighPass(std::vector<float> &samples, int channels, float cutoffHz);

/* Makes quiet and loud words closer in volume. */
void Compress(std::vector<float> &samples, int channels, float thresholdDb, float ratio, float attackMs,
	      float releaseMs);

/* Keeps every sample under ceilingDb without audible clipping. */
void Limit(std::vector<float> &samples, int channels, float ceilingDb);

/* Integrated loudness in LUFS (ITU-R BS.1770), or a very low value for silence. */
double MeasureLoudness(const std::vector<float> &samples, int channels);

/* Sets the loudness to targetLufs and limits peaks to ceilingDb. */
bool NormalizeLoudness(std::vector<float> &samples, int channels, double targetLufs, float ceilingDb);

/* Removes steady background noise with RNNoise. Returns false when not built in. */
bool RemoveNoise(std::vector<float> &samples, int channels);
bool CanRemoveNoise();

/* Changes the volume of [from, to) by gainDb, with short ramps at the edges
 * so the change does not click. */
void ChangeVolume(std::vector<float> &samples, int channels, int64_t from, int64_t to, float gainDb);

/* Spectral noise removal: learns the noise from the quietest parts of the
 * recording (like Audacity's noise profile, but automatic) and lowers it in
 * 512 narrow bands. reductionDb is how far noise may be pushed down. */
void CleanNoiseSpectral(std::vector<float> &samples, int channels, float reductionDb);

/* Smooth volume ramp over [from, to). */
void Fade(std::vector<float> &samples, int channels, int64_t from, int64_t to, bool fadeIn);

/* Downward expander: pushes the gaps between words down by up to
 * maxReductionDb, so noise that compression lifted goes quiet again. */
void QuietPauses(std::vector<float> &samples, int channels, float maxReductionDb);

/* The one-button chain: noise, rumble, even volume, then the loudness target. */
void PolishVoice(std::vector<float> &samples, int channels, double targetLufs);

/* Parts to cut so every pause longer than minPauseSec becomes keepSec long. */
std::vector<Range> FindLongPauses(const std::vector<float> &samples, int channels, double minPauseSec,
				  double keepSec);

} // namespace VoiceEffects
