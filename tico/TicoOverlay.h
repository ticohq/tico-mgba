/// @file TicoOverlay.h
/// @brief Overlay UI for tico-integrated mgba
/// GBA: no disc select menu (cartridge-based)
#pragma once

#include "imgui.h"
#include <SDL.h>
#include <string>
#include <vector>
#include <memory>

class TicoCore;

enum class OverlayMenu
{
    None,
    QuickMenu,
    SaveStates,
    Settings
};

// ============================================================================
// Enums for mGBA specific configuration
// ============================================================================

/// @brief Represents the display mode strategy for the core
enum class MgbaDisplayMode
{
    Display, // Fit to display (respects aspect ratio)
    Integer  // Integer scaling
};

/// @brief Represents the display size or aspect ratio
enum class MgbaDisplaySize
{
    Stretch, // Only for Display mode: stretch to fill ignoring aspect
    _4_3,    // Only for Display mode: force 4:3
    _16_9,   // Only for Display mode: force 16:9
    Auto,    // Only for Integer mode: maximum integer scale
    _1x,     // Only for Integer mode: exactly 1x
    _2x      // Only for Integer mode: exactly 2x
};

// ============================================================================
// TicoOverlay Class
// ============================================================================

/// @brief Overlay UI for mgba with tico styling
class TicoOverlay
{
public:
    TicoOverlay();
    ~TicoOverlay();

    /// @brief Update overlay animation
    void Update(float deltaTime);

    /// @brief Render the overlay
    void Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                int frameWidth, int frameHeight, int fboWidth = 0, int fboHeight = 0);

    /// @brief Handle input
    /// @return true if input was consumed by overlay
    bool HandleInput(SDL_GameController *controller);

    /// @brief Show/hide overlay
    void Show();
    void Hide();
    bool IsVisible() const { return m_currentMenu != OverlayMenu::None; }

    /// @brief Set game title for title card
    void SetGameTitle(const std::string &title) { m_gameTitle = title; }

    /// @brief Set core reference for save states
    void SetCore(TicoCore *core) { m_core = core; }

    /// @brief Check if user wants to exit to system (close owned title)
    bool ShouldExitToSystem() const { return m_shouldExitToSystem; }
    void ClearExitToSystem() { m_shouldExitToSystem = false; }

    /// @brief Check if user wants to exit to tico (chainload)
    bool ShouldExit() const { return m_shouldExit; }
    void ClearExit() { m_shouldExit = false; }

    /// @brief Shader selection accessor
    int GetShaderSelection() const { return m_shaderSelection; }

    /// @brief Check if user wants to reset
    bool ShouldReset() const { return m_shouldReset; }
    void ClearReset() { m_shouldReset = false; }

private:
    void RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                    float aspectRatio, int width, int height,
                    int fboWidth, int fboHeight);
    void RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize);
    void RenderTitleCard(ImDrawList *dl, ImVec2 displaySize);
    void RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderStatusBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime);

    OverlayMenu m_currentMenu = OverlayMenu::None;
    std::string m_gameTitle;
    TicoCore *m_core = nullptr;

    float m_animTimer = 0.0f;

    int m_quickMenuSelection = 0;
    int m_saveStateSlot = 0;
    bool m_isSaveMode = true;
    int m_settingsSelection = 0;
    int m_shaderSelection = 0; // 0=None, 1=LCD, 2=Scale2x
    
    MgbaDisplayMode m_displayMode = MgbaDisplayMode::Display;
    MgbaDisplaySize m_displaySize = MgbaDisplaySize::_4_3;

    void LoadCoreSettings();
    void SaveCoreSettings();
    void ApplyScalingSettings(bool save = true);

    unsigned int m_triangleTexture = 0;
    int m_triangleWidth = 0;
    int m_triangleHeight = 0;

    bool m_upHeld = false;
    bool m_downHeld = false;
    bool m_leftHeld = false;
    bool m_rightHeld = false;
    bool m_confirmHeld = false;
    bool m_backHeld = false;
    bool m_toggleHeld = false;
    bool m_xHeld = false;
    uint32_t m_lastInputTime = 0;
    static constexpr uint32_t DEBOUNCE_MS = 200;

    bool m_shouldExitToSystem = false;
    bool m_shouldExit = false;
    bool m_shouldReset = false;

    uint32_t m_batteryLevel = 100;
    bool m_isCharging = false;
    float m_batteryTimer = 0.0f;
    float m_chargingStateProgress = 0.0f;
    unsigned int m_boltTexture = 0;
    int m_boltWidth = 0;
    int m_boltHeight = 0;

    bool m_isDarkMode = true;
    bool m_showNickname = false;
    std::string m_hourFormat = "24h";
    void LoadConfig();
    void LoadGeneralConfig();
    void LoadSVGIcon();

    unsigned int m_avatarTexture = 0;
    std::string m_nickname;
    void LoadAccountData();
    void RenderSocialArea(ImDrawList *dl, ImVec2 displaySize);
};
