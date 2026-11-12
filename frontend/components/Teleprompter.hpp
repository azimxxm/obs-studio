#pragma once

#include <QPointer>
#include <QWidget>

#include "ScriptMarkup.hpp"

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QTextCharFormat;
class QPushButton;
class QSlider;
class ScriptView;
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
	/* with "Follow my voice" on, the text only moves while the narrator talks */
	void SetVoiceActive(bool active);
	void BiggerText();
	void SmallerText();

public:
	/* puts a widget (the microphone button) into the toolbar before Edit Script;
	 * nullptr takes it out again */
	void SetToolbarAccessory(QWidget *widget);
	enum class ToolbarSide { Start, End };
	/* adds a button to the toolbar; it gets the toolbar's size and, unless it has its own, its style */
	void AddToolbarButton(QPushButton *button, ToolbarSide side = ToolbarSide::End);
	void SetRecording(bool recording);

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
	void InsertPause();
	void ClearMarks();
	void UpdateFormatButtons();

private:
	ScriptView *textView = nullptr;
	/* the row of marking buttons, only shown while editing */
	QWidget *formatBar = nullptr;
	QPushButton *emphasisButton = nullptr;
	QPushButton *stretchButton = nullptr;
	QPushButton *hookButton = nullptr;
	QPushButton *toneButton = nullptr;
	QPushButton *sceneButton = nullptr;
	QLabel *emptyHint = nullptr;
	QPushButton *playButton = nullptr;
	QPushButton *editButton = nullptr;
	QSlider *speedSlider = nullptr;
	QPushButton *openButton = nullptr;
	QCheckBox *followVoiceBox = nullptr;
	QHBoxLayout *toolbar = nullptr;
	QWidget *accessory = nullptr;
	bool voiceActive = false;
	QTimer *scrollTimer = nullptr;

	bool editing = false;
	int fontSize = 0;
	double scrollRemainder = 0.0;

	void BuildFormatBar();
	void ToggleMark(ScriptMarkup::Mark mark);
	void ToggleLineKind(ScriptMarkup::LineKind kind);
	QTextCharFormat FormatAtCursor() const;
	void LoadScript(const QString &markup);
	void ApplyReadingStyle();
	void UpdateEmptyState();
	void UpdatePlayButton();
	void PositionEmptyHint();
	void LoadState();
	void SaveState();
	static QString ScriptFilePath();
};
