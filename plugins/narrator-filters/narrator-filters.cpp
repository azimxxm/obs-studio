/* Live voice filters for narrators: the real-time versions of the audio
 * editor's Studio noise removal, De-esser and rumble cut. */

#include <obs-module.h>
#include <media-io/audio-math.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <memory>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("narrator-filters", "en-US")

namespace {

constexpr double kPi = 3.14159265358979323846;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

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

/* RBJ cookbook Butterworth sections */
Biquad MakeFilter(double cutoffHz, double sampleRate, bool highPass)
{
	double w0 = 2.0 * kPi * cutoffHz / sampleRate;
	double alpha = std::sin(w0) / (2.0 * std::sqrt(0.5));
	double cosw = std::cos(w0);
	double a0 = 1.0 + alpha;
	Biquad f;
	if (highPass) {
		f.b0 = (1.0 + cosw) / 2.0 / a0;
		f.b1 = -(1.0 + cosw) / a0;
	} else {
		f.b0 = (1.0 - cosw) / 2.0 / a0;
		f.b1 = (1.0 - cosw) / a0;
	}
	f.b2 = f.b0;
	f.a1 = -2.0 * cosw / a0;
	f.a2 = (1.0 - alpha) / a0;
	return f;
}

double GainToDb(double gain)
{
	return gain > 0.0 ? 20.0 * std::log10(gain) : -200.0;
}

size_t Channels()
{
	return audio_output_get_channels(obs_get_audio());
}

double SampleRate()
{
	return double(audio_output_get_sample_rate(obs_get_audio()));
}

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

double ExpIntegral(double x)
{
	if (x < 1.0) {
		double sum = 0.0, term = 1.0;
		for (int k = 1; k < 30; k++) {
			term *= -x / k;
			sum -= term / k;
		}
		return -0.5772156649015329 - std::log(x) + sum;
	}
	double num = x * x + 2.334733 * x + 0.250621;
	double den = x * x + 3.330657 * x + 1.681534;
	return std::exp(-x) / x * num / den;
}

/* ------------------------------------------------------------------ */
/* Low rumble cut                                                      */

struct RumbleFilter {
	obs_source_t *context = nullptr;
	std::vector<Biquad> first, second;
};

void RumbleUpdate(void *data, obs_data_t *settings)
{
	auto *f = static_cast<RumbleFilter *>(data);
	double cutoff = obs_data_get_double(settings, "cutoff");
	Biquad proto = MakeFilter(cutoff, SampleRate(), true);
	f->first.assign(Channels(), proto);
	f->second.assign(Channels(), proto);
}

obs_audio_data *RumbleAudio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<RumbleFilter *>(data);
	for (size_t c = 0; c < f->first.size(); c++) {
		auto *samples = reinterpret_cast<float *>(audio->data[c]);
		if (!samples) {
			continue;
		}
		for (uint32_t i = 0; i < audio->frames; i++) {
			samples[i] = float(f->second[c].Process(f->first[c].Process(samples[i])));
		}
	}
	return audio;
}

/* ------------------------------------------------------------------ */
/* De-esser: turns the top end down while it is close to the whole signal */

struct DeEssFilter {
	obs_source_t *context = nullptr;
	std::vector<Biquad> lowpass;
	double maxReductionDb = 6.0;
	double attack = 0.0, release = 0.0;
	double highEnv = 0.0, fullEnv = 0.0, reduction = 0.0;
};

void DeEssUpdate(void *data, obs_data_t *settings)
{
	constexpr double kSplitHz = 5000.0;
	constexpr double kAttackMs = 1.0;
	constexpr double kReleaseMs = 60.0;
	auto *f = static_cast<DeEssFilter *>(data);
	const double rate = SampleRate();
	f->maxReductionDb = obs_data_get_double(settings, "strength");
	f->lowpass.assign(Channels(), MakeFilter(kSplitHz, rate, false));
	f->attack = std::exp(-1.0 / (kAttackMs * 0.001 * rate));
	f->release = std::exp(-1.0 / (kReleaseMs * 0.001 * rate));
}

obs_audio_data *DeEssAudio(void *data, obs_audio_data *audio)
{
	constexpr double kTriggerDb = -6.0;
	constexpr double kRangeDb = 10.0;
	auto *f = static_cast<DeEssFilter *>(data);
	const size_t channels = f->lowpass.size();
	std::vector<double> low(channels), high(channels);

	for (uint32_t i = 0; i < audio->frames; i++) {
		double highPeak = 0.0, fullPeak = 0.0;
		for (size_t c = 0; c < channels; c++) {
			auto *samples = reinterpret_cast<float *>(audio->data[c]);
			double x = samples ? samples[i] : 0.0;
			low[c] = f->lowpass[c].Process(x);
			high[c] = x - low[c];
			highPeak = std::max(highPeak, std::fabs(high[c]));
			fullPeak = std::max(fullPeak, std::fabs(x));
		}
		auto follow = [&](double env, double peak) {
			double coef = peak > env ? f->attack : f->release;
			return coef * env + (1.0 - coef) * peak;
		};
		f->highEnv = follow(f->highEnv, highPeak);
		f->fullEnv = follow(f->fullEnv, fullPeak);

		double ratioDb = GainToDb(f->highEnv) - GainToDb(f->fullEnv + 1e-9);
		double target = std::clamp((ratioDb - kTriggerDb) / kRangeDb, 0.0, 1.0) * f->maxReductionDb;
		double coef = target > f->reduction ? f->attack : f->release;
		f->reduction = coef * f->reduction + (1.0 - coef) * target;
		double g = db_to_mul(float(-f->reduction));

		for (size_t c = 0; c < channels; c++) {
			auto *samples = reinterpret_cast<float *>(audio->data[c]);
			if (samples) {
				samples[i] = float(low[c] + g * high[c]);
			}
		}
	}
	return audio;
}

/* ------------------------------------------------------------------ */
/* Studio noise removal, live: STFT + log-spectral gain, with the noise
 * learned on the fly from the quietest moments (minimum statistics). */

class LiveSpectralCleaner {
public:
	static constexpr size_t kFft = 1024;
	static constexpr size_t kHop = kFft / 4;
	static constexpr size_t kBins = kFft / 2 + 1;

	explicit LiveSpectralCleaner(double sampleRate)
	{
		window.resize(kFft);
		for (size_t i = 0; i < kFft; i++) {
			window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(kFft));
		}
		input.assign(kFft, 0.0);
		overlap.assign(kFft, 0.0);
		/* one hop of silence is enough to never run dry; total delay is one frame (21 ms) */
		output.assign(kHop, 0.0f);
		smoothPower.assign(kBins, 0.0);
		minNow.assign(kBins, 1e30);
		minBefore.assign(kBins, 1e30);
		noise.assign(kBins, 0.0);
		previousClean.assign(kBins, 0.0);
		spectrum.resize(kFft);
		/* minimum statistics window: about 1.5 s of frames */
		minWindowFrames = std::max<size_t>(8, size_t(1.5 * sampleRate / double(kHop)));
	}

	void SetFloor(double reductionDb) { floorGain = std::pow(10.0, -std::fabs(reductionDb) / 20.0); }

	void Process(float *samples, uint32_t frames)
	{
		for (uint32_t i = 0; i < frames; i++) {
			/* ring buffer: no shifting on every sample */
			input[writePos] = samples[i];
			writePos = (writePos + 1) % kFft;
			if (++sinceHop == kHop) {
				sinceHop = 0;
				ProcessFrame();
			}
			samples[i] = output.front();
			output.pop_front();
		}
	}

private:
	std::vector<double> window, input, overlap;
	std::deque<float> output;
	std::vector<double> smoothPower, minNow, minBefore, noise, previousClean;
	std::vector<std::complex<double>> spectrum;
	size_t sinceHop = 0;
	size_t writePos = 0;
	size_t framesInWindow = 0;
	size_t minWindowFrames = 0;
	size_t framesSeen = 0;
	double floorGain = 0.25;

	void ProcessFrame()
	{
		constexpr double kSmoothing = 0.98;     // decision-directed a-priori SNR
		constexpr double kPowerSmoothing = 0.8; // for the noise tracker
		constexpr double kMinBias = 1.8;        // the minimum sits below the mean noise
		constexpr double kOverlapGain = 1.5;    // Hann in and out at 75% overlap

		/* oldest sample sits at writePos */
		for (size_t i = 0; i < kFft; i++) {
			spectrum[i] = input[(writePos + i) % kFft] * window[i];
		}
		Fft(spectrum, false);

		for (size_t k = 0; k < kBins; k++) {
			double power = std::norm(spectrum[k]);
			smoothPower[k] = kPowerSmoothing * smoothPower[k] + (1.0 - kPowerSmoothing) * power;
			minNow[k] = std::min(minNow[k], smoothPower[k]);
			noise[k] = std::max(1e-20, std::min(minNow[k], minBefore[k]) * kMinBias);
		}
		if (++framesInWindow >= minWindowFrames) {
			/* start a fresh window so the noise estimate can rise again */
			framesInWindow = 0;
			minBefore = minNow;
			std::fill(minNow.begin(), minNow.end(), 1e30);
		}
		framesSeen++;

		std::vector<double> gain(kBins);
		for (size_t k = 0; k < kBins; k++) {
			double power = std::norm(spectrum[k]);
			double posteriori = power / noise[k];
			double priori = kSmoothing * previousClean[k] / noise[k] +
					(1.0 - kSmoothing) * std::max(posteriori - 1.0, 0.0);
			double wiener = priori / (1.0 + priori);
			double v = std::max(wiener * posteriori, 1e-10);
			gain[k] = std::clamp(wiener * std::exp(0.5 * ExpIntegral(v)), floorGain, 1.0);
			/* leave the very first moments alone while the noise is still unknown */
			if (framesSeen < minWindowFrames / 4) {
				gain[k] = 1.0;
			}
			previousClean[k] = gain[k] * gain[k] * power;
		}

		for (size_t k = 0; k < kBins; k++) {
			double left = gain[k > 0 ? k - 1 : k];
			double right = gain[k + 1 < kBins ? k + 1 : k];
			spectrum[k] *= 0.25 * left + 0.5 * gain[k] + 0.25 * right;
		}
		for (size_t k = kBins; k < kFft; k++) {
			spectrum[k] = std::conj(spectrum[kFft - k]);
		}
		Fft(spectrum, true);

		for (size_t i = 0; i < kFft; i++) {
			overlap[i] += spectrum[i].real() * window[i] / kOverlapGain;
		}
		/* the first hop is finished: hand it out, shift the rest */
		for (size_t i = 0; i < kHop; i++) {
			output.push_back(float(overlap[i]));
		}
		overlap.erase(overlap.begin(), overlap.begin() + kHop);
		overlap.resize(kFft, 0.0);
	}
};

struct StudioNoiseFilter {
	obs_source_t *context = nullptr;
	std::vector<std::unique_ptr<LiveSpectralCleaner>> cleaners;
};

void StudioNoiseUpdate(void *data, obs_data_t *settings)
{
	auto *f = static_cast<StudioNoiseFilter *>(data);
	const size_t channels = Channels();
	if (f->cleaners.size() != channels) {
		f->cleaners.clear();
		for (size_t c = 0; c < channels; c++) {
			f->cleaners.push_back(std::make_unique<LiveSpectralCleaner>(SampleRate()));
		}
	}
	for (auto &cleaner : f->cleaners) {
		cleaner->SetFloor(obs_data_get_double(settings, "reduction"));
	}
}

obs_audio_data *StudioNoiseAudio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<StudioNoiseFilter *>(data);
	for (size_t c = 0; c < f->cleaners.size(); c++) {
		if (audio->data[c]) {
			f->cleaners[c]->Process(reinterpret_cast<float *>(audio->data[c]), audio->frames);
		}
	}
	return audio;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */

template<typename T> void *Create(obs_data_t *settings, obs_source_t *context, void (*update)(void *, obs_data_t *))
{
	auto *filter = new T();
	filter->context = context;
	update(filter, settings);
	return filter;
}

template<typename T> void Destroy(void *data)
{
	delete static_cast<T *>(data);
}

obs_source_info MakeInfo(const char *id, const char *nameKey)
{
	obs_source_info info = {};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.type_data = const_cast<char *>(nameKey);
	info.get_name = [](void *typeData) { return obs_module_text(static_cast<const char *>(typeData)); };
	return info;
}

} // namespace

bool obs_module_load(void)
{
	obs_source_info studio = MakeInfo("narrator_studio_noise_filter", "StudioNoise");
	studio.create = [](obs_data_t *s, obs_source_t *c) {
		return Create<StudioNoiseFilter>(s, c, StudioNoiseUpdate);
	};
	studio.destroy = Destroy<StudioNoiseFilter>;
	studio.update = StudioNoiseUpdate;
	studio.filter_audio = StudioNoiseAudio;
	studio.get_defaults = [](obs_data_t *s) { obs_data_set_default_double(s, "reduction", 12.0); };
	studio.get_properties = [](void *) {
		obs_properties_t *props = obs_properties_create();
		obs_property_t *p = obs_properties_add_float_slider(props, "reduction",
								    obs_module_text("StudioNoise.Reduction"), 3.0,
								    30.0, 1.0);
		obs_property_float_set_suffix(p, " dB");
		return props;
	};
	obs_register_source(&studio);

	obs_source_info deess = MakeInfo("narrator_deess_filter", "DeEss");
	deess.create = [](obs_data_t *s, obs_source_t *c) { return Create<DeEssFilter>(s, c, DeEssUpdate); };
	deess.destroy = Destroy<DeEssFilter>;
	deess.update = DeEssUpdate;
	deess.filter_audio = DeEssAudio;
	deess.get_defaults = [](obs_data_t *s) { obs_data_set_default_double(s, "strength", 6.0); };
	deess.get_properties = [](void *) {
		obs_properties_t *props = obs_properties_create();
		obs_property_t *p = obs_properties_add_float_slider(props, "strength",
								    obs_module_text("DeEss.Strength"), 0.0, 12.0, 0.5);
		obs_property_float_set_suffix(p, " dB");
		return props;
	};
	obs_register_source(&deess);

	obs_source_info rumble = MakeInfo("narrator_rumble_filter", "Rumble");
	rumble.create = [](obs_data_t *s, obs_source_t *c) { return Create<RumbleFilter>(s, c, RumbleUpdate); };
	rumble.destroy = Destroy<RumbleFilter>;
	rumble.update = RumbleUpdate;
	rumble.filter_audio = RumbleAudio;
	rumble.get_defaults = [](obs_data_t *s) { obs_data_set_default_double(s, "cutoff", 80.0); };
	rumble.get_properties = [](void *) {
		obs_properties_t *props = obs_properties_create();
		obs_property_t *p = obs_properties_add_float_slider(props, "cutoff", obs_module_text("Rumble.Cutoff"),
								    40.0, 200.0, 5.0);
		obs_property_float_set_suffix(p, " Hz");
		return props;
	};
	obs_register_source(&rumble);
	return true;
}
