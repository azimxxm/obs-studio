#include "VoiceEffects.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>

#ifdef HAVE_RNNOISE
#include <rnnoise.h>
#endif

namespace VoiceEffects {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSilenceLufs = -120.0;

double DbToGain(double db)
{
	return std::pow(10.0, db / 20.0);
}

double GainToDb(double gain)
{
	return gain > 0.0 ? 20.0 * std::log10(gain) : -200.0;
}

/* Direct form I biquad, one per channel. */
struct Biquad {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

	double Process(double x)
	{
		double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		return y;
	}
};

/* RBJ cookbook high-pass with Q = 1/sqrt(2) (Butterworth) */
Biquad MakeHighPass(double cutoffHz)
{
	double w0 = 2.0 * kPi * cutoffHz / kSampleRate;
	double alpha = std::sin(w0) / (2.0 * std::sqrt(0.5));
	double cosw = std::cos(w0);
	double a0 = 1.0 + alpha;

	Biquad f;
	f.b0 = (1.0 + cosw) / 2.0 / a0;
	f.b1 = -(1.0 + cosw) / a0;
	f.b2 = (1.0 + cosw) / 2.0 / a0;
	f.a1 = -2.0 * cosw / a0;
	f.a2 = (1.0 - alpha) / a0;
	return f;
}

int64_t Frames(const std::vector<float> &samples, int channels)
{
	return channels > 0 ? int64_t(samples.size()) / channels : 0;
}

float FramePeak(const std::vector<float> &samples, int channels, int64_t frame)
{
	float peak = 0.0f;
	for (int c = 0; c < channels; c++) {
		peak = std::max(peak, std::fabs(samples[size_t(frame * channels + c)]));
	}
	return peak;
}
/* Exponential integral E1(x), needed by the log-spectral gain. */
double ExpIntegral(double x)
{
	if (x < 1.0) {
		/* power series, converges fast below 1 */
		double sum = 0.0, term = 1.0;
		for (int k = 1; k < 30; k++) {
			term *= -x / k;
			sum -= term / k;
		}
		return -0.5772156649015329 - std::log(x) + sum;
	}
	/* rational approximation (Abramowitz & Stegun 5.1.56) */
	double num = x * x + 2.334733 * x + 0.250621;
	double den = x * x + 3.330657 * x + 1.681534;
	return std::exp(-x) / x * num / den;
}

/* In-place radix-2 FFT, size must be a power of two. */
void Fft(std::vector<std::complex<double>> &data, bool inverse)
{
	const size_t n = data.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(data[i], data[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		double angle = 2.0 * kPi / double(len) * (inverse ? 1.0 : -1.0);
		std::complex<double> step(std::cos(angle), std::sin(angle));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (size_t k = 0; k < len / 2; k++) {
				std::complex<double> u = data[i + k];
				std::complex<double> v = data[i + k + len / 2] * w;
				data[i + k] = u + v;
				data[i + k + len / 2] = u - v;
				w *= step;
			}
		}
	}
	if (inverse) {
		for (auto &x : data) {
			x /= double(n);
		}
	}
}
} // namespace

void CleanNoiseSpectral(std::vector<float> &samples, int channels, float reductionDb,
			const std::vector<Range> &noiseRanges)
{
	/* a known stretch of room sound is the best noise profile there is */
	constexpr int64_t kMinRoomToneFrames = kSampleRate * 3 / 10;
	constexpr size_t kFft = 1024; // 21 ms, fine enough to keep voice harmonics apart
	constexpr size_t kHop = kFft / 4;
	constexpr size_t kBins = kFft / 2 + 1;
	/* decision-directed smoothing (Ephraim & Malah); higher = less "musical noise" */
	constexpr double kSmoothing = 0.98;
	/* share of the quietest frames used to learn what the noise sounds like */
	constexpr double kNoiseFrameShare = 0.15;

	const int64_t frames = Frames(samples, channels);
	if (frames < int64_t(kFft) * 4) {
		return;
	}
	const double floorGain = DbToGain(-std::fabs(reductionDb));

	std::vector<double> window(kFft);
	for (size_t i = 0; i < kFft; i++) {
		window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(kFft));
	}
	/* Hann analysis + synthesis at 75% overlap adds up to 1.5 everywhere */
	constexpr double kOverlapGain = 1.5;

	const size_t pad = kFft;
	const size_t padded = size_t(frames) + 2 * pad;
	const size_t stftFrames = (padded - kFft) / kHop + 1;

	for (int c = 0; c < channels; c++) {
		std::vector<double> input(padded, 0.0);
		for (int64_t f = 0; f < frames; f++) {
			input[pad + size_t(f)] = samples[size_t(f * channels + c)];
		}

		/* spectrum of one frame, computed when needed so long recordings
		 * don't keep every frame in memory */
		std::vector<std::complex<double>> buffer(kFft);
		auto spectrumAt = [&](size_t t, std::vector<std::complex<double>> &out) {
			for (size_t i = 0; i < kFft; i++) {
				out[i] = input[t * kHop + i] * window[i];
			}
			Fft(out, false);
		};

		/* 1. how much voice could be in every frame: energy in the speech band
		 * only, so low rumble that comes and goes doesn't decide what is quiet */
		const size_t speechLow = size_t(300.0 * kFft / kSampleRate);
		const size_t speechHigh = size_t(4000.0 * kFft / kSampleRate);
		std::vector<double> frameEnergy(stftFrames, 0.0);
		for (size_t t = 0; t < stftFrames; t++) {
			spectrumAt(t, buffer);
			for (size_t k = speechLow; k <= speechHigh; k++) {
				frameEnergy[t] += std::norm(buffer[k]);
			}
		}

		/* 2. noise profile: average power of the quietest frames that are not
		 * the digital silence we padded with */
		std::vector<size_t> order;
		int64_t roomToneFrames = 0;
		for (const Range &range : noiseRanges) {
			roomToneFrames += range.end - range.start;
		}
		if (roomToneFrames >= kMinRoomToneFrames) {
			/* frames that lie completely inside the room tone */
			for (size_t t = 0; t < stftFrames; t++) {
				int64_t first = int64_t(t * kHop) - int64_t(pad);
				int64_t last = first + int64_t(kFft);
				for (const Range &range : noiseRanges) {
					if (first >= range.start && last <= range.end) {
						order.push_back(t);
						break;
					}
				}
			}
		}
		size_t noiseFrames = order.size();
		if (noiseFrames < 4) {
			order.clear();
			for (size_t t = 0; t < stftFrames; t++) {
				if (frameEnergy[t] > 1e-14) {
					order.push_back(t);
				}
			}
			if (order.size() < 8) {
				continue;
			}
			std::sort(order.begin(), order.end(),
				  [&](size_t a, size_t b) { return frameEnergy[a] < frameEnergy[b]; });
			noiseFrames = std::max<size_t>(4, size_t(double(order.size()) * kNoiseFrameShare));
		}
		std::vector<double> noise(kBins, 0.0);
		for (size_t i = 0; i < noiseFrames; i++) {
			spectrumAt(order[i], buffer);
			for (size_t k = 0; k < kBins; k++) {
				noise[k] += std::norm(buffer[k]);
			}
		}
		for (double &n : noise) {
			n = n / double(noiseFrames) + 1e-20;
		}

		/* 3. Wiener gain per band, smoothed over time and across neighbours */
		std::vector<double> previousClean(kBins, 0.0);
		std::vector<double> gain(kBins), smoothGain(kBins);
		std::vector<double> output(padded, 0.0);
		std::vector<std::complex<double>> spectrum(kFft);
		for (size_t t = 0; t < stftFrames; t++) {
			spectrumAt(t, spectrum);
			for (size_t k = 0; k < kBins; k++) {
				double power = std::norm(spectrum[k]);
				double posteriori = power / noise[k];
				double priori = kSmoothing * previousClean[k] / noise[k] +
						(1.0 - kSmoothing) * std::max(posteriori - 1.0, 0.0);
				/* MMSE log-spectral amplitude gain: removes as much noise as a
				 * Wiener filter but keeps quiet parts of the voice better */
				double wiener = priori / (1.0 + priori);
				double v = std::max(wiener * posteriori, 1e-10);
				double lsa = wiener * std::exp(0.5 * ExpIntegral(v));
				gain[k] = std::clamp(lsa, floorGain, 1.0);
				previousClean[k] = gain[k] * gain[k] * power;
			}
			/* a 3-band average stops single bands from blinking on and off */
			for (size_t k = 0; k < kBins; k++) {
				double left = gain[k > 0 ? k - 1 : k];
				double right = gain[k + 1 < kBins ? k + 1 : k];
				smoothGain[k] = 0.25 * left + 0.5 * gain[k] + 0.25 * right;
			}

			for (size_t k = 0; k < kBins; k++) {
				buffer[k] = spectrum[k] * smoothGain[k];
			}
			for (size_t k = kBins; k < kFft; k++) {
				buffer[k] = std::conj(buffer[kFft - k]);
			}
			Fft(buffer, true);
			for (size_t i = 0; i < kFft; i++) {
				output[t * kHop + i] += buffer[i].real() * window[i] / kOverlapGain;
			}
		}

		for (int64_t f = 0; f < frames; f++) {
			samples[size_t(f * channels + c)] = float(output[pad + size_t(f)]);
		}
	}
}

void HighPass(std::vector<float> &samples, int channels, float cutoffHz)
{
	/* two stages give a steep 24 dB/octave slope that leaves the voice alone */
	for (int c = 0; c < channels; c++) {
		Biquad first = MakeHighPass(cutoffHz);
		Biquad second = MakeHighPass(cutoffHz);
		for (size_t i = size_t(c); i < samples.size(); i += size_t(channels)) {
			samples[i] = float(second.Process(first.Process(samples[i])));
		}
	}
}

void Compress(std::vector<float> &samples, int channels, float thresholdDb, float ratio, float attackMs,
	      float releaseMs)
{
	constexpr double kKneeDb = 6.0;
	const double attack = std::exp(-1.0 / (attackMs * 0.001 * kSampleRate));
	const double release = std::exp(-1.0 / (releaseMs * 0.001 * kSampleRate));
	const int64_t frames = Frames(samples, channels);

	double envelopeDb = -120.0;
	for (int64_t f = 0; f < frames; f++) {
		double levelDb = GainToDb(FramePeak(samples, channels, f));
		double coef = levelDb > envelopeDb ? attack : release;
		envelopeDb = coef * envelopeDb + (1.0 - coef) * levelDb;

		/* soft knee: the reduction fades in over kKneeDb around the threshold */
		double over = envelopeDb - thresholdDb;
		double reductionDb = 0.0;
		if (over > kKneeDb / 2) {
			reductionDb = over * (1.0 - 1.0 / ratio);
		} else if (over > -kKneeDb / 2) {
			double x = over + kKneeDb / 2;
			reductionDb = (1.0 - 1.0 / ratio) * x * x / (2.0 * kKneeDb);
		}

		float gain = float(DbToGain(-reductionDb));
		for (int c = 0; c < channels; c++) {
			samples[size_t(f * channels + c)] *= gain;
		}
	}
}

void Limit(std::vector<float> &samples, int channels, float ceilingDb)
{
	const int64_t frames = Frames(samples, channels);
	if (frames == 0) {
		return;
	}

	constexpr double kLookaheadMs = 5.0;
	constexpr double kReleaseMs = 80.0;
	const int64_t lookahead = int64_t(kLookaheadMs * 0.001 * kSampleRate);
	const double release = std::exp(-1.0 / (kReleaseMs * 0.001 * kSampleRate));
	const float ceiling = float(DbToGain(ceilingDb));

	/* gain each frame needs on its own */
	std::vector<float> needed(static_cast<size_t>(frames));
	for (int64_t f = 0; f < frames; f++) {
		float peak = FramePeak(samples, channels, f);
		needed[size_t(f)] = peak > ceiling ? ceiling / peak : 1.0f;
	}

	/* look ahead: the smallest gain needed in the next few ms (sliding minimum) */
	std::vector<float> ahead(static_cast<size_t>(frames));
	std::deque<int64_t> window;
	for (int64_t f = frames - 1; f >= 0; f--) {
		while (!window.empty() && needed[size_t(window.back())] >= needed[size_t(f)]) {
			window.pop_back();
		}
		window.push_back(f);
		while (window.front() > f + lookahead) {
			window.pop_front();
		}
		ahead[size_t(f)] = needed[size_t(window.front())];
	}

	/* average over the same window so the gain ramps down instead of jumping;
	 * every value in it is already low enough, so the peak still fits */
	std::vector<double> prefix(size_t(frames) + 1, 0.0);
	for (int64_t f = 0; f < frames; f++) {
		prefix[size_t(f + 1)] = prefix[size_t(f)] + ahead[size_t(f)];
	}

	double gain = 1.0;
	for (int64_t f = 0; f < frames; f++) {
		int64_t first = std::max<int64_t>(0, f - lookahead);
		double smooth = (prefix[size_t(f + 1)] - prefix[size_t(first)]) / double(f + 1 - first);
		gain = smooth < gain ? smooth : release * gain + (1.0 - release) * smooth;

		for (int c = 0; c < channels; c++) {
			float &s = samples[size_t(f * channels + c)];
			s = std::clamp(float(s * gain), -ceiling, ceiling);
		}
	}
}

double MeasureLoudness(const std::vector<float> &samples, int channels)
{
	const int64_t frames = Frames(samples, channels);
	const int64_t blockFrames = kSampleRate * 4 / 10; // 400 ms
	const int64_t stepFrames = kSampleRate / 10;     // 75% overlap
	if (frames < blockFrames) {
		return kSilenceLufs;
	}

	/* K-weighting filter from BS.1770 for 48 kHz */
	std::vector<double> power(size_t(frames), 0.0);
	for (int c = 0; c < channels; c++) {
		Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
			     0.73248077421585};
		Biquad rlb{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
		for (int64_t f = 0; f < frames; f++) {
			double y = rlb.Process(shelf.Process(samples[size_t(f * channels + c)]));
			power[size_t(f)] += y * y;
		}
	}

	std::vector<double> prefix(size_t(frames) + 1, 0.0);
	for (int64_t f = 0; f < frames; f++) {
		prefix[size_t(f + 1)] = prefix[size_t(f)] + power[size_t(f)];
	}

	std::vector<double> blocks;
	for (int64_t start = 0; start + blockFrames <= frames; start += stepFrames) {
		blocks.push_back((prefix[size_t(start + blockFrames)] - prefix[size_t(start)]) / blockFrames);
	}

	auto loudness = [](double meanSquare) {
		return meanSquare > 0 ? -0.691 + 10.0 * std::log10(meanSquare) : kSilenceLufs;
	};
	auto gatedMean = [&](double gateLufs, double &mean) {
		double sum = 0.0;
		int count = 0;
		for (double z : blocks) {
			if (loudness(z) > gateLufs) {
				sum += z;
				count++;
			}
		}
		mean = count ? sum / count : 0.0;
		return count > 0;
	};

	/* absolute gate at -70 LUFS, then a relative gate 10 LU below that result */
	double mean = 0.0;
	if (!gatedMean(-70.0, mean)) {
		return kSilenceLufs;
	}
	if (!gatedMean(loudness(mean) - 10.0, mean)) {
		return kSilenceLufs;
	}
	return loudness(mean);
}

bool NormalizeLoudness(std::vector<float> &samples, int channels, double targetLufs, float ceilingDb)
{
	/* the limiter takes a little loudness away, so a second pass makes up for it */
	constexpr int kPasses = 5;
	constexpr double kToleranceLu = 0.3;

	for (int pass = 0; pass < kPasses; pass++) {
		double current = MeasureLoudness(samples, channels);
		if (current <= kSilenceLufs) {
			return false;
		}
		double changeDb = targetLufs - current;
		if (pass > 0 && std::fabs(changeDb) < kToleranceLu) {
			break;
		}
		float gain = float(DbToGain(changeDb));
		for (float &s : samples) {
			s *= gain;
		}
		Limit(samples, channels, ceilingDb);
	}
	return true;
}

bool CanRemoveNoise()
{
#ifdef HAVE_RNNOISE
	return true;
#else
	return false;
#endif
}

bool RemoveNoise(std::vector<float> &samples, int channels)
{
#ifdef HAVE_RNNOISE
	constexpr int kFrame = 480;
	/* RNNoise answers one frame late, so we feed one extra frame and drop the first */
	constexpr int kDelay = kFrame;
	constexpr float kScale = 32768.0f;
	const int64_t frames = Frames(samples, channels);

	for (int c = 0; c < channels; c++) {
		DenoiseState *state = rnnoise_create(nullptr);
		if (!state) {
			return false;
		}

		std::vector<float> output(size_t(frames + kDelay + kFrame), 0.0f);
		float in[kFrame];
		float out[kFrame];
		for (int64_t start = 0; start < frames + kDelay; start += kFrame) {
			for (int i = 0; i < kFrame; i++) {
				int64_t f = start + i;
				in[i] = f < frames ? samples[size_t(f * channels + c)] * kScale : 0.0f;
			}
			rnnoise_process_frame(state, out, in);
			std::copy(out, out + kFrame, output.begin() + start);
		}
		rnnoise_destroy(state);

		for (int64_t f = 0; f < frames; f++) {
			samples[size_t(f * channels + c)] = output[size_t(f + kDelay)] / kScale;
		}
	}
	return true;
#else
	(void)samples;
	(void)channels;
	return false;
#endif
}

void ChangeVolume(std::vector<float> &samples, int channels, int64_t from, int64_t to, float gainDb)
{
	constexpr int64_t kRampFrames = kSampleRate / 100; // 10 ms
	const int64_t frames = Frames(samples, channels);
	from = std::clamp<int64_t>(from, 0, frames);
	to = std::clamp<int64_t>(to, 0, frames);
	if (to <= from) {
		return;
	}

	const double target = DbToGain(gainDb);
	const int64_t ramp = std::min(kRampFrames, (to - from) / 2);
	for (int64_t f = from; f < to; f++) {
		/* only ramp at an edge that is not the start or end of the whole audio */
		double edge = 1.0;
		if (ramp > 0 && from > 0 && f - from < ramp) {
			edge = double(f - from) / ramp;
		}
		if (ramp > 0 && to < frames && to - f <= ramp) {
			edge = std::min(edge, double(to - f) / ramp);
		}
		float gain = float(1.0 + (target - 1.0) * edge);
		for (int c = 0; c < channels; c++) {
			samples[size_t(f * channels + c)] *= gain;
		}
	}
}

void Fade(std::vector<float> &samples, int channels, int64_t from, int64_t to, bool fadeIn)
{
	const int64_t frames = Frames(samples, channels);
	from = std::clamp<int64_t>(from, 0, frames);
	to = std::clamp<int64_t>(to, 0, frames);
	if (to <= from) {
		return;
	}

	for (int64_t f = from; f < to; f++) {
		double t = double(f - from) / double(to - from);
		/* half a cosine sounds smoother than a straight line */
		double curve = 0.5 - 0.5 * std::cos(kPi * (fadeIn ? t : 1.0 - t));
		for (int c = 0; c < channels; c++) {
			samples[size_t(f * channels + c)] *= float(curve);
		}
	}

	/* everything before a fade-in or after a fade-out is silent */
	if (fadeIn) {
		std::fill(samples.begin(), samples.begin() + from * channels, 0.0f);
	} else {
		std::fill(samples.begin() + to * channels, samples.end(), 0.0f);
	}
}

namespace {
/* 10 ms RMS levels in dB, and a silence threshold learned from this recording
 * (a share of the way from its noise floor to its speech level) */
constexpr int64_t kLevelWindow = kSampleRate / 100;

std::vector<double> WindowLevels(const std::vector<float> &samples, int channels)
{
	const int64_t windows = Frames(samples, channels) / kLevelWindow;
	std::vector<double> levels(static_cast<size_t>(windows));
	for (int64_t w = 0; w < windows; w++) {
		double sum = 0.0;
		for (int64_t i = w * kLevelWindow * channels; i < (w + 1) * kLevelWindow * channels; i++) {
			sum += double(samples[size_t(i)]) * samples[size_t(i)];
		}
		levels[size_t(w)] = 10.0 * std::log10(sum / double(kLevelWindow * channels) + 1e-20);
	}
	return levels;
}

double SilenceThreshold(const std::vector<double> &levels)
{
	std::vector<double> sorted = levels;
	std::sort(sorted.begin(), sorted.end());
	double noise = sorted[sorted.size() / 10];
	double speech = sorted[sorted.size() * 95 / 100];
	return noise + std::max(6.0, (speech - noise) * 0.35);
}
} // namespace

void QuietPauses(std::vector<float> &samples, int channels, float maxReductionDb)
{
	constexpr double kRatio = 3.0;
	constexpr double kAttackMs = 2.0;
	constexpr double kReleaseMs = 150.0;

	std::vector<double> levels = WindowLevels(samples, channels);
	if (levels.size() < 10) {
		return;
	}
	const double threshold = SilenceThreshold(levels);
	const int64_t frames = Frames(samples, channels);
	/* fast to open when a word starts, slow to close so word endings survive */
	const double attack = std::exp(-1.0 / (kAttackMs * 0.001 * kSampleRate));
	const double release = std::exp(-1.0 / (kReleaseMs * 0.001 * kSampleRate));

	double envelopeDb = threshold;
	double reduction = 0.0;
	for (int64_t f = 0; f < frames; f++) {
		double levelDb = GainToDb(FramePeak(samples, channels, f));
		/* peak envelope that holds through the short dips inside words */
		envelopeDb = levelDb > envelopeDb ? levelDb : envelopeDb - 60.0 / (0.3 * kSampleRate);
		double target = envelopeDb < threshold
					? std::min<double>(maxReductionDb, (threshold - envelopeDb) * (kRatio - 1.0))
					: 0.0;
		double coef = target < reduction ? attack : release;
		reduction = coef * reduction + (1.0 - coef) * target;

		float gain = float(DbToGain(-reduction));
		for (int c = 0; c < channels; c++) {
			samples[size_t(f * channels + c)] *= gain;
		}
	}
}

void PolishVoice(std::vector<float> &samples, int channels, double targetLufs, const std::vector<Range> &noiseRanges)
{
	constexpr float kRumbleHz = 80.0f;
	/* bring every microphone to the same working level before compressing */
	constexpr double kWorkingLufs = -20.0;
	constexpr float kCeilingDb = -1.0f;

	constexpr float kStudioReductionDb = 15.0f;
	constexpr float kPauseReductionDb = 12.0f;

	/* measured on real narration: the spectral cleaner keeps the voice more
	 * natural than RNNoise for steady room noise */
	CleanNoiseSpectral(samples, channels, kStudioReductionDb, noiseRanges);
	HighPass(samples, channels, kRumbleHz);
	NormalizeLoudness(samples, channels, kWorkingLufs, kCeilingDb);
	Compress(samples, channels, -26.0f, 3.0f, 5.0f, 120.0f);
	/* compression lifts the noise in the gaps, put it back down */
	QuietPauses(samples, channels, kPauseReductionDb);
	NormalizeLoudness(samples, channels, targetLufs, kCeilingDb);
}

std::vector<Range> FindLongPauses(const std::vector<float> &samples, int channels, double minPauseSec,
				  double keepSec)
{
	constexpr int64_t kWindow = kSampleRate / 100; // 10 ms
	const int64_t frames = Frames(samples, channels);
	const int64_t windows = frames / kWindow;
	std::vector<Range> cuts;
	if (windows < 10) {
		return cuts;
	}

	std::vector<double> levels(static_cast<size_t>(windows));
	for (int64_t w = 0; w < windows; w++) {
		double sum = 0.0;
		for (int64_t i = w * kWindow * channels; i < (w + 1) * kWindow * channels; i++) {
			sum += double(samples[size_t(i)]) * samples[size_t(i)];
		}
		levels[size_t(w)] = 10.0 * std::log10(sum / double(kWindow * channels) + 1e-20);
	}

	/* decide what counts as silence from this recording itself, so it works for
	 * quiet and loud microphones alike */
	std::vector<double> sorted = levels;
	std::sort(sorted.begin(), sorted.end());
	double noise = sorted[sorted.size() / 10];
	double speech = sorted[sorted.size() * 95 / 100];
	double threshold = noise + std::max(6.0, (speech - noise) * 0.35);

	const int64_t minWindows = int64_t(minPauseSec * 100);
	const int64_t keepFrames = int64_t(keepSec * kSampleRate);

	int64_t runStart = -1;
	for (int64_t w = 0; w <= windows; w++) {
		bool quiet = w < windows && levels[size_t(w)] < threshold;
		if (quiet && runStart < 0) {
			runStart = w;
		} else if (!quiet && runStart >= 0) {
			/* leave the very start and end alone, those are not pauses between words */
			bool inside = runStart > 0 && w < windows;
			if (inside && w - runStart >= minWindows) {
				int64_t start = runStart * kWindow + keepFrames / 2;
				int64_t end = w * kWindow - keepFrames / 2;
				if (end > start) {
					cuts.push_back({start, end});
				}
			}
			runStart = -1;
		}
	}
	return cuts;
}

std::vector<Range> FindPauses(const std::vector<float> &samples, int channels, double minPauseSec)
{
	std::vector<Range> pauses;
	std::vector<double> levels = WindowLevels(samples, channels);
	if (levels.size() < 10) {
		return pauses;
	}
	const double threshold = SilenceThreshold(levels);
	const int64_t minWindows = std::max<int64_t>(1, int64_t(minPauseSec * 100));
	const int64_t windows = int64_t(levels.size());

	int64_t runStart = -1;
	for (int64_t w = 0; w <= windows; w++) {
		bool quiet = w < windows && levels[size_t(w)] < threshold;
		if (quiet && runStart < 0) {
			runStart = w;
		} else if (!quiet && runStart >= 0) {
			if (w - runStart >= minWindows) {
				pauses.push_back({runStart * kLevelWindow, w * kLevelWindow});
			}
			runStart = -1;
		}
	}
	return pauses;
}

std::vector<Range> MistakeCuts(const std::vector<float> &samples, int channels, const std::vector<int64_t> &marks)
{
	/* a sentence boundary is a pause of at least this long */
	constexpr double kSentencePauseSec = 0.3;
	/* M is pressed a moment after the slip, so look a little before the mark */
	constexpr int64_t kReactionFrames = kSampleRate / 2;
	/* leave this much quiet before the re-read so it doesn't start abruptly */
	constexpr int64_t kLeadInFrames = kSampleRate / 10;

	std::vector<Range> pauses = FindPauses(samples, channels, kSentencePauseSec);
	const int64_t total = Frames(samples, channels);
	std::vector<Range> cuts;

	for (int64_t mark : marks) {
		/* the pause where the narrator stopped (contains the mark or comes right after) */
		const Range *after = nullptr;
		for (const Range &pause : pauses) {
			if (pause.end > mark) {
				after = &pause;
				break;
			}
		}
		/* the pause before the sentence that went wrong */
		const Range *before = nullptr;
		for (const Range &pause : pauses) {
			if (pause.end <= mark - kReactionFrames && (!after || pause.end <= after->start)) {
				before = &pause;
			}
		}

		int64_t start = before ? (before->start + before->end) / 2 : 0;
		int64_t end = after ? std::max(after->start, after->end - kLeadInFrames) : total;
		if (end > start) {
			cuts.push_back({start, end});
		}
	}

	/* two marks on the same sentence give overlapping cuts: merge them */
	std::sort(cuts.begin(), cuts.end(), [](const Range &a, const Range &b) { return a.start < b.start; });
	std::vector<Range> merged;
	for (const Range &cut : cuts) {
		if (!merged.empty() && cut.start <= merged.back().end) {
			merged.back().end = std::max(merged.back().end, cut.end);
		} else {
			merged.push_back(cut);
		}
	}
	return merged;
}

namespace {
double RmsDb(const std::vector<float> &samples)
{
	double sum = 0.0;
	for (float x : samples) {
		sum += double(x) * x;
	}
	return 10.0 * std::log10(sum / double(std::max<size_t>(1, samples.size())) + 1e-20);
}

double PeakDb(const std::vector<float> &samples)
{
	float peak = 0.0f;
	for (float x : samples) {
		peak = std::max(peak, std::fabs(x));
	}
	return GainToDb(peak);
}

/* the quietest half second: ACX's idea of the noise floor, and our room tone */
Range QuietestHalfSecond(const std::vector<float> &samples, int channels)
{
	constexpr int64_t kWindow = kSampleRate / 2;
	const int64_t frames = Frames(samples, channels);
	Range best{0, std::min(kWindow, frames)};
	double bestEnergy = 1e30;
	for (int64_t start = 0; start + kWindow <= frames; start += kWindow / 4) {
		double energy = 0.0;
		for (int64_t i = start * channels; i < (start + kWindow) * channels; i++) {
			energy += double(samples[size_t(i)]) * samples[size_t(i)];
		}
		if (energy < bestEnergy) {
			bestEnergy = energy;
			best = {start, start + kWindow};
		}
	}
	return best;
}

/* how long the audio stays quiet at the start (or end) */
double QuietEdgeSeconds(const std::vector<float> &samples, int channels, bool atStart)
{
	std::vector<Range> pauses = FindPauses(samples, channels, 0.05);
	const int64_t frames = Frames(samples, channels);
	for (const Range &pause : pauses) {
		if (atStart && pause.start == 0) {
			return double(pause.end) / kSampleRate;
		}
		if (!atStart && pause.end >= frames - kLevelWindow) {
			return double(frames - pause.start) / kSampleRate;
		}
	}
	return 0.0;
}

/* room tone of the given length, looped with short crossfades */
std::vector<float> MakeRoomTone(const std::vector<float> &tone, int channels, int64_t frames)
{
	constexpr int64_t kFade = kSampleRate / 50;
	const int64_t toneFrames = Frames(tone, channels);
	std::vector<float> out(size_t(frames * channels), 0.0f);
	if (toneFrames <= kFade * 2) {
		return out;
	}
	const int64_t step = toneFrames - kFade;
	for (int64_t start = 0; start < frames; start += step) {
		for (int64_t f = 0; f < toneFrames && start + f < frames; f++) {
			double gain = 1.0;
			if (f < kFade && start > 0) {
				gain = double(f) / kFade;
			} else if (f >= toneFrames - kFade) {
				gain = double(toneFrames - f) / kFade;
			}
			for (int c = 0; c < channels; c++) {
				out[size_t((start + f) * channels + c)] += float(gain * tone[size_t(f * channels + c)]);
			}
		}
	}
	return out;
}
} // namespace

void MasterForAudiobook(std::vector<float> &samples, int channels, const std::vector<Range> &roomTone)
{
	constexpr double kTargetRmsDb = -20.0;
	constexpr float kPeakCeilingDb = -3.5f;
	constexpr double kHeadSec = 0.75;
	constexpr double kTailSec = 2.0;
	constexpr int kPasses = 4;

	for (int pass = 0; pass < kPasses; pass++) {
		double change = kTargetRmsDb - RmsDb(samples);
		if (pass > 0 && std::fabs(change) < 0.2) {
			break;
		}
		float gain = float(DbToGain(change));
		for (float &x : samples) {
			x *= gain;
		}
		Limit(samples, channels, kPeakCeilingDb);
	}

	/* room tone to pad with: the recorded one if we have it, else the quietest bit */
	Range source = roomTone.empty() ? QuietestHalfSecond(samples, channels) : roomTone.front();
	std::vector<float> tone(samples.begin() + source.start * channels, samples.begin() + source.end * channels);

	auto fixEdge = [&](bool atStart, double wantSec) {
		const int64_t have = int64_t(QuietEdgeSeconds(samples, channels, atStart) * kSampleRate);
		const int64_t want = int64_t(wantSec * kSampleRate);
		if (have > want) {
			/* too much silence: trim it */
			int64_t cut = (have - want) * channels;
			if (atStart) {
				samples.erase(samples.begin(), samples.begin() + cut);
			} else {
				samples.erase(samples.end() - cut, samples.end());
			}
		} else if (have < want) {
			std::vector<float> pad = MakeRoomTone(tone, channels, want - have);
			samples.insert(atStart ? samples.begin() : samples.end(), pad.begin(), pad.end());
		}
	};
	fixEdge(true, kHeadSec);
	fixEdge(false, kTailSec);
}

AcxReport CheckAudiobook(const std::vector<float> &samples, int channels)
{
	AcxReport report;
	report.rmsDb = RmsDb(samples);
	report.peakDb = PeakDb(samples);
	Range quiet = QuietestHalfSecond(samples, channels);
	std::vector<float> floor(samples.begin() + quiet.start * channels, samples.begin() + quiet.end * channels);
	report.noiseFloorDb = RmsDb(floor);
	report.headSec = QuietEdgeSeconds(samples, channels, true);
	report.tailSec = QuietEdgeSeconds(samples, channels, false);
	return report;
}

int ReduceBreaths(std::vector<float> &samples, int channels, float reductionDb)
{
	/* a breath is a short, soft, airy (hiss-like) sound right next to silence;
	 * a fading word ending is soft too, but its energy sits low, below 1 kHz */
	constexpr double kMinBreathSec = 0.08;
	constexpr double kMaxBreathSec = 0.8;
	constexpr double kBelowSpeechDb = 8.0;
	constexpr double kAiryDb = 3.0; // high band louder than low band by this much
	constexpr int64_t kNearSilenceWindows = 15; // 150 ms
	constexpr int64_t kRampFrames = kSampleRate / 50;

	std::vector<double> levels = WindowLevels(samples, channels);
	if (levels.size() < 20) {
		return 0;
	}
	std::vector<double> sorted = levels;
	std::sort(sorted.begin(), sorted.end());
	const double speech = sorted[sorted.size() * 95 / 100];
	const double silence = SilenceThreshold(levels);
	const double breathCeiling = speech - kBelowSpeechDb;
	if (breathCeiling <= silence) {
		return 0;
	}

	/* per 10 ms window: energy above 2 kHz minus energy below 1 kHz, in dB */
	const int64_t windows = int64_t(levels.size());
	std::vector<double> airiness(static_cast<size_t>(windows), -100.0);
	{
		double w0 = 2.0 * kPi * 1000.0 / kSampleRate;
		double alpha = std::sin(w0) / (2.0 * std::sqrt(0.5));
		double cosw = std::cos(w0);
		double a0 = 1.0 + alpha;
		Biquad low{(1.0 - cosw) / 2.0 / a0, (1.0 - cosw) / a0, (1.0 - cosw) / 2.0 / a0, -2.0 * cosw / a0,
			     (1.0 - alpha) / a0};
		Biquad high = MakeHighPass(2000.0);
		for (int64_t w = 0; w < windows; w++) {
			double lowSum = 1e-20, highSum = 1e-20;
			for (int64_t f = w * kLevelWindow; f < (w + 1) * kLevelWindow; f++) {
				double mono = 0.0;
				for (int c = 0; c < channels; c++) {
					mono += samples[size_t(f * channels + c)];
				}
				mono /= channels;
				double l = low.Process(mono);
				double h = high.Process(mono);
				lowSum += l * l;
				highSum += h * h;
			}
			airiness[size_t(w)] = 10.0 * std::log10(highSum / lowSum);
		}
	}

	auto isSilent = [&](int64_t w) { return w < 0 || w >= windows || levels[size_t(w)] < silence; };
	auto nearSilence = [&](int64_t from, int64_t to) {
		for (int64_t w = from; w < to; w++) {
			if (isSilent(w)) {
				return true;
			}
		}
		return false;
	};
	auto isBreathy = [&](int64_t w) {
		return w < windows && levels[size_t(w)] >= silence && levels[size_t(w)] < breathCeiling &&
		       airiness[size_t(w)] > kAiryDb;
	};

	int found = 0;
	const double gain = DbToGain(-std::fabs(reductionDb));
	int64_t runStart = -1;
	for (int64_t w = 0; w <= windows; w++) {
		bool breathy = isBreathy(w);
		if (breathy && runStart < 0) {
			runStart = w;
		} else if (!breathy && runStart >= 0) {
			double seconds = double(w - runStart) / 100.0;
			bool breathLength = seconds >= kMinBreathSec && seconds <= kMaxBreathSec;
			bool besideSilence = nearSilence(runStart - kNearSilenceWindows, runStart) ||
					     nearSilence(w, w + kNearSilenceWindows);
			if (breathLength && besideSilence) {
				const int64_t first = runStart * kLevelWindow;
				const int64_t last = w * kLevelWindow;
				for (int64_t f = first; f < last; f++) {
					/* ramp down and back up so the dip itself is not heard */
					double edge = std::min(1.0, std::min(double(f - first), double(last - f)) / kRampFrames);
					float g = float(1.0 - (1.0 - gain) * edge);
					for (int c = 0; c < channels; c++) {
						samples[size_t(f * channels + c)] *= g;
					}
				}
				found++;
			}
			runStart = -1;
		}
	}
	return found;
}

void DeEss(std::vector<float> &samples, int channels, float maxReductionDb)
{
	/* "s" lives above ~5 kHz; when that band gets close to the whole signal, turn it down */
	constexpr double kSplitHz = 5000.0;
	constexpr double kTriggerDb = -6.0;
	constexpr double kRangeDb = 10.0;
	constexpr double kAttackMs = 1.0;
	constexpr double kReleaseMs = 60.0;

	const int64_t frames = Frames(samples, channels);
	const double attack = std::exp(-1.0 / (kAttackMs * 0.001 * kSampleRate));
	const double release = std::exp(-1.0 / (kReleaseMs * 0.001 * kSampleRate));

	/* RBJ low-pass at the split point; high = input - low */
	double w0 = 2.0 * kPi * kSplitHz / kSampleRate;
	double alpha = std::sin(w0) / (2.0 * std::sqrt(0.5));
	double cosw = std::cos(w0);
	double a0 = 1.0 + alpha;
	Biquad proto{(1.0 - cosw) / 2.0 / a0, (1.0 - cosw) / a0, (1.0 - cosw) / 2.0 / a0, -2.0 * cosw / a0,
		     (1.0 - alpha) / a0};

	std::vector<Biquad> lowpass(size_t(channels), proto);
	double highEnv = 0.0, fullEnv = 0.0, reduction = 0.0;
	std::vector<double> low(static_cast<size_t>(channels));
	std::vector<double> high(static_cast<size_t>(channels));

	for (int64_t f = 0; f < frames; f++) {
		double highPeak = 0.0, fullPeak = 0.0;
		for (int c = 0; c < channels; c++) {
			double x = samples[size_t(f * channels + c)];
			low[size_t(c)] = lowpass[size_t(c)].Process(x);
			high[size_t(c)] = x - low[size_t(c)];
			highPeak = std::max(highPeak, std::fabs(high[size_t(c)]));
			fullPeak = std::max(fullPeak, std::fabs(x));
		}
		highEnv = highPeak > highEnv ? attack * highEnv + (1 - attack) * highPeak
					     : release * highEnv + (1 - release) * highPeak;
		fullEnv = fullPeak > fullEnv ? attack * fullEnv + (1 - attack) * fullPeak
					     : release * fullEnv + (1 - release) * fullPeak;

		double ratioDb = GainToDb(highEnv) - GainToDb(fullEnv + 1e-9);
		double target = std::clamp((ratioDb - kTriggerDb) / kRangeDb, 0.0, 1.0) * maxReductionDb;
		double coef = target > reduction ? attack : release;
		reduction = coef * reduction + (1 - coef) * target;

		double g = DbToGain(-reduction);
		for (int c = 0; c < channels; c++) {
			samples[size_t(f * channels + c)] = float(low[size_t(c)] + g * high[size_t(c)]);
		}
	}
}

int RemoveClicks(std::vector<float> &samples, int channels)
{
	/* a click is a spike a few milliseconds long that sticks far out of its surroundings */
	constexpr int64_t kContext = kSampleRate / 50;     // 20 ms around it
	constexpr int64_t kMaxClick = kSampleRate * 3 / 1000; // longer than 3 ms is a sound, not a click
	constexpr int64_t kRepairPad = kSampleRate / 2000; // 0.5 ms each side
	constexpr double kSpikeRatio = 12.0;
	/* jumps smaller than this (about -46 dBFS) are never audible clicks */
	constexpr double kMinJump = 0.005;
	constexpr int64_t kGuard = kSampleRate / 500; // 2 ms kept out of the "surroundings"

	const int64_t frames = Frames(samples, channels);
	if (frames < kContext * 4) {
		return 0;
	}

	/* first difference of the mono mix highlights sudden jumps */
	std::vector<double> diff(static_cast<size_t>(frames), 0.0);
	for (int64_t f = 1; f < frames; f++) {
		double now = 0.0, before = 0.0;
		for (int c = 0; c < channels; c++) {
			now += samples[size_t(f * channels + c)];
			before += samples[size_t((f - 1) * channels + c)];
		}
		diff[size_t(f)] = std::fabs(now - before) / channels;
	}
	std::vector<double> prefix(size_t(frames) + 1, 0.0);
	for (int64_t f = 0; f < frames; f++) {
		prefix[size_t(f + 1)] = prefix[size_t(f)] + diff[size_t(f)] * diff[size_t(f)];
	}
	auto localRms = [&](int64_t a, int64_t b) {
		a = std::max<int64_t>(0, a);
		b = std::min(frames, b);
		return std::sqrt((prefix[size_t(b)] - prefix[size_t(a)]) / double(std::max<int64_t>(1, b - a)));
	};

	int found = 0;
	for (int64_t f = kContext; f < frames - kContext;) {
		/* surroundings without the spike itself, so a click can't hide itself */
		double around = std::sqrt((std::pow(localRms(f - kContext, f - kGuard), 2) +
					   std::pow(localRms(f + kGuard, f + kContext), 2)) /
					  2.0);
		if (around <= 1e-6 || diff[size_t(f)] < kMinJump || diff[size_t(f)] < kSpikeRatio * around) {
			f++;
			continue;
		}
		/* how long does the spike last? */
		int64_t end = f;
		while (end < frames - kContext && diff[size_t(end)] >= kSpikeRatio * 0.3 * around) {
			end++;
		}
		if (end - f > kMaxClick) {
			f = end;
			continue;
		}

		/* bridge the click with a smooth curve between the clean samples around it */
		int64_t a = f - kRepairPad;
		int64_t b = end + kRepairPad;
		for (int c = 0; c < channels; c++) {
			double ya = samples[size_t(a * channels + c)];
			double yb = samples[size_t(b * channels + c)];
			for (int64_t i = a + 1; i < b; i++) {
				double t = double(i - a) / double(b - a);
				double smooth = t * t * (3.0 - 2.0 * t);
				samples[size_t(i * channels + c)] = float(ya + (yb - ya) * smooth);
			}
		}
		found++;
		f = b + 1;
	}
	return found;
}

} // namespace VoiceEffects

