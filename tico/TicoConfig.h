/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (mgba)
#pragma once

#include <string>

namespace TicoConfig {
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/gba/rom.gba";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";

    // Current console slug (gba, from argv[1])
    inline std::string CURRENT_SLUG = "gba";

    /// @brief Set the console being booted (gba)
    inline void SetSlug(const std::string& slug) {
        if (!slug.empty())
            CURRENT_SLUG = slug;
    }

    /// Content directories, with a trailing slash. Tico's per-module Paths tab
    /// stores custom roots as tico_{system,saves,states}_path in mgba.jsonc;
    /// empty or missing keys fall back to sdmc:/tico/<kind>/. Like tico's own
    /// {saves}/{states}/{system}, the console slug is appended to the root.
    std::string SystemPath();
    std::string SavesPath();
    std::string StatesPath();

    /// Create a directory and any missing parents.
    void MakeDirs(const std::string& path);

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    /// @brief Use callback/ring-buffer path (supports resampling)
    constexpr bool USE_SDLQUEUEAUDIO = false;
}

/// @brief UI action identifiers for the helpers bar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
