#include "MacGlass.hpp"

#include <QWidget>

#import <AppKit/AppKit.h>

/* glass that never takes clicks, so the teleprompter under it still works */
API_AVAILABLE(macos(26.0))
@interface OBSPassThroughGlassView : NSGlassEffectView
@end

@implementation OBSPassThroughGlassView
- (NSView *)hitTest:(NSPoint)point
{
	(void) point;
	return nil;
}
@end

namespace MacGlass {

namespace {
constexpr CGFloat kCornerRadius = 28.0;
constexpr CGFloat kPaddingX = 48.0;
constexpr CGFloat kPaddingY = 26.0;
constexpr CGFloat kLineGap = 6.0;
constexpr CGFloat kSubtitlePointSize = 16.0;

NSView *card = nil;
NSTextField *titleLabel = nil;
NSTextField *subtitleLabel = nil;

NSTextField *MakeLabel()
{
	NSTextField *label = [NSTextField labelWithString:@""];
	label.alignment = NSTextAlignmentCenter;
	label.translatesAutoresizingMaskIntoConstraints = YES;
	return label;
}

NSColor *ToNSColor(const QColor &color)
{
	return [NSColor colorWithSRGBRed:color.redF() green:color.greenF() blue:color.blueF() alpha:color.alphaF()];
}
} // namespace

bool Available()
{
	if (@available(macOS 26.0, *)) {
		return true;
	}
	return false;
}

void ShowCard(QWidget *anchor, const QString &title, const QString &subtitle, const QColor &titleColor,
	      int titlePointSize)
{
	if (@available(macOS 26.0, *)) {
		if (!anchor || !anchor->window()) {
			return;
		}
		/* the top-level window's view; Qt's view is flipped, so y grows downwards */
		NSView *host = (__bridge NSView *)reinterpret_cast<void *>(anchor->window()->winId());
		if (!host) {
			return;
		}

		if (!card) {
			OBSPassThroughGlassView *glass = [[OBSPassThroughGlassView alloc] initWithFrame:NSZeroRect];
			glass.cornerRadius = kCornerRadius;
			glass.style = NSGlassEffectViewStyleRegular;
			NSView *content = [[NSView alloc] initWithFrame:NSZeroRect];
			titleLabel = MakeLabel();
			subtitleLabel = MakeLabel();
			[content addSubview:titleLabel];
			[content addSubview:subtitleLabel];
			glass.contentView = content;
			card = glass;
		}
		if (card.superview != host) {
			[card removeFromSuperview];
			[host addSubview:card positioned:NSWindowAbove relativeTo:nil];
		}

		titleLabel.stringValue = title.toNSString();
		titleLabel.font = [NSFont systemFontOfSize:titlePointSize weight:NSFontWeightBold];
		titleLabel.textColor = ToNSColor(titleColor);
		subtitleLabel.stringValue = subtitle.toNSString();
		subtitleLabel.font = [NSFont systemFontOfSize:kSubtitlePointSize weight:NSFontWeightMedium];
		subtitleLabel.textColor = [NSColor colorWithWhite:0.92 alpha:1.0];
		subtitleLabel.hidden = subtitle.isEmpty();

		[titleLabel sizeToFit];
		[subtitleLabel sizeToFit];
		NSSize titleSize = titleLabel.frame.size;
		NSSize subtitleSize = subtitle.isEmpty() ? NSZeroSize : subtitleLabel.frame.size;
		CGFloat width = MAX(titleSize.width, subtitleSize.width) + kPaddingX * 2;
		CGFloat height = titleSize.height + (subtitle.isEmpty() ? 0 : subtitleSize.height + kLineGap) +
				 kPaddingY * 2;

		/* centre on the anchor widget, in the window's coordinates */
		QPoint origin = anchor->mapTo(anchor->window(), QPoint(0, 0));
		CGFloat x = origin.x() + (anchor->width() - width) / 2.0;
		CGFloat y = origin.y() + (anchor->height() - height) / 2.0;
		if (!host.isFlipped) {
			y = host.bounds.size.height - y - height;
		}
		card.frame = NSMakeRect(x, y, width, height);

		NSView *content = ((NSGlassEffectView *)card).contentView;
		content.frame = card.bounds;
		/* labels from the top of the card (content view is not flipped) */
		CGFloat top = height - kPaddingY - titleSize.height;
		titleLabel.frame = NSMakeRect((width - titleSize.width) / 2.0, top, titleSize.width, titleSize.height);
		if (!subtitle.isEmpty()) {
			subtitleLabel.frame = NSMakeRect((width - subtitleSize.width) / 2.0,
							 top - kLineGap - subtitleSize.height, subtitleSize.width,
							 subtitleSize.height);
		}
		card.hidden = NO;
	}
}

void HideCard()
{
	if (card) {
		card.hidden = YES;
	}
}

} // namespace MacGlass
