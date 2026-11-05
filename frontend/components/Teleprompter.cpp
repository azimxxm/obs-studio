#include "Teleprompter.hpp"

#include <algorithm>

#include <OBSApp.hpp>

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <qt-wrappers.hpp>
#include <util/platform.h>

#include "moc_Teleprompter.cpp"

namespace {
constexpr int kDefaultFontSize = 48;
constexpr int kMinFontSize = 28;
constexpr int kMaxFontSize = 96;
constexpr int kFontStep = 4;
constexpr int kDefaultSpeed = 4;
constexpr int kMaxSpeed = 10;
constexpr int kTimerIntervalMs = 16;
/* pixels per second for speed 1, grows linearly with the slider */
constexpr double kPixelsPerSecondPerStep = 12.0;
constexpr double kReadingGuideRatio = 0.33;
constexpr int kMaxColumnWidth = 1200;
constexpr int kMinSideMargin = 64;
constexpr const char *kConfigSection = "Teleprompter";
} // namespace

Teleprompter::Teleprompter(QWidget *parent) : QWidget(parent)
{
	setObjectName("teleprompter");
	setFocusPolicy(Qt::StrongFocus);

	textView = new QTextEdit(this);
	textView->setObjectName("teleprompterText");
	textView->setFrameShape(QFrame::NoFrame);
	textView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	textView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	textView->setAcceptRichText(false);
	textView->setPlaceholderText(QTStr("Teleprompter.Placeholder"));
	textView->viewport()->installEventFilter(this);

	/* Thin band at one third of the height: the line to read sits here */
	readingGuide = new QWidget(textView->viewport());
	readingGuide->setObjectName("teleprompterGuide");
	readingGuide->setAttribute(Qt::WA_TransparentForMouseEvents);
	readingGuide->setStyleSheet("background-color: rgba(120, 160, 255, 40);"
				    "border-top: 1px solid rgba(120, 160, 255, 120);"
				    "border-bottom: 1px solid rgba(120, 160, 255, 120);");

	/* Shown in the middle of the text area until a script is added */
	emptyHint = new QLabel(QTStr("Teleprompter.EmptyHint"), textView->viewport());
	emptyHint->setAlignment(Qt::AlignCenter);
	emptyHint->setWordWrap(true);
	emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
	emptyHint->setStyleSheet("color: #b0b0b8; font-size: 17px;");

	auto *openButton = new QPushButton(QTStr("Teleprompter.Open"), this);
	editButton = new QPushButton(this);
	playButton = new QPushButton(this);
	playButton->setProperty("class", "btn-primary");
	playButton->setMinimumSize(160, 44);
	openButton->setMinimumHeight(36);
	editButton->setMinimumHeight(36);
	auto *topButton = new QPushButton(QTStr("Teleprompter.ToStart"), this);
	auto *smallerButton = new QPushButton("A-", this);
	auto *biggerButton = new QPushButton("A+", this);
	smallerButton->setToolTip(QTStr("Teleprompter.FontSmaller"));
	biggerButton->setToolTip(QTStr("Teleprompter.FontBigger"));

	speedSlider = new QSlider(Qt::Horizontal, this);
	speedSlider->setRange(1, kMaxSpeed);
	speedSlider->setFixedWidth(160);
	speedSlider->setToolTip(QTStr("Teleprompter.Speed"));

	auto *toolbar = new QHBoxLayout();
	toolbar->setContentsMargins(8, 8, 8, 8);
	toolbar->setSpacing(8);
	toolbar->addWidget(openButton);
	toolbar->addWidget(editButton);
	toolbar->addStretch();
	toolbar->addWidget(topButton);
	toolbar->addWidget(playButton);
	toolbar->addSpacing(12);
	toolbar->addWidget(new QLabel(QTStr("Teleprompter.Speed"), this));
	toolbar->addWidget(speedSlider);
	toolbar->addSpacing(12);
	toolbar->addWidget(smallerButton);
	toolbar->addWidget(biggerButton);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(textView, 1);
	layout->addLayout(toolbar);

	scrollTimer = new QTimer(this);
	scrollTimer->setInterval(kTimerIntervalMs);

	connect(scrollTimer, &QTimer::timeout, this, &Teleprompter::ScrollStep);
	connect(playButton, &QPushButton::clicked, this, &Teleprompter::ToggleScrolling);
	connect(editButton, &QPushButton::clicked, this, &Teleprompter::ToggleEditMode);
	connect(openButton, &QPushButton::clicked, this, &Teleprompter::OpenTextFile);
	connect(topButton, &QPushButton::clicked, this, &Teleprompter::JumpToStart);
	connect(smallerButton, &QPushButton::clicked, this, [this]() { ChangeFontSize(-kFontStep); });
	connect(biggerButton, &QPushButton::clicked, this, [this]() { ChangeFontSize(kFontStep); });
	connect(textView, &QTextEdit::textChanged, this, &Teleprompter::UpdateEmptyState);

	LoadState();
	ApplyReadingStyle();
	UpdateEmptyState();
	UpdatePlayButton();
}

Teleprompter::~Teleprompter()
{
	SaveState();
}

QString Teleprompter::ScriptFilePath()
{
	char path[512];
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/teleprompter.txt") <= 0) {
		return QString();
	}
	return QString::fromUtf8(path);
}

void Teleprompter::LoadState()
{
	config_t *config = App()->GetUserConfig();
	config_set_default_int(config, kConfigSection, "FontSize", kDefaultFontSize);
	config_set_default_int(config, kConfigSection, "Speed", kDefaultSpeed);

	fontSize = std::clamp((int)config_get_int(config, kConfigSection, "FontSize"), kMinFontSize, kMaxFontSize);
	speedSlider->setValue((int)config_get_int(config, kConfigSection, "Speed"));

	QFile file(ScriptFilePath());
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		textView->setPlainText(QString::fromUtf8(file.readAll()));
	}
}

void Teleprompter::SaveState()
{
	config_t *config = App()->GetUserConfig();
	config_set_int(config, kConfigSection, "FontSize", fontSize);
	config_set_int(config, kConfigSection, "Speed", speedSlider->value());

	QString path = ScriptFilePath();
	if (path.isEmpty()) {
		return;
	}

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		blog(LOG_WARNING, "Teleprompter: could not save script to '%s'", QT_TO_UTF8(path));
		return;
	}
	file.write(textView->toPlainText().toUtf8());
}

void Teleprompter::ApplyReadingStyle()
{
	QFont font = textView->font();
	font.setPointSize(editing ? 16 : fontSize);
	textView->setFont(font);
	textView->setReadOnly(!editing);
	textView->setPlaceholderText(editing ? QTStr("Teleprompter.Placeholder") : QString());
	/* The theme sets a font-size on every widget, so the size has to go in the stylesheet too */
	textView->setStyleSheet(
		editing ? QString("QTextEdit#teleprompterText { font-size: 16pt; }")
			: QString("QTextEdit#teleprompterText { background-color: #16171c; color: #f2f2f2;"
				  " font-size: %1pt; }")
				  .arg(fontSize));
	editButton->setText(QTStr(editing ? "Teleprompter.Done" : "Teleprompter.Edit"));

	/* Leave room above and below so the first and last lines can reach the guide,
	 * and keep the text in a centered column so long lines stay easy to follow */
	int viewportWidth = textView->viewport()->width();
	int viewportHeight = textView->viewport()->height();
	double sideMargin = editing ? 16.0 : std::max(kMinSideMargin, (viewportWidth - kMaxColumnWidth) / 2);

	QTextFrameFormat frameFormat = textView->document()->rootFrame()->frameFormat();
	frameFormat.setTopMargin(editing ? 16.0 : viewportHeight * kReadingGuideRatio);
	frameFormat.setBottomMargin(editing ? 16.0 : viewportHeight * (1.0 - kReadingGuideRatio));
	frameFormat.setLeftMargin(sideMargin);
	frameFormat.setRightMargin(sideMargin);
	textView->document()->rootFrame()->setFrameFormat(frameFormat);

	PositionReadingGuide();
	UpdateEmptyState();
}

void Teleprompter::UpdateEmptyState()
{
	bool empty = textView->toPlainText().trimmed().isEmpty();
	emptyHint->setVisible(empty && !editing);
	playButton->setEnabled(!empty);
	readingGuide->setVisible(!editing && !empty);
}

void Teleprompter::UpdatePlayButton()
{
	bool running = scrollTimer->isActive();
	playButton->setText(QTStr(running ? "Teleprompter.Pause" : "Teleprompter.Play"));
}

void Teleprompter::PositionReadingGuide()
{
	/* The first line starts at the top margin, so the guide sits exactly there */
	QFont readingFont = textView->font();
	readingFont.setPointSize(editing ? 16 : fontSize);
	QFontMetrics metrics(readingFont);
	int padding = 6;
	int lineHeight = metrics.lineSpacing() + padding * 2;
	int y = int(textView->viewport()->height() * kReadingGuideRatio) - padding;
	readingGuide->setGeometry(0, y, textView->viewport()->width(), lineHeight);
	emptyHint->setGeometry(textView->viewport()->rect().adjusted(48, 0, -48, 0));
}

bool Teleprompter::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == textView->viewport() && event->type() == QEvent::Resize) {
		ApplyReadingStyle();
	}
	return QWidget::eventFilter(watched, event);
}

void Teleprompter::keyPressEvent(QKeyEvent *event)
{
	if (editing) {
		QWidget::keyPressEvent(event);
		return;
	}

	switch (event->key()) {
	case Qt::Key_Space:
		ToggleScrolling();
		break;
	case Qt::Key_Up:
		speedSlider->setValue(speedSlider->value() + 1);
		break;
	case Qt::Key_Down:
		speedSlider->setValue(speedSlider->value() - 1);
		break;
	default:
		QWidget::keyPressEvent(event);
	}
}

void Teleprompter::StartScrolling()
{
	if (editing || textView->toPlainText().trimmed().isEmpty()) {
		return;
	}
	scrollTimer->start();
	UpdatePlayButton();
	setFocus();
}

void Teleprompter::PauseScrolling()
{
	scrollTimer->stop();
	UpdatePlayButton();
}

void Teleprompter::StopScrolling()
{
	PauseScrolling();
	scrollRemainder = 0.0;
}

void Teleprompter::ToggleScrolling()
{
	if (scrollTimer->isActive()) {
		PauseScrolling();
	} else {
		StartScrolling();
	}
}

void Teleprompter::ToggleEditMode()
{
	editing = !editing;
	StopScrolling();
	ApplyReadingStyle();
	UpdateEmptyState();
	if (editing) {
		textView->setFocus();
	} else {
		SaveState();
		setFocus();
	}
}

void Teleprompter::OpenTextFile()
{
	QString path = QFileDialog::getOpenFileName(this, QTStr("Teleprompter.Open"), QString(),
						    QTStr("Teleprompter.TextFiles") + " (*.txt *.md)");
	if (path.isEmpty()) {
		return;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		blog(LOG_WARNING, "Teleprompter: could not open '%s'", QT_TO_UTF8(path));
		return;
	}

	StopScrolling();
	textView->setPlainText(QString::fromUtf8(file.readAll()));
	JumpToStart();
	SaveState();
}

void Teleprompter::ScrollStep()
{
	QScrollBar *bar = textView->verticalScrollBar();
	if (bar->value() >= bar->maximum()) {
		PauseScrolling();
		return;
	}

	/* Keep the fractional part so slow speeds still move smoothly */
	scrollRemainder += speedSlider->value() * kPixelsPerSecondPerStep * kTimerIntervalMs / 1000.0;
	int pixels = int(scrollRemainder);
	if (pixels > 0) {
		scrollRemainder -= pixels;
		bar->setValue(bar->value() + pixels);
	}
}

void Teleprompter::ChangeFontSize(int delta)
{
	fontSize = std::clamp(fontSize + delta, kMinFontSize, kMaxFontSize);
	ApplyReadingStyle();
}

void Teleprompter::JumpToStart()
{
	textView->verticalScrollBar()->setValue(0);
	scrollRemainder = 0.0;
}
