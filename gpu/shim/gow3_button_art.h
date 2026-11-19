// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: Switch button prompts that the game draws as pictures, not with its prompt font
// (gow3_button_glyphs.h covers the font). The texture cache calls RewriteImage before it uploads
// a game texture; a known PS4 button picture is changed in guest memory to the Switch button.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Gow3ButtonArt {

// `bytes` is the texture as the game loaded it (`size` bytes, `width` x `height`, `hash` its
// XXH3_64bits). Returns true if it was a PS4 button picture and has been rewritten in place.
bool RewriteImage(std::uint8_t* bytes, std::size_t size, std::uint32_t width, std::uint32_t height,
                  std::uint64_t hash);

// Whether a texture of this size can be one (to skip hashing every other texture).
inline bool Candidate(std::uint32_t width, std::uint32_t height, std::size_t size) {
    return width == 128 && height == 64 && size == 128 * 64 * 4;
}

} // namespace Gow3ButtonArt
