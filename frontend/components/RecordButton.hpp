#pragma once

#include <QElapsedTimer>
#include <QPushButton>

class QTimer;

/* "● Record ⌘R" in the teleprompter's row. While recording it turns red and
 * shows the time, so the big Controls panel can stay hidden. */
class RecordButton : public QPushButton {
public:
	explicit RecordButton(QWidget *parent = nullptr);

	void RecordingStarted();
	void RecordingPaused();
	void RecordingUnpaused();
	void RecordingStopped();

private:
	QTimer *clockTimer = nullptr;
	/* time recorded before the last pause, the running part is in runningClock */
	qint64 recordedMs = 0;
	QElapsedTimer runningClock;
	bool recording = false;

	void UpdateLook();
};
