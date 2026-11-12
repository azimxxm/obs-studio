#include "ScriptView.hpp"

#include <algorithm>

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFrame>

#include "ScriptMarkup.hpp"

namespace {
const QColor kCardColor(0x1f, 0x21, 0x2e);
const QColor kCardBorderColor(0x33, 0x36, 0x52);
const QColor kHookBorderColor(0xff, 0xa2, 0x3f);
const QColor kReadBorderColor(0x34, 0xd3, 0x99);
const QColor kBadgeColor(0x8b, 0x5c, 0xf6);
const QColor kBadgeTextColor(0xff, 0xff, 0xff);
/* laid over a read card, so its text looks faded */
const QColor kReadCoverColor(0x1f, 0x21, 0x2e, 150);

/* all card sizes are a share of the line height, so they grow with the text */
constexpr double kCardPaddingXRatio = 0.4;
constexpr double kCardPaddingYRatio = 0.22;
constexpr double kCardRadiusRatio = 0.3;
constexpr double kBadgeRadiusRatio = 0.2;
constexpr double kMinBadgeRadius = 10.0;
constexpr double kMaxBadgeRadius = 18.0;
constexpr double kBadgeInset = 14.0;
constexpr double kBadgeTextRatio = 1.1;
constexpr double kMinEdgeGap = 6.0;
constexpr double kHookBorderWidth = 2.0;
constexpr double kReadBorderWidth = 1.5;
/* a press that moves further than this is a text selection, not a click on a card */
constexpr double kClickSlop = 4.0;
} // namespace

ScriptView::ScriptView(QWidget *parent) : QTextEdit(parent)
{
	viewport()->setMouseTracking(true);
}

void ScriptView::SetBaseFontSize(int pointSize)
{
	baseFontSize = pointSize;
	viewport()->update();
}

void ScriptView::ClearReadCards()
{
	readCards.clear();
	viewport()->update();
}

void ScriptView::paintEvent(QPaintEvent *event)
{
	const std::vector<Card> cards = Cards();
	int lineHeight = LineHeight();
	QRect dirtyArea = event->rect();

	{
		QPainter painter(viewport());
		painter.setRenderHint(QPainter::Antialiasing);
		for (const Card &card : cards) {
			/* the number badge sticks out above the card */
			if (card.rect.adjusted(0, -kMaxBadgeRadius, 0, 0).intersects(dirtyArea)) {
				PaintCard(painter, card, lineHeight);
			}
		}
	}

	QTextEdit::paintEvent(event);

	if (readCards.isEmpty()) {
		return;
	}
	QPainter painter(viewport());
	painter.setRenderHint(QPainter::Antialiasing);
	for (const Card &card : cards) {
		if (readCards.contains(card.number) && card.rect.intersects(dirtyArea)) {
			PaintReadCover(painter, card, lineHeight);
		}
	}
}

void ScriptView::keyPressEvent(QKeyEvent *event)
{
	QTextEdit::keyPressEvent(event);

	/* a new line after a hook, tone note or scene title starts as normal text */
	bool newLine = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
	if (!newLine || isReadOnly()) {
		return;
	}
	QTextCursor cursor = textCursor();
	bool special = ScriptMarkup::LineKindOf(cursor.block()) != ScriptMarkup::LineKind::Normal;
	if (special && cursor.block().text().isEmpty()) {
		ScriptMarkup::SetLineKind(cursor, ScriptMarkup::LineKind::Normal, baseFontSize);
		setCurrentCharFormat(ScriptMarkup::CharFormat(ScriptMarkup::LineKind::Normal, {}, baseFontSize));
	}
}

void ScriptView::mousePressEvent(QMouseEvent *event)
{
	pressPosition = event->position();
	QTextEdit::mousePressEvent(event);
}

void ScriptView::mouseReleaseEvent(QMouseEvent *event)
{
	QTextEdit::mouseReleaseEvent(event);

	/* cards are only ticked off while reading, in edit mode a click places the cursor */
	bool click = (event->position() - pressPosition).manhattanLength() <= kClickSlop;
	if (!isReadOnly() || event->button() != Qt::LeftButton || !click) {
		return;
	}
	int number = CardAt(event->position());
	if (number == 0) {
		return;
	}
	if (!readCards.remove(number)) {
		readCards.insert(number);
	}
	viewport()->update();
}

void ScriptView::mouseMoveEvent(QMouseEvent *event)
{
	QTextEdit::mouseMoveEvent(event);

	bool overCard = isReadOnly() && event->buttons() == Qt::NoButton && CardAt(event->position()) != 0;
	viewport()->setCursor(overCard ? Qt::PointingHandCursor : (isReadOnly() ? Qt::ArrowCursor : Qt::IBeamCursor));
}

int ScriptView::LineHeight() const
{
	QFont baseFont = font();
	baseFont.setPointSize(baseFontSize);
	return QFontMetrics(baseFont).height();
}

std::vector<ScriptView::Card> ScriptView::Cards() const
{
	int lineHeight = LineHeight();
	QTextFrameFormat frame = document()->rootFrame()->frameFormat();
	double paddingX = std::min(lineHeight * kCardPaddingXRatio, frame.leftMargin() - kMinEdgeGap);
	double paddingY = lineHeight * kCardPaddingYRatio;
	double left = frame.leftMargin() - paddingX;
	double width = viewport()->width() - frame.rightMargin() + paddingX - left;
	int scrolled = verticalScrollBar()->value();
	QAbstractTextDocumentLayout *layout = document()->documentLayout();

	std::vector<Card> cards;
	QTextBlock block = document()->begin();
	while (block.isValid()) {
		if (!ScriptMarkup::IsCardLine(block)) {
			block = block.next();
			continue;
		}

		/* lines that follow each other without an empty line share one card */
		QRectF lines = layout->blockBoundingRect(block);
		bool hook = false;
		for (; block.isValid() && ScriptMarkup::IsCardLine(block); block = block.next()) {
			lines = lines.united(layout->blockBoundingRect(block));
			hook = hook || ScriptMarkup::LineKindOf(block) == ScriptMarkup::LineKind::Hook;
		}

		QRectF rect(left, lines.top() - scrolled - paddingY, width, lines.height() + paddingY * 2);
		cards.push_back({int(cards.size()) + 1, rect, hook});
	}
	return cards;
}

int ScriptView::CardAt(const QPointF &point) const
{
	for (const Card &card : Cards()) {
		if (card.rect.contains(point)) {
			return card.number;
		}
	}
	return 0;
}

void ScriptView::PaintCard(QPainter &painter, const Card &card, int lineHeight)
{
	double radius = lineHeight * kCardRadiusRatio;
	painter.setPen(card.hook ? QPen(kHookBorderColor, kHookBorderWidth) : QPen(kCardBorderColor));
	painter.setBrush(kCardColor);
	painter.drawRoundedRect(card.rect, radius, radius);

	double badgeRadius = std::clamp(lineHeight * kBadgeRadiusRatio, kMinBadgeRadius, kMaxBadgeRadius);
	QRectF badge(card.rect.left() + kBadgeInset, card.rect.top() - badgeRadius, badgeRadius * 2, badgeRadius * 2);
	painter.setPen(Qt::NoPen);
	painter.setBrush(card.hook ? kHookBorderColor : kBadgeColor);
	painter.drawEllipse(badge);

	QFont badgeFont = font();
	badgeFont.setPixelSize(int(badgeRadius * kBadgeTextRatio));
	badgeFont.setBold(true);
	painter.setFont(badgeFont);
	painter.setPen(kBadgeTextColor);
	painter.drawText(badge, Qt::AlignCenter, QString::number(card.number));
}

void ScriptView::PaintReadCover(QPainter &painter, const Card &card, int lineHeight)
{
	double radius = lineHeight * kCardRadiusRatio;
	painter.setPen(QPen(kReadBorderColor, kReadBorderWidth));
	painter.setBrush(kReadCoverColor);
	painter.drawRoundedRect(card.rect, radius, radius);

	/* the badge fades with the card */
	double badgeRadius = std::clamp(lineHeight * kBadgeRadiusRatio, kMinBadgeRadius, kMaxBadgeRadius);
	QRectF badge(card.rect.left() + kBadgeInset, card.rect.top() - badgeRadius, badgeRadius * 2, badgeRadius * 2);
	painter.setPen(Qt::NoPen);
	painter.setBrush(kReadCoverColor);
	painter.drawEllipse(badge);
}
