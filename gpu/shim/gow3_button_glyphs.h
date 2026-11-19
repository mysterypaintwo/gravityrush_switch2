// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: Switch button prompts. The game draws its inline controller prompts with its own glyph
// font (font/gravity_gaiji.ttf: ✕ is 'h', L1 is 'i', OPTIONS is 'n' ...). Gow3GlyphIndex looks
// a character up like FT_Get_Char_Index, but for those characters of that font it switches
// `face` to the console's Nintendo extension font (the system shared font from pl:u) and returns
// the matching Switch button glyph, so every prompt shows the Switch buttons. Without that font
// (the Mac host, unless GOW3_BUTTON_FONT names a font file) the PlayStation glyphs stay.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

namespace Gow3ButtonGlyphs {

// FT_Get_Char_Index(face, code), with the button substitution (face may change).
FT_UInt GlyphIndex(FT_Face& face, FT_ULong code);

} // namespace Gow3ButtonGlyphs
