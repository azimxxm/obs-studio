#include "Teleprompter.hpp"

#include <algorithm>

#include <OBSApp.hpp>

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <qt-wrappers.hpp>
#include <util/platform.h>

#include "ScriptView.hpp"

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
/* the first line starts a third of the way down, where the eye reads */
constexpr double kReadingGuideRatio = 0.33;
/* wide enough to fill a maximized window, the side margins only keep text off the edges */
constexpr int kMaxColumnWidth = 2400;
constexpr int kMinSideMargin = 32;
constexpr int kToolbarButtonHeight = 36;
constexpr int kToolbarFontPx = 13;
constexpr int kPlayButtonWidth = 140;
constexpr double kEditMargin = 24.0;
const char *const kEmphasisButtonColor = "#ffa23f";
constexpr const char *kConfigSection = "Teleprompter";
} // namespace

Teleprompter::Teleprompter(QWidget *parent) : QWidget(parent)
{
	setObjectName("teleprompter");
	setFocusPolicy(Qt::StrongFocus);

	textView = new ScriptView(this);
	textView->setObjectName("teleprompterText");
	textView->setFrameShape(QFrame::NoFrame);
	textView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	textView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	textView->setAcceptRichText(false);
	textView->setPlaceholderText(QTStr("Teleprompter.Placeholder"));
	textView->viewport()->installEventFilter(this);

	/* Shown in the middle of the text area until a script is added */
	emptyHint = new QLabel(QTStr("Teleprompter.EmptyHint"), textView->viewport());
	emptyHint->setAlignment(Qt::AlignCenter);
	emptyHint->setWordWrap(true);
	emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
	emptyHint->setStyleSheet("color: #b0b0b8; font-size: 17px;");

	editButton = new QPushButton(this);
	openButton = new QPushButton(QTStr("Teleprompter.Open"), this);
	followVoiceBox = new QCheckBox(QTStr("Teleprompter.FollowVoice"), this);
	followVoiceBox->setToolTip(QTStr("Teleprompter.FollowVoiceTip"));
	followVoiceBox->setFocusPolicy(Qt::NoFocus);
	followVoiceBox->setStyleSheet(QStringLiteral("font-size: %1px;").arg(kToolbarFontPx));
	playButton = new QPushButton(this);
	playButton->setProperty("class", "btn-primary");
	auto *topButton = new QPushButton(QTStr("Teleprompter.ToStart"), this);
	/* the buttons show their own shortcut: Cmd- and Cmd+ */
	auto *smallerButton = new QPushButton(
		QStringLiteral("A−   ") + QKeySequence(QKeySequence::ZoomOut).toString(QKeySequence::NativeText), this);
	auto *biggerButton = new QPushButton(
		QStringLiteral("A+   ") + QKeySequence(QKeySequence::ZoomIn).toString(QKeySequence::NativeText), this);
	smallerButton->setToolTip(QTStr("Teleprompter.FontSmaller"));
	biggerButton->setToolTip(QTStr("Teleprompter.FontBigger"));

	speedSlider = new QSlider(Qt::Horizontal, this);
	speedSlider->setRange(1, kMaxSpeed);
	speedSlider->setFixedWidth(160);
	speedSlider->setToolTip(QTStr("Teleprompter.Speed"));

	/* one height and one text size for every button in the bar */
	for (QPushButton *button : {editButton, openButton, topButton, playButton, smallerButton, biggerButton}) {
		button->setFocusPolicy(Qt::NoFocus);
		button->setFixedHeight(kToolbarButtonHeight);
		button->setStyleSheet(QStringLiteral("font-size: %1px; padding: 0 14px;").arg(kToolbarFontPx));
	}
	/* Start Scrolling / Pause swap text, a fixed width stops the bar from jumping */
	playButton->setMinimumWidth(kPlayButtonWidth);

	toolbar = new QHBoxLayout();
	toolbar->setContentsMargins(8, 8, 8, 8);
	toolbar->setSpacing(8);
	toolbar->addWidget(editButton);
	toolbar->addWidget(openButton);
	toolbar->addSpacing(4);
	toolbar->addWidget(followVoiceBox);
	toolbar->addStretch();
	toolbar->addWidget(topButton);
	toolbar->addWidget(playButton);
	toolbar->addSpacing(12);
	auto *speedLabel = new QLabel(QTStr("Teleprompter.Speed"), this);
	speedLabel->setStyleSheet(QStringLiteral("font-size: %1px;").arg(kToolbarFontPx));
	toolbar->addWidget(speedLabel);
	toolbar->addWidget(speedSlider);
	toolbar->addSpacing(12);
	toolbar->addWidget(smallerButton);
	toolbar->addWidget(biggerButton);

	BuildFormatBar();

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(formatBar);
	layout->addWidget(textView, 1);
	layout->addLayout(toolbar);

	scrollTimer = new QTimer(this);
	scrollTimer->setInterval(kTimerIntervalMs);

	connect(scrollTimer, &QTimer::timeout, this, &Teleprompter::ScrollStep);
	connect(playButton, &QPushButton::clicked, this, &Teleprompter::ToggleScrolling);
	connect(editButton, &QPushButton::clicked, this, &Teleprompter::ToggleEditMode);
	connect(openButton, &QPushButton::clicked, this, &Teleprompter::OpenTextFile);
	connect(topButton, &QPushButton::clicked, this, &Teleprompter::JumpToStart);
	connect(smallerButton, &QPushButton::clicked, this, &Teleprompter::SmallerText);
	connect(biggerButton, &QPushButton::clicked, this, &Teleprompter::BiggerText);
	connect(textView, &QTextEdit::textChanged, this, &Teleprompter::UpdateEmptyState);
	connect(textView, &QTextEdit::cursorPositionChanged, this, &Teleprompter::UpdateFormatButtons);

	LoadState();
	ApplyReadingStyle();
	UpdateEmptyState();
	UpdatePlayButton();
}

Teleprompter::~Teleprompter()
{
	SaveState();
}

/* The marking buttons: select text, click a button, see the result right away */
void Teleprompter::BuildFormatBar()
{
	formatBar = new QWidget(this);
	auto *bar = new QHBoxLayout(formatBar);
	bar->setContentsMargins(8, 8, 8, 8);
	bar->setSpacing(8);

	/* every button has a Cmd + number key, shown on the button like A+ and A- do;
	 * the keys only work while the bar is shown, so only while editing */
	auto addButton = [&](const char *name, Qt::Key keyName, bool checkable, const QString &extraStyle = QString()) {
		QKeySequence shortcut(Qt::CTRL | keyName);
		QString keyText = shortcut.toString(QKeySequence::NativeText);
		QString label = QTStr(QString("Teleprompter.Format.%1").arg(name).toUtf8().constData());
		auto *button = new QPushButton(label + QStringLiteral("   ") + keyText, formatBar);
		/* a button's own shortcut clicks it ~100 ms later, by then the selection may be another one */
		auto *key = new QShortcut(shortcut, formatBar);
		connect(key, &QShortcut::activated, button, &QPushButton::click);
		button->setToolTip(QTStr(QString("Teleprompter.Format.%1Tip").arg(name).toUtf8().constData()) +
				   QStringLiteral(" (") + keyText + QStringLiteral(")"));
		button->setCheckable(checkable);
		/* a click must not take the selection away from the text */
		button->setFocusPolicy(Qt::NoFocus);
		button->setFixedHeight(kToolbarButtonHeight);
		button->setStyleSheet(
			QStringLiteral("font-size: %1px; padding: 0 14px; %2").arg(kToolbarFontPx).arg(extraStyle));
		bar->addWidget(button);
		return button;
	};

	emphasisButton =
		addButton("Emphasis", Qt::Key_1, true,
			  QStringLiteral("color: %1; font-weight: bold;").arg(QLatin1String(kEmphasisButtonColor)));
	stretchButton = addButton("Stretch", Qt::Key_2, true);
	auto *pauseButton = addButton("Pause", Qt::Key_3, false);
	bar->addSpacing(12);
	hookButton = addButton("Hook", Qt::Key_4, true, QStringLiteral("font-weight: bold;"));
	toneButton = addButton("Tone", Qt::Key_5, true, QStringLiteral("font-style: italic;"));
	sceneButton = addButton("Scene", Qt::Key_6, true);
	bar->addSpacing(12);
	auto *clearButton = addButton("Clear", Qt::Key_0, false);
	bar->addStretch();

	using ScriptMarkup::LineKind;
	using ScriptMarkup::Mark;
	connect(emphasisButton, &QPushButton::clicked, this, [this]() { ToggleMark(Mark::Emphasis); });
	connect(stretchButton, &QPushButton::clicked, this, [this]() { ToggleMark(Mark::Stretch); });
	connect(pauseButton, &QPushButton::clicked, this, &Teleprompter::InsertPause);
	connect(hookButton, &QPushButton::clicked, this, [this]() { ToggleLineKind(LineKind::Hook); });
	connect(toneButton, &QPushButton::clicked, this, [this]() { ToggleLineKind(LineKind::Tone); });
	connect(sceneButton, &QPushButton::clicked, this, [this]() { ToggleLineKind(LineKind::Scene); });
	connect(clearButton, &QPushButton::clicked, this, &Teleprompter::ClearMarks);

	formatBar->hide();
}

/* the format the buttons should show: the first selected letter, or what would be typed next */
QTextCharFormat Teleprompter::FormatAtCursor() const
{
	QTextCursor cursor = textView->textCursor();
	if (!cursor.hasSelection()) {
		return textView->currentCharFormat();
	}
	/* a cursor reports the format of the letter before it */
	cursor.setPosition(cursor.selectionStart() + 1);
	return cursor.charFormat();
}

void Teleprompter::ToggleMark(ScriptMarkup::Mark mark)
{
	ScriptMarkup::WordMarks marks = ScriptMarkup::WordMarksOf(FormatAtCursor());
	bool &flag = mark == ScriptMarkup::Mark::Emphasis ? marks.emphasis : marks.stretch;
	flag = !flag;

	QTextCursor cursor = textView->textCursor();
	if (cursor.hasSelection()) {
		ScriptMarkup::SetMark(cursor, mark, flag, fontSize);
	} else {
		/* nothing selected: the mark applies to what is typed next */
		textView->setCurrentCharFormat(
			ScriptMarkup::CharFormat(ScriptMarkup::LineKindOf(cursor.block()), marks, fontSize));
	}
	UpdateFormatButtons();
}

void Teleprompter::ToggleLineKind(ScriptMarkup::LineKind kind)
{
	QTextCursor cursor = textView->textCursor();
	QTextBlock firstLine = textView->document()->findBlock(cursor.selectionStart());
	bool alreadySet = ScriptMarkup::LineKindOf(firstLine) == kind;
	ScriptMarkup::SetLineKind(cursor, alreadySet ? ScriptMarkup::LineKind::Normal : kind, fontSize);
	UpdateFormatButtons();
}

void Teleprompter::InsertPause()
{
	QTextCharFormat typingFormat = textView->currentCharFormat();
	QTextCursor cursor = textView->textCursor();
	ScriptMarkup::InsertPause(cursor, fontSize);
	textView->setTextCursor(cursor);
	/* what is typed after the pause sign is normal text again */
	textView->setCurrentCharFormat(typingFormat);
}

void Teleprompter::ClearMarks()
{
	ScriptMarkup::ClearMarks(textView->textCursor(), fontSize);
	UpdateFormatButtons();
}

void Teleprompter::UpdateFormatButtons()
{
	if (!editing) {
		return;
	}

	QTextCursor cursor = textView->textCursor();
	ScriptMarkup::LineKind kind = ScriptMarkup::LineKindOf(textView->document()->findBlock(cursor.selectionStart()));
	ScriptMarkup::WordMarks marks = ScriptMarkup::WordMarksOf(FormatAtCursor());

	/* typing right behind a pause sign would come out grey like the sign itself */
	if (marks.pause && !cursor.hasSelection()) {
		marks.pause = false;
		textView->setCurrentCharFormat(ScriptMarkup::CharFormat(kind, marks, fontSize));
	}

	emphasisButton->setChecked(marks.emphasis);
	stretchButton->setChecked(marks.stretch);
	hookButton->setChecked(kind == ScriptMarkup::LineKind::Hook);
	toneButton->setChecked(kind == ScriptMarkup::LineKind::Tone);
	sceneButton->setChecked(kind == ScriptMarkup::LineKind::Scene);
}

void Teleprompter::LoadScript(const QString &markup)
{
	ScriptMarkup::Load(textView->document(), markup, fontSize);
	textView->ClearReadCards();
	/* a fresh document forgets its margins */
	ApplyReadingStyle();
	/* loading leaves the cursor at the end and the view would follow it down */
	textView->moveCursor(QTextCursor::Start);
	JumpToStart();
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
	config_set_default_bool(config, kConfigSection, "FollowMyVoice", false);
	followVoiceBox->setChecked(config_get_bool(config, kConfigSection, "FollowMyVoice"));

	fontSize = std::clamp((int)config_get_int(config, kConfigSection, "FontSize"), kMinFontSize, kMaxFontSize);
	speedSlider->setValue((int)config_get_int(config, kConfigSection, "Speed"));

	QFile file(ScriptFilePath());
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		LoadScript(QString::fromUtf8(file.readAll()));
	}
}

void Teleprompter::SaveState()
{
	config_t *config = App()->GetUserConfig();
	config_set_int(config, kConfigSection, "FontSize", fontSize);
	config_set_int(config, kConfigSection, "Speed", speedSlider->value());
	config_set_bool(config, kConfigSection, "FollowMyVoice", followVoiceBox->isChecked());

	QString path = ScriptFilePath();
	if (path.isEmpty()) {
		return;
	}

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		blog(LOG_WARNING, "Teleprompter: could not save script to '%s'", QT_TO_UTF8(path));
		return;
	}
	file.write(ScriptMarkup::Save(textView->document()).toUtf8());
}

void Teleprompter::ApplyReadingStyle()
{
	QFont font = textView->font();
	font.setPointSize(fontSize);
	textView->setFont(font);
	textView->SetBaseFontSize(fontSize);
	textView->setReadOnly(!editing);
	textView->setPlaceholderText(editing ? QTStr("Teleprompter.Placeholder") : QString());
	/* The theme sets a font-size on every widget, so the size has to go in the stylesheet too */
	/* editing looks the same as reading, so every mark shows the moment it is set */
	textView->setStyleSheet(QString("QTextEdit#teleprompterText { background-color: #16171c; color: #f2f2f2;"
					" font-size: %1pt; }")
					.arg(fontSize));
	formatBar->setVisible(editing);
	editButton->setText(QTStr(editing ? "Teleprompter.Done" : "Teleprompter.Edit"));

	/* Leave room above and below so the first and last lines can reach the guide,
	 * and keep the text in a centered column so long lines stay easy to follow */
	int viewportWidth = textView->viewport()->width();
	int viewportHeight = textView->viewport()->height();
	double sideMargin = std::max(kMinSideMargin, (viewportWidth - kMaxColumnWidth) / 2);

	QTextFrameFormat frameFormat = textView->document()->rootFrame()->frameFormat();
	frameFormat.setTopMargin(editing ? kEditMargin : viewportHeight * kReadingGuideRatio);
	frameFormat.setBottomMargin(editing ? kEditMargin : viewportHeight * (1.0 - kReadingGuideRatio));
	frameFormat.setLeftMargin(sideMargin);
	frameFormat.setRightMargin(sideMargin);
	textView->document()->rootFrame()->setFrameFormat(frameFormat);

	PositionEmptyHint();
	UpdateEmptyState();
}

void Teleprompter::UpdateEmptyState()
{
	bool empty = textView->toPlainText().trimmed().isEmpty();
	emptyHint->setVisible(empty && !editing);
	playButton->setEnabled(!empty);
}

void Teleprompter::UpdatePlayButton()
{
	bool running = scrollTimer->isActive();
	playButton->setText(QTStr(running ? "Teleprompter.Pause" : "Teleprompter.Play"));
}

void Teleprompter::PositionEmptyHint()
{
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
	if (!editing) {
		/* read the script back the way it is saved, so what stays on screen is what is in the file */
		LoadScript(ScriptMarkup::Save(textView->document()));
	}
	ApplyReadingStyle();
	UpdateEmptyState();
	if (editing) {
		textView->setFocus();
		UpdateFormatButtons();
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
	LoadScript(QString::fromUtf8(file.readAll()));
	JumpToStart();
	SaveState();
}

void Teleprompter::SetToolbarAccessory(QWidget *widget)
{
	if (accessory) {
		toolbar->removeWidget(accessory);
	}
	accessory = widget;
	if (auto *button = qobject_cast<QPushButton *>(accessory)) {
		/* same size and text as the buttons around it */
		button->setFixedHeight(kToolbarButtonHeight);
		button->setStyleSheet(QStringLiteral("font-size: %1px; padding: 0 14px;").arg(kToolbarFontPx));
	}
	if (accessory) {
		/* right before Edit Script, after a Record button if there is one */
		toolbar->insertWidget(toolbar->indexOf(editButton), accessory);
		accessory->show();
	}
}

void Teleprompter::AddToolbarButton(QPushButton *button, ToolbarSide side)
{
	button->setFixedHeight(kToolbarButtonHeight);
	if (button->styleSheet().isEmpty()) {
		button->setStyleSheet(QStringLiteral("font-size: %1px; padding: 0 14px;").arg(kToolbarFontPx));
	}
	button->setFocusPolicy(Qt::NoFocus);
	if (side == ToolbarSide::Start) {
		toolbar->insertWidget(0, button);
		return;
	}
	toolbar->addWidget(button);
}

void Teleprompter::SetRecording(bool recording)
{
	/* while recording the text must not change under the narrator */
	editButton->setEnabled(!recording);
	openButton->setEnabled(!recording);
}

void Teleprompter::BiggerText()
{
	ChangeFontSize(kFontStep);
}

void Teleprompter::SmallerText()
{
	ChangeFontSize(-kFontStep);
}

void Teleprompter::SetVoiceActive(bool active)
{
	voiceActive = active;
}

void Teleprompter::ScrollStep()
{
	/* wait for the narrator: no voice, no scrolling */
	if (followVoiceBox->isChecked() && !voiceActive) {
		return;
	}
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
	ScriptMarkup::RestyleAll(textView->document(), fontSize);
	ApplyReadingStyle();
}

void Teleprompter::JumpToStart()
{
	textView->verticalScrollBar()->setValue(0);
	scrollRemainder = 0.0;
}
