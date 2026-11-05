#pragma once

#include <QColor>
#include <QString>

class QWidget;

/* A floating Liquid Glass card (macOS 26+) over a Qt widget, used for the
 * countdown and the "mistake marked" note above the teleprompter text.
 * On older systems Available() is false and callers draw their own overlay. */
namespace MacGlass {

bool Available();

/* shows (or updates) the card centred on anchor */
void ShowCard(QWidget *anchor, const QString &title, const QString &subtitle, const QColor &titleColor,
	      int titlePointSize);
void HideCard();

} // namespace MacGlass
