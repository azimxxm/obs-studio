#pragma once

#include <components/AudioTimeline.hpp>

#include <QDialog>
#include <QPointer>

class QAudioSink;
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
	void Export();
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
	QPushButton *exportButton = nullptr;

	QAudioSink *sink = nullptr;
	QPointer<AudioTimelineDevice> device;
	QTimer *playheadTimer = nullptr;

	void LoadTakes();
	void StartPlayback(qint64 from, qint64 to);
	void StopPlayback();
	bool IsPlaying() const { return sink != nullptr; }
};
