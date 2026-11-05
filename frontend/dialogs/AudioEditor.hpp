#pragma once

#include <components/AudioTimeline.hpp>
#include <utility/VoiceEffects.hpp>

#include <QDialog>
#include <QPointer>

#include <functional>

class QAudioSink;
class QMenu;
class QLabel;
class QPushButton;
class QTimer;
class WaveformView;

/* Joins the chosen takes one after another, lets the narrator cut out the
 * parts they don't want, and exports everything as one WAV file. */
class AudioEditor : public QDialog {
	Q_OBJECT

public:
	static constexpr const char *kExportPrefix = "Narration ";

	AudioEditor(const QStringList &paths, const QString &exportFolder, QWidget *parent = nullptr);
	~AudioEditor() override;

signals:
	void Exported(const QString &path);

protected:
	void showEvent(QShowEvent *event) override;
	void reject() override;

private slots:
	void TogglePlayback();
	void PlaySelection();
	void CutSelection();
	void UndoCut();
	void NormalizeVolume();
	void PolishVoice();
	void RemoveMistakes();
	void ToggleCompare();
	/* 0 = WAV as is, 1 = YouTube/Instagram, 2 = Podcast MP3, 3 = Audiobook (ACX) */
	void Export(int preset = 0);
	void UpdatePlayhead();
	void UpdateInfo();

private:
	QStringList paths;
	QString exportFolder;
	AudioTimeline timeline;
	bool loaded = false;

	WaveformView *waveform = nullptr;
	QLabel *infoLabel = nullptr;
	QPushButton *playButton = nullptr;
	QPushButton *playSelectionButton = nullptr;
	QPushButton *cutButton = nullptr;
	QPushButton *undoButton = nullptr;
	QPushButton *normalizeButton = nullptr;
	QPushButton *polishButton = nullptr;
	QPushButton *mistakesButton = nullptr;
	QPushButton *effectsButton = nullptr;
	QPushButton *compareButton = nullptr;
	QPushButton *selectToolButton = nullptr;
	QPushButton *handToolButton = nullptr;
	bool processing = false;
	QPushButton *exportButton = nullptr;

	QAudioSink *sink = nullptr;
	QPointer<AudioTimelineDevice> device;
	QTimer *playheadTimer = nullptr;

	void LoadTakes();
	QMenu *CreateEffectsMenu();
	/* work runs on a background thread with a copy of the audio; it may fill
	 * cuts with parts to remove from the processed audio */
	using EffectWork = std::function<bool(std::vector<float> &samples, int channels,
					      QVector<QPair<qint64, qint64>> &cuts)>;
	void RunEffect(const QString &name, EffectWork work);
	/* runs work only on the selected part when there is one */
	EffectWork OnSelection(EffectWork work) const;
	void ApplyFade(bool fadeIn);
	void ChangeVolume();
	std::vector<VoiceEffects::Range> RoomToneRanges() const;
	static double LoudnessTarget();
	static void SetLoudnessTarget(double lufs);
	void StartPlayback(qint64 from, qint64 to);
	void StopPlayback();
	bool IsPlaying() const { return sink != nullptr; }
};
