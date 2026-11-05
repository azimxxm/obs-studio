#pragma once

#include <QIODevice>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

/* One decoded take, kept in memory as interleaved float samples. */
struct AudioSourceClip {
	QString path;
	std::vector<float> samples;
	/* max(|sample|) for every kPeakBlock frames, used to draw the waveform fast */
	std::vector<float> peaks;
	qint64 frames = 0;
};

/* A piece of one source clip that stays in the final audio. */
struct AudioSegment {
	int source = 0;
	qint64 start = 0; // first frame, inclusive
	qint64 end = 0;   // last frame, exclusive

	qint64 Length() const { return end - start; }
};

/* Non-destructive edit list: the takes are joined one after another and cuts
 * only change the segment list, so undo is cheap and the audio is never copied. */
class AudioTimeline {
public:
	static constexpr int kSampleRate = 48000;
	static constexpr int kPeakBlock = 256;

	bool Load(const QStringList &paths, QString &error);

	int Channels() const { return channels; }
	/* how many files were opened; effects add clips but not takes */
	int TakeCount() const { return takeCount; }
	qint64 TotalFrames() const;
	const QVector<AudioSegment> &Segments() const { return segments; }
	const std::vector<AudioSourceClip> &Sources() const { return sources; }

	/* timeline frames where one segment ends and the next one starts */
	QVector<qint64> Boundaries() const;

	/* peak value for the timeline range [from, to) */
	float PeakAt(qint64 from, qint64 to) const;

	/* copies frames starting at timeline position into out, returns frames copied */
	qint64 Read(qint64 position, float *out, qint64 frameCount) const;

	void Cut(qint64 from, qint64 to);
	/* raises the volume so the loudest point sits at targetDb, like Audacity's Normalize */
	bool Normalize(float targetDb);
	float GainDb() const;
	bool CanUndo() const { return !undoStack.isEmpty(); }
	void Undo();

	/* the whole edited audio as one buffer, the way it will be exported */
	std::vector<float> Render() const;
	/* puts processed audio (from an effect) in place of the timeline as one undo
	 * step; cuts (positions in the new audio) are applied in the same step */
	void ReplaceWithRendered(std::vector<float> &&samples, const QVector<QPair<qint64, qint64>> &cuts = {});
	/* cuts several parts as one step, ranges must not overlap */
	void CutRanges(const QVector<QPair<qint64, qint64>> &ranges);

	/* temporary volume change on [from, to) for previewing; it is heard and
	 * drawn but not saved until the effect itself is applied */
	void SetPreviewGain(qint64 from, qint64 to, float gainDb);
	void ClearPreview();

	/* switch between the audio before and after the last effect */
	bool CanCompare() const { return hasBefore; }
	bool IsComparing() const { return comparing; }
	void ToggleCompare();

	bool ExportWav(const QString &path, QString &error) const;

private:
	int channels = 1;
	int takeCount = 0;
	std::vector<AudioSourceClip> sources;
	struct EditState {
		QVector<AudioSegment> segments;
		float gain = 1.0f;
	};

	QVector<AudioSegment> segments;
	float gain = 1.0f;
	QVector<EditState> undoStack;
	qint64 previewFrom = 0;
	qint64 previewTo = 0;
	float previewGain = 1.0f;
	EditState before;
	bool hasBefore = false;
	bool comparing = false;

	bool Decode(const QString &path, AudioSourceClip &clip, QString &error) const;
	static void BuildPeaks(AudioSourceClip &clip, int channels);
	float FadeGain(qint64 position) const;
	float RawPeak() const;
	void SaveUndoState();
	void CutWithoutUndo(qint64 from, qint64 to);
};

/* Feeds the timeline to QAudioSink while the user listens in the editor. */
class AudioTimelineDevice : public QIODevice {
	Q_OBJECT

public:
	AudioTimelineDevice(const AudioTimeline *timeline, qint64 startFrame, qint64 endFrame,
			    QObject *parent = nullptr);

	qint64 CurrentFrame() const { return position; }
	bool atEnd() const override { return position >= endFrame; }
	qint64 bytesAvailable() const override;

protected:
	qint64 readData(char *data, qint64 maxSize) override;
	qint64 writeData(const char *, qint64) override { return -1; }

private:
	const AudioTimeline *timeline;
	qint64 position;
	qint64 endFrame;
};

/* Reads the channel count from a WAV header, returns 0 when the file is not a WAV. */
int ReadWavChannels(const QString &path);
/* Length of a WAV file in seconds from its header, -1 when unknown. */
double ReadWavDurationSeconds(const QString &path);
