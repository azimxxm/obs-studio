#pragma once

#include <QSet>
#include <QTextEdit>

#include <vector>

class QPainter;

/* The script text of the teleprompter. Draws a numbered card behind every
 * group of lines, so each sentence to read stands on its own. While reading,
 * a click on a card marks it as read: it fades and gets a green border. */
class ScriptView : public QTextEdit {
public:
	explicit ScriptView(QWidget *parent = nullptr);

	/* the text size in points, cards and marks scale with it */
	void SetBaseFontSize(int pointSize);
	/* every card is unread again, used when a new script is loaded */
	void ClearReadCards();

protected:
	void paintEvent(QPaintEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;

private:
	struct Card {
		int number;
		QRectF rect;
		bool hook;
	};

	int baseFontSize = 0;
	QSet<int> readCards;
	QPointF pressPosition;

	int LineHeight() const;
	/* the cards in viewport coordinates */
	std::vector<Card> Cards() const;
	int CardAt(const QPointF &point) const;
	void PaintCard(QPainter &painter, const Card &card, int lineHeight);
	void PaintReadCover(QPainter &painter, const Card &card, int lineHeight);
};
