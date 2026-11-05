#include "WaveformView.hpp"
#include "AudioTimeline.hpp"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include "moc_WaveformView.cpp"

namespace {
constexpr int kRulerHeight = 22;
constexpr qint64 kMinViewFrames = AudioTimeline::kSampleRate / 4;
constexpr double kZoomStep = 1.5;
/* a press that moves less than this is a click, not a selection */
constexpr int kDragThresholdPx = 3;

const QColor kBackground(22, 23, 28);
const QColor kWave(110, 200, 140);
const QColor kWaveSelected(240, 240, 240);
const QColor kSelection(120, 160, 255, 70);
const QColor kBoundary(255, 196, 80);
const QColor kPlayhead(229, 72, 77);
const QColor kRulerText(160, 160, 170);

QString FormatTime(double seconds, bool withTenths)
{
	int total = int(seconds);
	QString text = QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
	if (withTenths) {
		text += QStringLiteral(".%1").arg(int(std::lround((seconds - total) * 10)) % 10);
	}
	return text;
}
} // namespace

WaveformView::WaveformView(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(220);
	setMouseTracking(false);
	setFocusPolicy(Qt::StrongFocus);
	setCursor(Qt::IBeamCursor);
}

void WaveformView::SetTool(Tool newTool)
{
	if (tool == newTool) {
		return;
	}
	tool = newTool;
	panning = false;
	dragging = false;
	setCursor(tool == Tool::Hand ? Qt::OpenHandCursor : Qt::IBeamCursor);
	emit ToolChanged(tool);
}

void WaveformView::SetTimeline(const AudioTimeline *timeline_)
{
	timeline = timeline_;
	ZoomToFit();
}

void WaveformView::TimelineChanged()
{
	qint64 total = timeline ? timeline->TotalFrames() : 0;
	cursor = std::min(cursor, total);
	ClearSelection();
	SetView(viewStart, std::min(viewLength, std::max<qint64>(total, 1)));
}

void WaveformView::ClearSelection()
{
	selectionStart = selectionEnd = 0;
	emit SelectionChanged();
	update();
}

void WaveformView::SelectAll()
{
	if (!timeline || timeline->TotalFrames() == 0) {
		return;
	}
	selectionStart = 0;
	selectionEnd = timeline->TotalFrames();
	cursor = 0;
	emit SelectionChanged();
	update();
}

void WaveformView::SetPlayhead(qint64 frame)
{
	playhead = frame;

	/* keep the playhead on screen while zoomed in */
	if (frame >= 0 && (frame < viewStart || frame > viewStart + viewLength)) {
		SetView(frame - viewLength / 10, viewLength);
	}
	update();
}

void WaveformView::ZoomIn()
{
	ZoomAround(playhead >= 0 ? playhead : cursor, 1.0 / kZoomStep);
}

void WaveformView::ZoomOut()
{
	ZoomAround(playhead >= 0 ? playhead : cursor, kZoomStep);
}

void WaveformView::ZoomToFit()
{
	SetView(0, timeline ? std::max<qint64>(timeline->TotalFrames(), 1) : 1);
}

void WaveformView::SetView(qint64 start, qint64 length)
{
	qint64 total = timeline ? std::max<qint64>(timeline->TotalFrames(), 1) : 1;
	viewLength = std::clamp<qint64>(length, std::min(kMinViewFrames, total), total);
	viewStart = std::clamp<qint64>(start, 0, total - viewLength);
	update();
}

void WaveformView::ZoomAround(qint64 frame, double factor)
{
	double ratio = viewLength > 0 ? double(frame - viewStart) / double(viewLength) : 0.5;
	ratio = std::clamp(ratio, 0.0, 1.0);
	qint64 newLength = qint64(double(viewLength) * factor);
	SetView(frame - qint64(ratio * double(newLength)), newLength);
}

qint64 WaveformView::FrameAtX(double x) const
{
	double ratio = std::clamp(x / std::max(1, width()), 0.0, 1.0);
	return viewStart + qint64(ratio * double(viewLength));
}

double WaveformView::XAtFrame(qint64 frame) const
{
	return viewLength > 0 ? double(frame - viewStart) * width() / double(viewLength) : 0.0;
}

void WaveformView::DrawRuler(QPainter &painter, const QRect &area)
{
	const double seconds = double(viewLength) / AudioTimeline::kSampleRate;
	/* pick a tick step that leaves roughly 90 px between labels */
	const double steps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
	double step = steps[std::size(steps) - 1];
	for (double s : steps) {
		if (s / seconds * width() >= 90) {
			step = s;
			break;
		}
	}

	painter.setPen(kRulerText);
	double first = std::ceil(double(viewStart) / AudioTimeline::kSampleRate / step) * step;
	for (double t = first; t * AudioTimeline::kSampleRate < viewStart + viewLength; t += step) {
		double x = XAtFrame(qint64(t * AudioTimeline::kSampleRate));
		painter.drawLine(QPointF(x, area.bottom() - 5), QPointF(x, area.bottom()));
		painter.drawText(QPointF(x + 3, area.bottom() - 7), FormatTime(t, step < 1.0));
	}
}

void WaveformView::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.fillRect(rect(), kBackground);

	if (!timeline || timeline->TotalFrames() == 0) {
		return;
	}

	QRect ruler(0, 0, width(), kRulerHeight);
	QRect wave(0, kRulerHeight, width(), height() - kRulerHeight);
	DrawRuler(painter, ruler);

	if (HasSelection()) {
		double x1 = XAtFrame(selectionStart);
		double x2 = XAtFrame(selectionEnd);
		painter.fillRect(QRectF(x1, wave.top(), std::max(1.0, x2 - x1), wave.height()), kSelection);
	}

	/* one vertical line per pixel column, from the precomputed peaks */
	const double mid = wave.center().y();
	const double halfHeight = wave.height() / 2.0 - 4;
	const double framesPerPixel = double(viewLength) / std::max(1, width());
	for (int x = 0; x < width(); x++) {
		qint64 from = viewStart + qint64(x * framesPerPixel);
		qint64 to = std::max(from + 1, viewStart + qint64((x + 1) * framesPerPixel));
		float peak = timeline->PeakAt(from, to);
		bool selected = HasSelection() && from >= selectionStart && from < selectionEnd;
		painter.setPen(selected ? kWaveSelected : kWave);
		double h = std::max(0.5, double(peak) * halfHeight);
		painter.drawLine(QPointF(x + 0.5, mid - h), QPointF(x + 0.5, mid + h));
	}

	/* dashed line where one take (or a cut) joins the next */
	painter.setPen(QPen(kBoundary, 1, Qt::DashLine));
	for (qint64 boundary : timeline->Boundaries()) {
		double x = XAtFrame(boundary);
		if (x >= 0 && x <= width()) {
			painter.drawLine(QPointF(x, wave.top()), QPointF(x, wave.bottom()));
		}
	}

	painter.setPen(QPen(kWaveSelected, 1));
	double cursorX = XAtFrame(cursor);
	painter.drawLine(QPointF(cursorX, wave.top()), QPointF(cursorX, wave.bottom()));

	if (playhead >= 0) {
		painter.setPen(QPen(kPlayhead, 2));
		double x = XAtFrame(playhead);
		painter.drawLine(QPointF(x, 0), QPointF(x, height()));
	}
}

void WaveformView::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || !timeline) {
		return;
	}
	if (tool == Tool::Hand) {
		panning = true;
		panStartX = event->position().x();
		panStartView = viewStart;
		setCursor(Qt::ClosedHandCursor);
		return;
	}
	dragging = true;
	dragAnchor = FrameAtX(event->position().x());

	/* shift-click grows the current selection */
	if (event->modifiers() & Qt::ShiftModifier) {
		qint64 anchor = HasSelection() ? selectionStart : cursor;
		selectionStart = std::min(anchor, dragAnchor);
		selectionEnd = std::max(anchor, dragAnchor);
		dragAnchor = anchor;
		emit SelectionChanged();
		update();
	}
}

void WaveformView::mouseMoveEvent(QMouseEvent *event)
{
	if (panning) {
		/* the audio follows the hand: drag right to go back in time */
		double framesPerPixel = double(viewLength) / std::max(1, width());
		qint64 shift = qint64((panStartX - event->position().x()) * framesPerPixel);
		SetView(panStartView + shift, viewLength);
		return;
	}
	if (!dragging) {
		return;
	}
	qint64 frame = FrameAtX(event->position().x());
	if (std::abs(XAtFrame(frame) - XAtFrame(dragAnchor)) < kDragThresholdPx && !HasSelection()) {
		return;
	}
	selectionStart = std::min(dragAnchor, frame);
	selectionEnd = std::max(dragAnchor, frame);
	emit SelectionChanged();
	update();
}

void WaveformView::mouseReleaseEvent(QMouseEvent *event)
{
	if (panning && event->button() == Qt::LeftButton) {
		panning = false;
		setCursor(Qt::OpenHandCursor);
		return;
	}
	if (!dragging || event->button() != Qt::LeftButton) {
		return;
	}
	dragging = false;

	qint64 frame = FrameAtX(event->position().x());
	bool isClick = std::abs(XAtFrame(frame) - XAtFrame(dragAnchor)) < kDragThresholdPx &&
		       !(event->modifiers() & Qt::ShiftModifier);
	if (isClick) {
		ClearSelection();
		cursor = frame;
		emit CursorMoved(cursor);
	} else {
		cursor = selectionStart;
	}
	update();
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || tool == Tool::Hand) {
		return;
	}
	/* the release after a double click must not turn this back into a click */
	dragging = false;
	SelectAll();
}

void WaveformView::wheelEvent(QWheelEvent *event)
{
	if (!timeline) {
		return;
	}

	const QPoint delta = event->angleDelta();
	if (event->modifiers() & Qt::ControlModifier) {
		double factor = delta.y() > 0 ? 1.0 / 1.15 : 1.15;
		ZoomAround(FrameAtX(event->position().x()), factor);
	} else {
		/* trackpads send x, mouse wheels send y; both pan the view */
		int pixels = delta.x() != 0 ? delta.x() : delta.y();
		qint64 shift = qint64(-pixels / 8.0 * double(viewLength) / std::max(1, width()) * 4);
		SetView(viewStart + shift, viewLength);
	}
	event->accept();
}
