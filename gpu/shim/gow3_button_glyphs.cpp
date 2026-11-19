// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: Switch button prompts (see gow3_button_glyphs.h).
#include "gow3_button_glyphs.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef __SWITCH__
#include "horizon/gow3_switch.h"
#endif

namespace Gow3ButtonGlyphs {
namespace {

// Nintendo extension font characters (the system font's private-use button set).
enum : FT_ULong {
    kA = 0xE0E0, kB = 0xE0E1, kX = 0xE0E2, kY = 0xE0E3,
    kL = 0xE0E4, kR = 0xE0E5, kZL = 0xE0E6, kZR = 0xE0E7,
    kDpadUp = 0xE0EB, kDpadDown = 0xE0EC, kDpadLeft = 0xE0ED, kDpadRight = 0xE0EE,
    kPlus = 0xE0EF, kMinus = 0xE0F0,
    kLStick = 0xE0C1, kRStick = 0xE0C2, kLStickPress = 0xE0C4,
};

struct Mapping {
    FT_ULong gaiji;  // character of font/gravity_gaiji.ttf
    FT_ULong button; // Nintendo extension font character
};

// The pad is read by position (the bottom face button is the game's ✕), so each prompt shows
// the Switch button in the same place. GOW3_BUTTON_LABELS=nintendo shows the A/B and X/Y labels
// swapped instead, for an input map that swaps them (✕ on A).
constexpr Mapping kPositional[] = {
    {'h', kB}, {'e', kA}, {'g', kY}, {'f', kX},           // ✕ ○ □ △
    {'i', kL}, {'j', kR}, {'A', kZL}, {'B', kZR},         // L1 R1 L2 R2
    {'k', kLStick}, {'l', kRStick}, {'C', kLStickPress},  // L stick, R stick, L3
    {'m', kMinus}, {'n', kPlus},                          // SHARE, OPTIONS
    {'o', kDpadUp}, {'p', kDpadDown}, {'q', kDpadLeft}, {'r', kDpadRight},
};

bool LabelsSwapped() {
    static const bool swapped = [] {
        const char* env = std::getenv("GOW3_BUTTON_LABELS");
        return env && !std::strcmp(env, "nintendo");
    }();
    return swapped;
}

// GOW3_BUTTON_LABELS=swap_ab (Switch 2, options.ini swap_ab=1): A is the cross and B the circle,
// as the console's input mapping then reads them (src/gr_sdl_horizon.h); X and Y stay.
bool AbSwapped() {
    static const bool swapped = [] {
        const char* env = std::getenv("GOW3_BUTTON_LABELS");
        return env && !std::strcmp(env, "swap_ab");
    }();
    return swapped;
}

FT_ULong MapCode(FT_ULong code) {
    for (const auto& m : kPositional) {
        if (m.gaiji != code) {
            continue;
        }
        if (LabelsSwapped()) {
            switch (m.button) {
            case kA: return kB;
            case kB: return kA;
            case kX: return kY;
            case kY: return kX;
            default: break;
            }
        }
        if (AbSwapped()) {
            if (m.button == kA) return kB;
            if (m.button == kB) return kA;
        }
        return m.button;
    }
    return 0;
}

// The extension font's bytes: the system shared font on the console; on other hosts a font
// file named by GOW3_BUTTON_FONT (development: the same font, never shipped).
struct FontData {
    const FT_Byte* data = nullptr;
    FT_Long size = 0;
};

FontData LoadFontData() {
    FontData out;
#ifdef __SWITCH__
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        PlFontData font{};
        if (R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_NintendoExt)) &&
            font.address && font.size) {
            out.data = static_cast<const FT_Byte*>(font.address);
            out.size = static_cast<FT_Long>(font.size);
        } else {
            std::fprintf(stderr, "Button glyphs: no Nintendo extension font from pl:u\n");
        }
        // pl:u stays open: the shared memory holding the font must stay mapped.
    } else {
        std::fprintf(stderr, "Button glyphs: plInitialize failed\n");
    }
#endif
    if (!out.data) {
        if (const char* path = std::getenv("GOW3_BUTTON_FONT"); path && *path) {
            if (FILE* f = std::fopen(path, "rb")) {
                static std::vector<FT_Byte> bytes;
                std::fseek(f, 0, SEEK_END);
                const long n = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (n > 0) {
                    bytes.resize(size_t(n));
                    if (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()) {
                        out.data = bytes.data();
                        out.size = FT_Long(n);
                    }
                }
                std::fclose(f);
            }
            if (!out.data) {
                std::fprintf(stderr, "Button glyphs: cannot read GOW3_BUTTON_FONT %s\n", path);
            }
        }
    }
    return out;
}

const FontData& Font() {
    static const FontData data = LoadFontData();
    return data;
}

// One extension face per FreeType library (a face is used with its own library only).
struct ExtFace {
    FT_Library lib;
    FT_Face face;
};
std::mutex g_lock;
std::vector<ExtFace> g_ext_faces;

FT_Face ExtFaceFor(FT_Library lib) {
    std::scoped_lock lk{g_lock};
    for (const auto& e : g_ext_faces) {
        if (e.lib == lib) {
            return e.face;
        }
    }
    FT_Face face = nullptr;
    const FontData& font = Font();
    if (font.data && FT_New_Memory_Face(lib, font.data, font.size, 0, &face) != 0) {
        face = nullptr;
    }
    if (face) {
        int found = 0;
        for (const auto& m : kPositional) {
            found += FT_Get_Char_Index(face, m.button) != 0;
        }
        std::printf("Button glyphs: Switch button font ready (%d of %zu prompt glyphs)\n", found,
                    std::size(kPositional));
    }
    g_ext_faces.push_back({lib, face}); // a failure is remembered too
    return face;
}

bool IsGaijiFace(FT_Face face) {
    // The game opens the font once per font library; remember the last few answers.
    thread_local FT_Face last_face = nullptr;
    thread_local bool last_answer = false;
    if (face == last_face) {
        return last_answer;
    }
    last_face = face;
    last_answer = face->family_name && !std::strcmp(face->family_name, "Gravity_Gaiji");
    return last_answer;
}

} // namespace

// Bring-up: each font face the game draws with, once, and the first prompt replaced, so the log
// shows whether prompts reach this (the gaiji font) or are drawn some other way.
void NoteFace(FT_Face face, FT_ULong code) {
    static std::mutex note_mutex;
    static std::vector<FT_Face> seen;
    std::scoped_lock lock{note_mutex};
    if (std::find(seen.begin(), seen.end(), face) != seen.end() || seen.size() > 64) {
        return;
    }
    seen.push_back(face);
    std::printf("Button glyphs: the game draws with font '%s' (%s), first character U+%04lX\n",
                face->family_name ? face->family_name : "?", face->style_name ? face->style_name : "?",
                (unsigned long)code);
}

FT_UInt GlyphIndex(FT_Face& face, FT_ULong code) {
    if (!face) {
        return 0;
    }
    NoteFace(face, code);
    if (code < 0x80 && IsGaijiFace(face)) {
        if (const FT_ULong button = MapCode(code)) {
            if (FT_Face ext = ExtFaceFor(face->glyph->library)) {
                if (const FT_UInt gi = FT_Get_Char_Index(ext, button)) {
                    // Bring-up: each prompt character's first three replacements, so the log
                    // shows whether a prompt on screen came through here (or is a picture).
                    static std::atomic<unsigned> told[128]{};
                    if (told[code & 127].fetch_add(1) < 3) {
                        std::printf("Button glyphs: prompt '%c' drawn as U+%04lX\n", char(code),
                                    (unsigned long)button);
                    }
                    face = ext;
                    return gi;
                }
            }
        }
    }
    return FT_Get_Char_Index(face, code);
}

} // namespace Gow3ButtonGlyphs
