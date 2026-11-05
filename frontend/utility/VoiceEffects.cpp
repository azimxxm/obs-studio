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

void CleanNoiseSpectral(std::vector<float> &samples, int channels, float reductionDb)
{
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
		size_t noiseFrames = std::max<size_t>(4, size_t(double(order.size()) * kNoiseFrameShare));
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

void PolishVoice(std::vector<float> &samples, int channels, double targetLufs)
{
	constexpr float kRumbleHz = 80.0f;
	/* bring every microphone to the same working level before compressing */
	constexpr double kWorkingLufs = -20.0;
	constexpr float kCeilingDb = -1.0f;

	constexpr float kStudioReductionDb = 15.0f;
	constexpr float kPauseReductionDb = 12.0f;

	/* measured on real narration: the spectral cleaner keeps the voice more
	 * natural than RNNoise for steady room noise */
	CleanNoiseSpectral(samples, channels, kStudioReductionDb);
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

} // namespace VoiceEffects
