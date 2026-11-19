// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the pause menu's L1 and R1 tabs (gcd/ctrl/tabscene.gcd in arc_0_ep_0_0.psarc, its first
// and sixth textures) are 128x64 RGBA8 pictures, 1D-thin tiled: 8x8 tiles in rows, each tile's
// pixels in Morton order (x0 y0 x1 y1 x2 y2). Each shows two white characters on a flat tab
// colour, "L1" or "R1"; the Switch's shoulder buttons are L and R, so the "1" is painted over
// with the tab colour and the letter moved to the middle of where both were. Nothing of the
// picture is stored here: it is changed where the game loaded it.
#include "gow3_button_art.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace Gow3ButtonArt {
namespace {

constexpr std::uint64_t kL1Tab = 0x072e7e7f9ea3c0d2ull;
constexpr std::uint64_t kR1Tab = 0x9a1f209954877296ull;
constexpr int kWidth = 128, kHeight = 64;

std::uint8_t* Pixel(std::uint8_t* bytes, int x, int y) {
    const int tile = (y >> 3) * (kWidth >> 3) + (x >> 3);
    const int in_tile = (x & 1) | (y & 1) << 1 | ((x >> 1) & 1) << 2 | ((y >> 1) & 1) << 3 |
                        ((x >> 2) & 1) << 4 | ((y >> 2) & 1) << 5;
    return bytes + (tile * 64 + in_tile) * 4;
}

// The characters are white, the tab around them about (0x1f, 0x1f, 0x22).
bool IsText(const std::uint8_t* p) {
    return p[3] > 200 && p[0] > 0x30;
}

bool TabToLetter(std::uint8_t* bytes) {
    bool column[kWidth] = {};
    int top = kHeight, bottom = -1;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (IsText(Pixel(bytes, x, y))) {
                column[x] = true;
                top = y < top ? y : top;
                bottom = y > bottom ? y : bottom;
            }
        }
    }
    // The two characters: runs of columns with text in them.
    int runs[3][2], count = 0;
    for (int x = 0; x < kWidth; ++x) {
        if (!column[x] || (x > 0 && column[x - 1])) {
            continue;
        }
        int end = x;
        while (end + 1 < kWidth && column[end + 1]) {
            ++end;
        }
        if (count == 3) {
            return false;
        }
        runs[count][0] = x;
        runs[count][1] = end;
        ++count;
    }
    if (count != 2 || top > bottom || runs[0][0] < 2) {
        return false;
    }
    const int left = runs[0][0], letter_end = runs[0][1], right = runs[1][1];
    // The tab colour, just left of the text.
    std::uint8_t tab[4];
    std::memcpy(tab, Pixel(bytes, left - 2, (top + bottom) / 2), 4);
    if (IsText(tab)) {
        return false;
    }
    const int letter_width = letter_end - left + 1, rows = bottom - top + 1;
    std::vector<std::uint8_t> letter(std::size_t(letter_width) * rows * 4);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < letter_width; ++x) {
            std::memcpy(&letter[(std::size_t(y) * letter_width + x) * 4], Pixel(bytes, left + x, top + y), 4);
        }
    }
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            std::memcpy(Pixel(bytes, x, y), tab, 3); // the colour only: the tab is opaque there
        }
    }
    const int shift = (right - letter_end) / 2;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < letter_width; ++x) {
            std::memcpy(Pixel(bytes, left + shift + x, top + y), &letter[(std::size_t(y) * letter_width + x) * 4], 4);
        }
    }
    return true;
}

} // namespace

bool RewriteImage(std::uint8_t* bytes, std::size_t size, std::uint32_t width, std::uint32_t height,
                  std::uint64_t hash) {
    if (!Candidate(width, height, size) || (hash != kL1Tab && hash != kR1Tab)) {
        return false;
    }
    const bool done = TabToLetter(bytes);
    std::printf("Button art: the pause menu's %s tab %s\n", hash == kL1Tab ? "L1" : "R1",
                done ? "now shows the Switch button" : "was not as expected (left as is)");
    return done;
}

} // namespace Gow3ButtonArt
