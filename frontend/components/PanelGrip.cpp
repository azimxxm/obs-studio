#include "PanelGrip.hpp"

#include <QMouseEvent>
#include <QPainter>

namespace {
constexpr int kGripHeight = 8;
constexpr double kPillWidth = 44.0;
constexpr double kPillHeight = 4.0;
const QColor kPillColor(255, 255, 255, 60);
const QColor kPillHoverColor(139, 92, 246, 220);
} // namespace

PanelGrip::PanelGrip(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(kGripHeight);
	setCursor(Qt::SplitVCursor);
	setAttribute(Qt::WA_Hover);
}

void PanelGrip::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(hovered || dragging ? kPillHoverColor : kPillColor);
	QRectF pill((width() - kPillWidth) / 2.0, (height() - kPillHeight) / 2.0, kPillWidth, kPillHeight);
	painter.drawRoundedRect(pill, kPillHeight / 2.0, kPillHeight / 2.0);
}

void PanelGrip::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		return;
	}
	dragging = true;
	pressY = event->globalPosition().y();
	if (onPress) {
		onPress();
	}
	update();
}

void PanelGrip::mouseMoveEvent(QMouseEvent *event)
{
	if (dragging && onDrag) {
		onDrag(int(pressY - event->globalPosition().y()));
	}
}

void PanelGrip::mouseReleaseEvent(QMouseEvent *)
{
	if (!dragging) {
		return;
	}
	dragging = false;
	if (onRelease) {
		onRelease();
	}
	update();
}

void PanelGrip::enterEvent(QEnterEvent *)
{
	hovered = true;
	update();
}

void PanelGrip::leaveEvent(QEvent *)
{
	hovered = false;
	update();
}
