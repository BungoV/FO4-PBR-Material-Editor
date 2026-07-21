#pragma once

#include <QColor>
#include <QIcon>

// Font-independent disclosure triangles. The "▼"/"▶" text glyphs render at
// badly mismatched sizes in Segoe UI (the right-pointing triangle falls back
// to a tiny symbol), so collapsed headers looked broken next to expanded ones.
QIcon disclosureArrowIcon(bool expanded, int size, const QColor& color);
