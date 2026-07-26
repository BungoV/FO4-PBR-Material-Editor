#pragma once

#include <QColor>
#include <QIcon>

// Font-independent disclosure triangles. The "▼"/"▶" text glyphs render at
// badly mismatched sizes in Segoe UI (the right-pointing triangle falls back
// to a tiny symbol), so collapsed headers looked broken next to expanded ones.
QIcon disclosureArrowIcon(bool expanded, int size, const QColor& color);

// Previous/next triangles for stepping through a list, from the same geometry
// as the disclosure ones so a panel using both reads as one set. Same reason as
// above for not using "◀"/"▶" in a label: the glyph pair is not weight-matched
// in the UI font, it cannot be tinted apart from the text beside it, and it
// will not dim with the button when the button is disabled — which these are,
// most of the time.
QIcon navigationArrowIcon(bool forward, int size, const QColor& color);
