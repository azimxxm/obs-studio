#pragma once

#include <obs.hpp>

#include <QPushButton>

class QMenu;

/* "🎤 Wireless Mic Rx ▾": a toolbar button that shows which microphone is in
 * use and opens a menu of all microphones. The list is read fresh every time
 * the menu opens, so a mic plugged in a moment ago is already there. */
class MicrophoneButton : public QPushButton {
	Q_OBJECT

public:
	explicit MicrophoneButton(QWidget *parent = nullptr);

	void SetSource(obs_source_t *source);

private slots:
	void FillMenu();

private:
	OBSWeakSource weakSource;
	QMenu *menu = nullptr;

	void SelectDevice(const QString &deviceId);
	void UpdateText();
};
