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
void CleanNoiseSpectral(std::vector<float> &samples, int channels, float reductionDb,
			const std::vector<Range> &noiseRanges = {});

/* Smooth volume ramp over [from, to). */
void Fade(std::vector<float> &samples, int channels, int64_t from, int64_t to, bool fadeIn);

/* Downward expander: pushes the gaps between words down by up to
 * maxReductionDb, so noise that compression lifted goes quiet again. */
void QuietPauses(std::vector<float> &samples, int channels, float maxReductionDb);

/* The one-button chain: noise, rumble, even volume, then the loudness target. */
void PolishVoice(std::vector<float> &samples, int channels, double targetLufs,
		 const std::vector<Range> &noiseRanges = {});

/* Every quiet stretch of at least minPauseSec, including the start and end. */
std::vector<Range> FindPauses(const std::vector<float> &samples, int channels, double minPauseSec);

/* For each mistake mark (pressed after misreading), the part to cut: from the
 * pause before the wrong sentence to where the narrator starts it again. */
std::vector<Range> MistakeCuts(const std::vector<float> &samples, int channels, const std::vector<int64_t> &marks);

/* Parts to cut so every pause longer than minPauseSec becomes keepSec long. */
std::vector<Range> FindLongPauses(const std::vector<float> &samples, int channels, double minPauseSec,
				  double keepSec);

/* Lowers breaths between phrases by reductionDb; returns how many were found. */
int ReduceBreaths(std::vector<float> &samples, int channels, float reductionDb);

/* Softens harsh "s" and "sh" sounds by turning down only the top end while they last. */
void DeEss(std::vector<float> &samples, int channels, float maxReductionDb);

/* Finds very short clicks (mouth, lips, tongue) and smooths them over; returns the count. */
int RemoveClicks(std::vector<float> &samples, int channels);

/* Audiobook (ACX / Audible) mastering and check. */
struct AcxReport {
	double rmsDb = 0.0;        // must be -23 .. -18
	double peakDb = 0.0;       // must be at most -3
	double noiseFloorDb = 0.0; // must be at most -60
	double headSec = 0.0;      // room tone at the start, 0.5 .. 1
	double tailSec = 0.0;      // room tone at the end, 1 .. 5
	bool RmsOk() const { return rmsDb >= -23.0 && rmsDb <= -18.0; }
	bool PeakOk() const { return peakDb <= -3.0; }
	bool NoiseOk() const { return noiseFloorDb <= -60.0; }
	bool HeadOk() const { return headSec >= 0.5 && headSec <= 1.0; }
	bool TailOk() const { return tailSec >= 1.0 && tailSec <= 5.0; }
	bool Passed() const { return RmsOk() && PeakOk() && NoiseOk() && HeadOk() && TailOk(); }
};

/* sets RMS to -20 dB, peaks to -3.5 dB and the room tone at both ends */
void MasterForAudiobook(std::vector<float> &samples, int channels, const std::vector<Range> &roomTone);
AcxReport CheckAudiobook(const std::vector<float> &samples, int channels);

} // namespace VoiceEffects
