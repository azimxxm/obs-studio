#include "RecordButton.hpp"

#include <OBSApp.hpp>

#include <QKeySequence>
#include <QTimer>

#include <qt-wrappers.hpp>

namespace {
constexpr int kClockIntervalMs = 250;
constexpr int kToolbarFontPx = 13;
constexpr int kMsPerSecond = 1000;
constexpr int kSecondsPerMinute = 60;
const QString kIdleStyle = QStringLiteral("font-size: %1px; padding: 0 14px; font-weight: 600;").arg(kToolbarFontPx);
const QString kRecordingStyle = QStringLiteral("font-size: %1px; padding: 0 14px; font-weight: 600;"
					       "background-color: #c62f3a; color: white; border-color: #e0414d;")
					.arg(kToolbarFontPx);
} // namespace

RecordButton::RecordButton(QWidget *parent) : QPushButton(parent)
{
	setToolTip(QTStr("AudioOnly.RecordTip"));
	clockTimer = new QTimer(this);
	clockTimer->setInterval(kClockIntervalMs);
	connect(clockTimer, &QTimer::timeout, this, &RecordButton::UpdateLook);
	UpdateLook();
}

void RecordButton::RecordingStarted()
{
	recording = true;
	recordedMs = 0;
	runningClock.start();
	clockTimer->start();
	UpdateLook();
}

void RecordButton::RecordingPaused()
{
	if (runningClock.isValid()) {
		recordedMs += runningClock.elapsed();
		runningClock.invalidate();
	}
	UpdateLook();
}

void RecordButton::RecordingUnpaused()
{
	runningClock.start();
	UpdateLook();
}

void RecordButton::RecordingStopped()
{
	recording = false;
	runningClock.invalidate();
	clockTimer->stop();
	UpdateLook();
}

void RecordButton::UpdateLook()
{
	QString key = QKeySequence(Qt::CTRL | Qt::Key_R).toString(QKeySequence::NativeText);
	if (!recording) {
		setText(QStringLiteral("●  ") + QTStr("AudioOnly.Record") + QStringLiteral("   ") + key);
		setStyleSheet(kIdleStyle);
		return;
	}

	qint64 ms = recordedMs + (runningClock.isValid() ? runningClock.elapsed() : 0);
	qint64 seconds = ms / kMsPerSecond;
	QString time = QStringLiteral("%1:%2")
			       .arg(seconds / kSecondsPerMinute, 2, 10, QChar(u'0'))
			       .arg(seconds % kSecondsPerMinute, 2, 10, QChar(u'0'));
	setText(QStringLiteral("■  ") + QTStr("AudioOnly.Stop") + QStringLiteral("   ") + time);
	setStyleSheet(kRecordingStyle);
}
