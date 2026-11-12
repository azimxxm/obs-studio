#pragma once

#include <QWidget>

#include <functional>

/* A thin bar the narrator grabs and drags up or down, like the wall between
 * the script and the takes list. It only reports how far it was dragged;
 * the main window decides what to open, close or resize. */
class PanelGrip : public QWidget {
public:
	explicit PanelGrip(QWidget *parent = nullptr);

	std::function<void()> onPress;
	/* pixels moved up since the press: positive is up, negative is down */
	std::function<void(int)> onDrag;
	std::function<void()> onRelease;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	double pressY = 0.0;
	bool dragging = false;
	bool hovered = false;
};
