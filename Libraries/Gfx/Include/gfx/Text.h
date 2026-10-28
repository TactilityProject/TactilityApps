// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Font.h"

namespace gfx {

/**
 * Width in pixels of the widest line in `text`, at the given integer scale. Newlines split lines;
 * the value includes the advance after the last glyph, so it is suitable for layout.
 */
int measureText(const char* text, const Font& font, uint8_t scale = 1, uint8_t tabSize = 4);

/** Height in pixels of `text` including line gaps, at the given integer scale. */
int measureTextHeight(const char* text, const Font& font, uint8_t scale = 1);

/** Height of one line, including the gap below it. */
inline int lineHeight(const Font& font, uint8_t scale = 1) { return font.lineHeight * (scale == 0 ? 1 : scale); }

} // namespace gfx
