#pragma once

#include <obs.hpp>

#include <QPushButton>

#include <atomic>

class QMenu;
class QTimer;

/* "🎤 Wireless Mic Rx ▾": a toolbar button that shows which microphone is in
 * use and opens a menu of all microphones. The list is read fresh every time
 * the menu opens, so a mic plugged in a moment ago is already there.
 * A thin level line along its bottom edge shows how loud the voice is
 * (green, yellow when loud, red when close to clipping). */
class MicrophoneButton : public QPushButton {
	Q_OBJECT

public:
	explicit MicrophoneButton(QWidget *parent = nullptr);
	~MicrophoneButton() override;

	void SetSource(obs_source_t *source);

protected:
	void paintEvent(QPaintEvent *event) override;

private slots:
	void FillMenu();

private:
	OBSWeakSource weakSource;
	QMenu *menu = nullptr;
	obs_volmeter_t *volmeter = nullptr;
	QTimer *levelTimer = nullptr;
	/* written on the audio thread, read by the paint code */
	std::atomic<float> levelDb{-100.0f};
	float shownDb = -100.0f;

	void AttachLevel(obs_source_t *source);
	void DetachLevel();
	static void LevelCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);

	void SelectDevice(const QString &deviceId);
	void UpdateText();
};
