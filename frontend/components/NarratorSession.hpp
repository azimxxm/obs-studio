#pragma once

#include <obs.hpp>

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <atomic>

class QLabel;
class QTimer;
class QWidget;

/* Everything that happens around one narration take in audio-only mode:
 * the 3-2-1 countdown (recorded as room tone), mistake marks (M key) and
 * voice detection for the teleprompter. When the take stops,
 * the room tone and marks are saved next to it as "<take>.narration.json". */
class NarratorSession : public QObject {
	Q_OBJECT

public:
	explicit NarratorSession(QWidget *overlayParent, QObject *parent = nullptr);
	~NarratorSession() override;

	void AttachMicrophone(obs_source_t *source);
	bool IsCountingDown() const { return countdownLeft > 0; }

	/* sidecar file for a take; empty values when it has none */
	static QString SidecarPath(const QString &takePath);

public slots:
	void RecordingStarted();
	void RecordingPaused();
	void RecordingUnpaused();
	void RecordingStopped(const QString &takePath);
	void MarkMistake();

signals:
	/* the countdown is over, the narrator may start reading */
	void SpeakNow();
	void VoiceActivityChanged(bool speaking);

private slots:
	void CountdownTick();
	void DetectVoice();

private:
	QPointer<QWidget> overlayParent;
	QLabel *overlay = nullptr;
	QTimer *countdownTimer = nullptr;
	QTimer *voiceTimer = nullptr;
	QTimer *overlayHideTimer = nullptr;

	int countdownLeft = 0;
	bool recording = false;
	QElapsedTimer clock;
	qint64 pausedMs = 0;
	qint64 pauseStartedMs = -1;
	double roomToneEnd = 0.0;
	QVector<double> mistakes;

	obs_volmeter_t *volmeter = nullptr;
	std::atomic<float> levelDb{-100.0f};
	float noiseFloorDb = -60.0f;
	qint64 lastVoiceMs = -100000;
	bool speaking = false;
	QElapsedTimer uptime;

	double ElapsedSeconds() const;
	void ShowCard(const QString &title, const QString &subtitle, const QColor &color, int titlePx,
		      int hideAfterMs);
	void HideCard();
	void PositionOverlay();
	void DetachMicrophone();
	bool eventFilter(QObject *watched, QEvent *event) override;
	static void VolmeterCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				     const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);
};
