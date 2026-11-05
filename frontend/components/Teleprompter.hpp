#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QTextEdit;
class QPushButton;
class QSlider;
class QTimer;

/* Shows a script in large text and scrolls it while the narrator reads.
 * Used in place of the video preview when the app is in audio-only mode. */
class Teleprompter : public QWidget {
	Q_OBJECT

public:
	explicit Teleprompter(QWidget *parent = nullptr);
	~Teleprompter() override;

public slots:
	void StartScrolling();
	void PauseScrolling();
	void StopScrolling();

protected:
	void keyPressEvent(QKeyEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
	void ToggleScrolling();
	void ToggleEditMode();
	void OpenTextFile();
	void ScrollStep();
	void ChangeFontSize(int delta);
	void JumpToStart();

private:
	QTextEdit *textView = nullptr;
	QWidget *readingGuide = nullptr;
	QLabel *emptyHint = nullptr;
	QPushButton *playButton = nullptr;
	QPushButton *editButton = nullptr;
	QSlider *speedSlider = nullptr;
	QTimer *scrollTimer = nullptr;

	bool editing = false;
	int fontSize = 0;
	double scrollRemainder = 0.0;

	void ApplyReadingStyle();
	void UpdateEmptyState();
	void UpdatePlayButton();
	void PositionReadingGuide();
	void LoadState();
	void SaveState();
	static QString ScriptFilePath();
};
