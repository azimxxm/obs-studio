#pragma once

#include <QWidget>

class AudioTimeline;

/* Draws the joined takes, lets the user drag to select a part and shows the playhead.
 * Ctrl/Cmd + wheel zooms, plain wheel scrolls when zoomed in. */
class WaveformView : public QWidget {
	Q_OBJECT

public:
	/* Select drags out a selection, Hand drags the timeline left and right */
	enum class Tool { Select, Hand };

	explicit WaveformView(QWidget *parent = nullptr);

	void SetTool(Tool tool);
	Tool CurrentTool() const { return tool; }

	void SetTimeline(const AudioTimeline *timeline);
	void TimelineChanged();

	bool HasSelection() const { return selectionEnd > selectionStart; }
	qint64 SelectionStart() const { return selectionStart; }
	qint64 SelectionEnd() const { return selectionEnd; }
	void ClearSelection();
	void SelectAll();

	qint64 Cursor() const { return cursor; }
	void SetPlayhead(qint64 frame);

	void ZoomIn();
	void ZoomOut();
	void ZoomToFit();

signals:
	void SelectionChanged();
	void CursorMoved(qint64 frame);
	void ToolChanged(WaveformView::Tool tool);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	const AudioTimeline *timeline = nullptr;

	/* visible part of the timeline, in frames */
	qint64 viewStart = 0;
	qint64 viewLength = 0;

	qint64 selectionStart = 0;
	qint64 selectionEnd = 0;
	qint64 cursor = 0;
	qint64 playhead = -1;

	bool dragging = false;
	qint64 dragAnchor = 0;

	Tool tool = Tool::Select;
	bool panning = false;
	double panStartX = 0.0;
	qint64 panStartView = 0;

	qint64 FrameAtX(double x) const;
	double XAtFrame(qint64 frame) const;
	void SetView(qint64 start, qint64 length);
	void ZoomAround(qint64 frame, double factor);
	void DrawRuler(QPainter &painter, const QRect &area);
};
