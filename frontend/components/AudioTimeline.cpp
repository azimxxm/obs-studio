#include "AudioTimeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include "moc_AudioTimeline.cpp"

namespace {
/* short fade at every join so cuts don't click */
constexpr qint64 kFadeFrames = AudioTimeline::kSampleRate * 4 / 1000;
constexpr int kMaxChannels = 2;

struct WavInfo {
	int channels = 0;
	int sampleRate = 0;
	int bitsPerSample = 0;
	qint64 dataBytes = -1;
};

quint32 ReadLE32(const char *p)
{
	return quint32(quint8(p[0])) | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])) << 16 |
	       quint32(quint8(p[3])) << 24;
}

quint16 ReadLE16(const char *p)
{
	return quint16(quint8(p[0])) | quint16(quint8(p[1])) << 8;
}

bool ReadWavInfo(const QString &path, WavInfo &info)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}

	char header[12];
	if (file.read(header, 12) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		return false;
	}

	/* walk the chunks until we have both "fmt " and "data" */
	char chunk[8];
	while (file.read(chunk, 8) == 8) {
		quint32 size = ReadLE32(chunk + 4);
		if (memcmp(chunk, "fmt ", 4) == 0) {
			QByteArray fmt = file.read(std::min<quint32>(size, 64));
			if (fmt.size() < 16) {
				return false;
			}
			info.channels = ReadLE16(fmt.constData() + 2);
			info.sampleRate = int(ReadLE32(fmt.constData() + 4));
			info.bitsPerSample = ReadLE16(fmt.constData() + 14);
			file.seek(file.pos() - fmt.size() + size + (size & 1));
		} else if (memcmp(chunk, "data", 4) == 0) {
			/* a file still being written can have a zero size here */
			qint64 remaining = file.size() - file.pos();
			info.dataBytes = (size == 0 || size > remaining) ? remaining : size;
			break;
		} else {
			file.seek(file.pos() + size + (size & 1));
		}
	}

	return info.channels > 0 && info.dataBytes >= 0;
}
} // namespace

int ReadWavChannels(const QString &path)
{
	WavInfo info;
	return ReadWavInfo(path, info) ? info.channels : 0;
}

double ReadWavDurationSeconds(const QString &path)
{
	WavInfo info;
	if (!ReadWavInfo(path, info) || info.sampleRate <= 0 || info.bitsPerSample <= 0) {
		return -1.0;
	}
	qint64 bytesPerFrame = qint64(info.channels) * (info.bitsPerSample / 8);
	return bytesPerFrame > 0 ? double(info.dataBytes / bytesPerFrame) / info.sampleRate : -1.0;
}

/* ---------------------------------------------------------------------- */

bool AudioTimeline::Load(const QStringList &paths, QString &error)
{
	sources.clear();
	segments.clear();
	undoStack.clear();
	gain = 1.0f;
	mistakes.clear();
	roomTone.clear();

	/* WAV takes tell us their channel count, other formats are treated as stereo */
	channels = 1;
	for (const QString &path : paths) {
		int fileChannels = ReadWavChannels(path);
		channels = std::max(channels, fileChannels > 0 ? fileChannels : kMaxChannels);
	}
	channels = std::min(channels, kMaxChannels);

	for (const QString &path : paths) {
		AudioSourceClip clip;
		clip.path = path;
		if (!Decode(path, clip, error)) {
			return false;
		}
		if (clip.frames == 0) {
			continue;
		}
		BuildPeaks(clip, channels);
		LoadSidecar(path, TotalFrames());
		segments.append({int(sources.size()), 0, clip.frames});
		sources.push_back(std::move(clip));
	}

	if (sources.empty()) {
		error = QStringLiteral("No audio found in the selected files.");
		return false;
	}
	takeCount = int(sources.size());
	return true;
}

bool AudioTimeline::Decode(const QString &path, AudioSourceClip &clip, QString &error) const
{
	QAudioFormat format;
	format.setSampleRate(kSampleRate);
	format.setChannelCount(channels);
	format.setSampleFormat(QAudioFormat::Float);

	QAudioDecoder decoder;
	decoder.setAudioFormat(format);
	decoder.setSource(QUrl::fromLocalFile(path));

	QEventLoop loop;
	QString decodeError;

	QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
		QAudioBuffer buffer = decoder.read();
		const QAudioFormat bufferFormat = buffer.format();
		const int bufferChannels = bufferFormat.channelCount();
		const qint64 frames = buffer.frameCount();
		if (bufferChannels <= 0 || frames <= 0) {
			return;
		}

		size_t offset = clip.samples.size();
		clip.samples.resize(offset + size_t(frames) * channels);
		float *dst = clip.samples.data() + offset;
		const char *src = buffer.constData<char>();
		const int bytesPerSample = bufferFormat.bytesPerSample();

		/* map whatever the decoder gave us onto our channel count */
		for (qint64 f = 0; f < frames; f++) {
			for (int c = 0; c < channels; c++) {
				int srcChannel = std::min(c, bufferChannels - 1);
				const char *sample = src + (f * bufferChannels + srcChannel) * bytesPerSample;
				dst[f * channels + c] = bufferFormat.sampleFormat() == QAudioFormat::Float
								? *reinterpret_cast<const float *>(sample)
								: bufferFormat.normalizedSampleValue(sample);
			}
		}
	});
	QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
	QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop,
			 [&](QAudioDecoder::Error) {
				 decodeError = decoder.errorString();
				 loop.quit();
			 });

	decoder.start();
	loop.exec();
	decoder.stop();

	if (!decodeError.isEmpty()) {
		error = QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), decodeError);
		return false;
	}

	clip.frames = qint64(clip.samples.size()) / channels;
	return true;
}

void AudioTimeline::BuildPeaks(AudioSourceClip &clip, int channelCount)
{
	qint64 blocks = (clip.frames + kPeakBlock - 1) / kPeakBlock;
	clip.peaks.assign(size_t(blocks), 0.0f);

	for (qint64 b = 0; b < blocks; b++) {
		qint64 first = b * kPeakBlock * channelCount;
		qint64 last = std::min<qint64>(first + kPeakBlock * channelCount, qint64(clip.samples.size()));
		float peak = 0.0f;
		for (qint64 i = first; i < last; i++) {
			peak = std::max(peak, std::fabs(clip.samples[size_t(i)]));
		}
		clip.peaks[size_t(b)] = peak;
	}
}

qint64 AudioTimeline::TotalFrames() const
{
	qint64 total = 0;
	for (const AudioSegment &segment : segments) {
		total += segment.Length();
	}
	return total;
}

QVector<qint64> AudioTimeline::Boundaries() const
{
	QVector<qint64> result;
	qint64 position = 0;
	for (int i = 0; i + 1 < segments.size(); i++) {
		position += segments[i].Length();
		result.append(position);
	}
	return result;
}

float AudioTimeline::PeakAt(qint64 from, qint64 to) const
{
	float peak = 0.0f;
	qint64 segmentStart = 0;

	for (const AudioSegment &segment : segments) {
		qint64 segmentEnd = segmentStart + segment.Length();
		if (segmentEnd > from && segmentStart < to) {
			const AudioSourceClip &clip = sources[size_t(segment.source)];
			qint64 a = segment.start + std::max<qint64>(0, from - segmentStart);
			qint64 b = segment.start + std::min(segment.Length(), to - segmentStart);
			for (qint64 block = a / kPeakBlock; block <= (b - 1) / kPeakBlock; block++) {
				peak = std::max(peak, clip.peaks[size_t(block)]);
			}
		}
		if (segmentStart >= to) {
			break;
		}
		segmentStart = segmentEnd;
	}
	bool inPreview = previewGain != 1.0f && from < previewTo && to > previewFrom;
	return peak * gain * (inPreview ? previewGain : 1.0f);
}

void AudioTimeline::SetPreviewGain(qint64 from, qint64 to, float gainDb)
{
	previewFrom = from;
	previewTo = to;
	previewGain = std::pow(10.0f, gainDb / 20.0f);
}

void AudioTimeline::ClearPreview()
{
	previewFrom = previewTo = 0;
	previewGain = 1.0f;
}

float AudioTimeline::FadeGain(qint64 position) const
{
	float gain = 1.0f;
	for (qint64 boundary : Boundaries()) {
		qint64 distance = std::llabs(position - boundary);
		if (distance < kFadeFrames) {
			gain = std::min(gain, float(distance) / float(kFadeFrames));
		}
	}
	return gain;
}

qint64 AudioTimeline::Read(qint64 position, float *out, qint64 frameCount) const
{
	qint64 copied = 0;
	qint64 segmentStart = 0;

	for (const AudioSegment &segment : segments) {
		qint64 segmentEnd = segmentStart + segment.Length();
		qint64 cursor = position + copied;
		if (copied < frameCount && cursor < segmentEnd && cursor >= segmentStart) {
			qint64 offset = cursor - segmentStart;
			qint64 count = std::min(frameCount - copied, segment.Length() - offset);
			const float *src = sources[size_t(segment.source)].samples.data() +
					   (segment.start + offset) * channels;
			memcpy(out + copied * channels, src, size_t(count * channels) * sizeof(float));
			copied += count;
		}
		segmentStart = segmentEnd;
	}

	if (gain != 1.0f) {
		for (qint64 i = 0; i < copied * channels; i++) {
			out[i] *= gain;
		}
	}

	if (previewGain != 1.0f) {
		qint64 first = std::max(position, previewFrom);
		qint64 last = std::min(position + copied, previewTo);
		for (qint64 i = (first - position) * channels; i < (last - position) * channels; i++) {
			out[i] *= previewGain;
		}
	}

	/* soften the joins; only frames near a boundary are touched */
	for (qint64 boundary : Boundaries()) {
		qint64 first = std::max(position, boundary - kFadeFrames);
		qint64 last = std::min(position + copied, boundary + kFadeFrames);
		for (qint64 f = first; f < last; f++) {
			float gain = FadeGain(f);
			for (int c = 0; c < channels; c++) {
				out[(f - position) * channels + c] *= gain;
			}
		}
	}
	return copied;
}

void AudioTimeline::Cut(qint64 from, qint64 to)
{
	from = std::clamp<qint64>(from, 0, TotalFrames());
	to = std::clamp<qint64>(to, 0, TotalFrames());
	if (to <= from) {
		return;
	}

	SaveUndoState();
	CutWithoutUndo(from, to);
}

void AudioTimeline::CutRanges(const QVector<QPair<qint64, qint64>> &ranges)
{
	if (ranges.isEmpty()) {
		return;
	}
	SaveUndoState();
	/* from the end backwards so earlier positions stay valid */
	for (int i = int(ranges.size()) - 1; i >= 0; i--) {
		CutWithoutUndo(ranges[i].first, ranges[i].second);
	}
}

void AudioTimeline::LoadSidecar(const QString &path, qint64 offset)
{
	QFile file(path + QStringLiteral(".narration.json"));
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
	auto toFrame = [offset](double seconds) { return offset + qint64(seconds * kSampleRate); };

	QJsonArray tone = root["roomTone"].toArray();
	if (tone.size() == 2 && tone[1].toDouble() > tone[0].toDouble()) {
		roomTone.append({toFrame(tone[0].toDouble()), toFrame(tone[1].toDouble())});
	}
	for (const QJsonValue mark : root["mistakes"].toArray()) {
		mistakes.append(toFrame(mark.toDouble()));
	}
}

void AudioTimeline::ShiftMarksForCut(qint64 from, qint64 to)
{
	const qint64 length = to - from;
	QVector<qint64> keptMarks;
	for (qint64 mark : mistakes) {
		if (mark < from) {
			keptMarks.append(mark);
		} else if (mark >= to) {
			keptMarks.append(mark - length);
		}
	}
	mistakes = keptMarks;

	/* room tone that survives the cut, clipped and moved like the audio */
	QVector<QPair<qint64, qint64>> keptTone;
	for (const auto &range : roomTone) {
		auto move = [&](qint64 frame) { return frame < from ? frame : frame >= to ? frame - length : from; };
		qint64 start = move(range.first);
		qint64 end = move(range.second);
		if (end > start) {
			keptTone.append({start, end});
		}
	}
	roomTone = keptTone;
}

void AudioTimeline::RestoreState(const EditState &state)
{
	segments = state.segments;
	gain = state.gain;
	mistakes = state.mistakes;
	roomTone = state.roomTone;
}

void AudioTimeline::CutWithoutUndo(qint64 from, qint64 to)
{
	from = std::clamp<qint64>(from, 0, TotalFrames());
	to = std::clamp<qint64>(to, 0, TotalFrames());
	if (to <= from) {
		return;
	}
	ShiftMarksForCut(from, to);

	QVector<AudioSegment> kept;
	qint64 segmentStart = 0;
	for (const AudioSegment &segment : segments) {
		qint64 segmentEnd = segmentStart + segment.Length();

		/* part before the cut */
		if (segmentStart < from) {
			AudioSegment left = segment;
			left.end = segment.start + std::min(segment.Length(), from - segmentStart);
			kept.append(left);
		}
		/* part after the cut */
		if (segmentEnd > to) {
			AudioSegment right = segment;
			right.start = segment.start + std::max<qint64>(0, to - segmentStart);
			kept.append(right);
		}
		segmentStart = segmentEnd;
	}
	segments = kept;
}

void AudioTimeline::SaveUndoState()
{
	undoStack.append(CurrentState());
	/* a new edit makes the old before/after pair meaningless */
	hasBefore = false;
}

std::vector<float> AudioTimeline::Render() const
{
	std::vector<float> out(size_t(TotalFrames() * channels));
	Read(0, out.data(), TotalFrames());
	return out;
}

void AudioTimeline::ReplaceWithRendered(std::vector<float> &&samples, const QVector<QPair<qint64, qint64>> &cuts)
{
	EditState previous = CurrentState();
	SaveUndoState();

	AudioSourceClip clip;
	clip.path = sources.empty() ? QString() : sources.front().path;
	clip.samples = std::move(samples);
	clip.frames = qint64(clip.samples.size()) / channels;
	BuildPeaks(clip, channels);

	segments = {{int(sources.size()), 0, clip.frames}};
	gain = 1.0f;
	sources.push_back(std::move(clip));
	for (int i = int(cuts.size()) - 1; i >= 0; i--) {
		CutWithoutUndo(cuts[i].first, cuts[i].second);
	}

	before = previous;
	hasBefore = true;
}

void AudioTimeline::ToggleCompare()
{
	if (!hasBefore) {
		return;
	}
	EditState current = CurrentState();
	RestoreState(before);
	before = current;
	comparing = !comparing;
}

void AudioTimeline::Undo()
{
	if (comparing) {
		ToggleCompare();
	}
	hasBefore = false;
	if (!undoStack.isEmpty()) {
		RestoreState(undoStack.takeLast());
	}
}

/* loudest sample of what is left on the timeline, before our gain */
float AudioTimeline::RawPeak() const
{
	float peak = 0.0f;
	for (const AudioSegment &segment : segments) {
		const AudioSourceClip &clip = sources[size_t(segment.source)];
		const float *data = clip.samples.data();
		for (qint64 i = segment.start * channels; i < segment.end * channels; i++) {
			peak = std::max(peak, std::fabs(data[i]));
		}
	}
	return peak;
}

bool AudioTimeline::Normalize(float targetDb)
{
	float peak = RawPeak();
	if (peak <= 0.0f) {
		return false;
	}

	float newGain = std::pow(10.0f, targetDb / 20.0f) / peak;
	if (std::fabs(newGain - gain) < 1e-4f) {
		return false;
	}

	SaveUndoState();
	gain = newGain;
	return true;
}

float AudioTimeline::GainDb() const
{
	return 20.0f * std::log10(gain);
}

/* ---------------------------------------------------------------------- */

AudioTimelineDevice::AudioTimelineDevice(const AudioTimeline *timeline_, qint64 startFrame, qint64 endFrame_,
					 QObject *parent)
	: QIODevice(parent),
	  timeline(timeline_),
	  position(startFrame),
	  endFrame(endFrame_)
{
}

qint64 AudioTimelineDevice::bytesAvailable() const
{
	return (endFrame - position) * timeline->Channels() * qint64(sizeof(float)) + QIODevice::bytesAvailable();
}

qint64 AudioTimelineDevice::readData(char *data, qint64 maxSize)
{
	const qint64 frameBytes = timeline->Channels() * qint64(sizeof(float));
	qint64 frames = std::min(maxSize / frameBytes, endFrame - position);
	if (frames <= 0) {
		return 0;
	}
	qint64 copied = timeline->Read(position, reinterpret_cast<float *>(data), frames);
	position += copied;
	return copied * frameBytes;
}
