/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (mgba)
#pragma once

namespace TicoConfig
{
    // Minimal paths required by the overlay and integration
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/gba/test.gba";
    
    // Core system paths
    constexpr const char* SYSTEM_PATH = "sdmc:/tico/system/mgba/";
    constexpr const char* SAVES_PATH = "sdmc:/tico/saves/mgba/";
    constexpr const char* STATES_PATH = "sdmc:/tico/states/mgba/";

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
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
