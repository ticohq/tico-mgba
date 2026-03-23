/// @file TicoCore.cpp
/// @brief Simplified libretro frontend for mgba with tico overlay
/// GBA: no disk control, ROM loaded into memory (need_fullpath=false),
/// HW render via GLSM, save data uses native mgba formats with .srm fallback

#include "TicoCore.h"
#include "TicoShaders.h"
#include "TicoConfig.h"
#include <algorithm>
#include <json.hpp>
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>
#include <fstream>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include "TicoLogger.h"

// RetroAchievements
#include "rc_client.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include "TicoLogger.h"
#include "deps/stb/stb_image.h"



#ifdef __SWITCH__
#include <glad/glad.h>
#include <switch.h>

/// @brief Switch vibration handles and state
static HidVibrationDeviceHandle s_vibrationHandles[5][2] = {};
static HidVibrationValue s_currentVibration[5][2] = {};
static bool s_vibrationInitialized = false;

#else
#include <glad/glad.h>
#endif



#define tico_debug_log(...) LOG_CORE(__VA_ARGS__)

void TicoCore::LoadSaveData()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    std::string savePathSav = std::string(TicoConfig::SAVES_PATH) + filename + ".sav";
    std::string savePathSrm = std::string(TicoConfig::SAVES_PATH) + filename + ".srm";

    std::ifstream fileSav(savePathSav, std::ios::binary);
    if (fileSav)
    {
        fileSav.read((char *)data, size);
        tico_debug_log("Loaded SRAM from %s", savePathSav.c_str());
        return;
    }

    std::ifstream fileSrm(savePathSrm, std::ios::binary);
    if (fileSrm)
    {
        fileSrm.read((char *)data, size);
        tico_debug_log("Loaded SRAM from %s (legacy fallback)", savePathSrm.c_str());
    }
}

void TicoCore::SaveSaveData()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    struct stat st = {0};
    if (stat(TicoConfig::SAVES_PATH, &st) == -1)
        mkdir(TicoConfig::SAVES_PATH, 0777);

    std::string savePath = std::string(TicoConfig::SAVES_PATH) + filename + ".sav";

    std::ofstream file(savePath, std::ios::binary);
    if (file)
    {
        file.write((const char *)data, size);
        tico_debug_log("Saved SRAM to %s", savePath.c_str());
    }
}

#include "libretro.h"

#ifndef RETRO_ENVIRONMENT_RETROARCH_START_BLOCK
#define RETRO_ENVIRONMENT_RETROARCH_START_BLOCK 0x800000
#endif

#ifndef RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND
#define RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND (2 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

#ifndef RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB
#define RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB (3 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

#ifndef RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE
#define RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE (4 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

// Forward declarations for mgba core functions (C linkage)
extern "C"
{
    void retro_init(void);
    void retro_deinit(void);
    void retro_set_environment(retro_environment_t);
    void retro_set_video_refresh(retro_video_refresh_t);
    void retro_set_audio_sample(retro_audio_sample_t);
    void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
    void retro_set_input_poll(retro_input_poll_t);
    void retro_set_input_state(retro_input_state_t);
    void retro_get_system_info(struct retro_system_info *info);
    void retro_get_system_av_info(struct retro_system_av_info *info);
    void retro_set_controller_port_device(unsigned port, unsigned device);
    void retro_reset(void);
    void retro_run(void);
    bool retro_load_game(const struct retro_game_info *game);
    void retro_unload_game(void);
    size_t retro_serialize_size(void);
    bool retro_serialize(void *data, size_t size);
    bool retro_unserialize(const void *data, size_t size);
    void *retro_get_memory_data(unsigned id);
    size_t retro_get_memory_size(unsigned id);
}

// Static instance for callbacks
static TicoCore *s_instance = nullptr;

// HW render callback storage
static retro_hw_render_callback s_hwRenderCallback = {};

//==============================================================================
// RetroAchievements Callbacks
//==============================================================================
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    if (!s_instance) return 0;
    
    uint32_t real_address = address;
    if (s_instance->m_consoleId == 5) { // Game Boy Advance Translation
        if (address < 0x08000) {
            real_address = 0x03000000 + address;
        } else if (address < 0x48000) {
            real_address = 0x02000000 + (address - 0x08000);
        } else if (address < 0x58000) {
            real_address = 0x0E000000 + (address - 0x48000);
        }
    }
    
    // First check memory maps provided by RETRO_ENVIRONMENT_GET_MEMORY_MAPS
    if (!s_instance->m_memoryMaps.empty()) {
        for (const auto& map : s_instance->m_memoryMaps) {
            if (real_address >= map.start && real_address + num_bytes <= map.start + map.length) {
                memcpy(buffer, map.ptr + (real_address - map.start), num_bytes);
                return num_bytes;
            }
        }
        return 0;
    }
    
    // Fallback for mGBA
    // Gameboy WRAM
    uint8_t* wram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t wram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (wram && real_address + num_bytes <= wram_size) {
        memcpy(buffer, wram + real_address, num_bytes);
        return num_bytes;
    }
    
    // Gameboy SRAM
    uint8_t* sram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    // Offset depends on rcheevos mapping, often SRAM is placed after WRAM or logically isolated
    if (sram && real_address >= 0xA000 && (real_address - 0xA000) + num_bytes <= sram_size) {
        memcpy(buffer, sram + (real_address - 0xA000), num_bytes);
        return num_bytes;
    }
    
    return 0;
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Persistent RA worker thread entry point
void TicoCore::RAWorkerEntry(void* arg) {
    TicoCore* self = (TicoCore*)arg;
    
    while (true) {
        RAJob job;
        {
            std::unique_lock<std::mutex> lock(self->m_raJobMutex);
            self->m_raJobCond.wait(lock, [self]() {
                return !self->m_raJobQueue.empty() || !self->m_raWorkerRunning;
            });
            
            if (!self->m_raWorkerRunning && self->m_raJobQueue.empty())
                break;
            
            job = std::move(self->m_raJobQueue.front());
            self->m_raJobQueue.pop_front();
        }
        
        // Handle badge download jobs specially
        if (job.url == "__badge__") {
            self->DownloadAndCacheBadge(job.post_data);
            continue;
        }
        
        // Do the HTTP request on this worker thread
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string errorMsg;
        std::string requestUrl = job.url;
        
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            if (!job.post_data.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.post_data.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            } else {
                errorMsg = curl_easy_strerror(res);
                http_code = 500;
            }
            curl_easy_cleanup(curl);
        }
        
        // Queue result callback for main thread
        {
            std::lock_guard<std::mutex> lock(self->m_raCallbackMutex);
            self->m_raPendingCallbacks.push_back(
                [job, http_code, readBuffer, errorMsg, requestUrl]() {
                    tico_debug_log("RA: HTTP Request -> %s", requestUrl.c_str());
                    if (!errorMsg.empty()) {
                        tico_debug_log("RA HTTP Error: %s", errorMsg.c_str());
                    }
                    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
                    
                    rc_api_server_response_t response;
                    memset(&response, 0, sizeof(response));
                    response.body = readBuffer.c_str();
                    response.body_length = readBuffer.size();
                    response.http_status_code = http_code;
                    
                    rc_client_server_callback_t cb = (rc_client_server_callback_t)job.callback;
                    if (cb) {
                        cb(&response, job.callback_data);
                    }
                }
            );
        }
    }
}

void TicoCore::StartRAWorker() {
#ifdef __SWITCH__
    m_raWorkerRunning = true;
    memset(&m_raThread, 0, sizeof(m_raThread));
    // Pin to core 0 (free for emulators), priority 0x2C (normal), stack 256KB
    Result rc = threadCreate(&m_raThread, RAWorkerEntry, this, NULL, 0x40000, 0x2C, 0);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&m_raThread);
        if (R_SUCCEEDED(rc)) {
            m_raThreadCreated = true;
            tico_debug_log("RA: Worker thread started (core 0, 256KB stack)");
        } else {
            tico_debug_log("RA: threadStart failed: 0x%x", rc);
            threadClose(&m_raThread);
            m_raWorkerRunning = false;
        }
    } else {
        tico_debug_log("RA: threadCreate failed: 0x%x", rc);
        m_raWorkerRunning = false;
    }
#else
    // Stub
#endif
}

void TicoCore::StopRAWorker() {
#ifdef __SWITCH__
    if (!m_raThreadCreated) return;
    
    {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        m_raWorkerRunning = false;
    }
    m_raJobCond.notify_one();
    
    threadWaitForExit(&m_raThread);
    threadClose(&m_raThread);
    m_raThreadCreated = false;
    tico_debug_log("RA: Worker thread stopped");
#endif
}

static void RC_CCONV RAServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
    if (!s_instance) return;
    
    TicoCore::RAJob job;
    job.url = request->url;
    if (request->post_data) job.post_data = request->post_data;
    job.callback = (void*)callback;
    job.callback_data = callback_data;
    
#ifdef __SWITCH__
    if (s_instance->m_raWorkerRunning) {
        std::lock_guard<std::mutex> lock(s_instance->m_raJobMutex);
        s_instance->m_raJobQueue.push_back(std::move(job));
        s_instance->m_raJobCond.notify_one();
    } else {
        // Fallback: synchronous if worker not running
        tico_debug_log("RA: HTTP Request (sync) -> %s", request->url);
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, request->url);
            if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
            curl_easy_cleanup(curl);
        }
        tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.body = readBuffer.c_str();
        response.body_length = readBuffer.size();
        response.http_status_code = http_code;
        if (callback) callback(&response, callback_data);
    }
#else
    // On non-Switch: just do it synchronously
    tico_debug_log("RA: HTTP Request -> %s", request->url);
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    long http_code = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, request->url);
        if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
        curl_easy_cleanup(curl);
    }
    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
    rc_api_server_response_t response;
    memset(&response, 0, sizeof(response));
    response.body = readBuffer.c_str();
    response.body_length = readBuffer.size();
    response.http_status_code = http_code;
    if (callback) callback(&response, callback_data);
#endif
}

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));

    m_systemDir = TicoConfig::SYSTEM_PATH;
    m_saveDir = TicoConfig::SAVES_PATH;
}

TicoCore::~TicoCore()
{
    tico_debug_log("~TicoCore: destroying (gameLoaded=%d, initialized=%d, hwRender=%d)",
             m_gameLoaded, m_initialized, m_hwRender);

    UnloadGame();
    DestroyShaderPipeline();

    if (m_initialized)
    {
        glFinish(); // drain any pending GPU commands before CoreShutdown
        tico_debug_log("Calling retro_deinit...");
        retro_deinit();
        tico_debug_log("retro_deinit done");
        m_initialized = false;
    }

    StopRAWorker();

    if (m_trophySound) {
        Mix_FreeChunk(m_trophySound);
        m_trophySound = nullptr;
    }

    if (m_rcClient) {
        rc_client_destroy(m_rcClient);
        m_rcClient = nullptr;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }

    tico_debug_log("~TicoCore: done");
}

//==============================================================================
// Initialization
//==============================================================================

bool TicoCore::Init()
{
    if (m_initialized)
        return true;

    s_instance = this;

    tico_debug_log("=== TicoCore::Init() ===");

#ifdef __SWITCH__
    if (!s_vibrationInitialized)
    {
        memset(s_currentVibration, 0, sizeof(s_currentVibration));
        for(int i = 0; i < 5; i++) {
            for(int j = 0; j < 2; j++) {
                s_currentVibration[i][j].freq_low = 160.0f;
                s_currentVibration[i][j].freq_high = 320.0f;
            }
        }
        
        hidInitializeVibrationDevices(s_vibrationHandles[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidInitializeVibrationDevices(s_vibrationHandles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[2], 2, HidNpadIdType_No2, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[3], 2, HidNpadIdType_No3, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[4], 2, HidNpadIdType_No4, HidNpadStyleTag_NpadJoyDual);
        
        s_vibrationInitialized = true;
        tico_debug_log("Vibration devices initialized for P1-P4");
    }
#endif

    // Ensure system/mgba directories exist
    struct stat st = {0};
    if (stat(m_systemDir.c_str(), &st) == -1) {
        mkdir(m_systemDir.c_str(), 0777);
    }
    std::string mgbaDir = m_systemDir + "mgba/";
    if (stat(mgbaDir.c_str(), &st) == -1) {
        mkdir(mgbaDir.c_str(), 0777);
    }
    tico_debug_log("System dir: %s", m_systemDir.c_str());

    // Load configuration to ensure variables are ready for init
    LoadConfig();
    tico_debug_log("Config loaded, %lu options", m_configOptions.size());

    bool soundEnabled = false;
#ifdef __SWITCH__
    std::string audioConfigPath = "sdmc:/tico/config/audio.jsonc";
#else
    std::string audioConfigPath = "tico/config/audio.jsonc";
#endif
    std::ifstream audioIn(audioConfigPath);
    if (audioIn.is_open()) {
        nlohmann::json j = nlohmann::json::parse(audioIn, nullptr, false, true); // allow_exceptions = false, allow_comments = true
        if (!j.is_discarded() && j.contains("sound_enabled")) {
            if (j["sound_enabled"].is_boolean()) {
                soundEnabled = j["sound_enabled"].get<bool>();
            }
        }
        audioIn.close();
    }
    if (soundEnabled) {
#ifdef __SWITCH__
        m_trophySound = Mix_LoadWAV("romfs:/assets/trophy.mp3");
#else
        m_trophySound = Mix_LoadWAV("tico/assets/trophy.mp3");
#endif
        if (m_trophySound) tico_debug_log("RA: Loaded trophy.mp3 successfully.");
        else tico_debug_log("RA: Failed to load trophy.mp3 -> %s", Mix_GetError());
    }

    // Environment callback must be set before retro_init
    tico_debug_log("Calling retro_set_environment...");
    retro_set_environment(EnvironmentCallback);
    tico_debug_log("retro_set_environment done");

    // Initialize core
    tico_debug_log("Calling retro_init...");
    retro_init();
    tico_debug_log("retro_init done");

    // Set all callbacks
    retro_set_video_refresh(VideoRefreshCallback);
    retro_set_audio_sample(AudioSampleCallback);
    retro_set_audio_sample_batch(AudioSampleBatchCallback);
    retro_set_input_poll(InputPollCallback);
    retro_set_input_state(InputStateCallback);

    // Get core info
    struct retro_system_info sysInfo = {};
    retro_get_system_info(&sysInfo);

    tico_debug_log("Initialized: %s %s",
             sysInfo.library_name ? sysInfo.library_name : "Unknown",
             sysInfo.library_version ? sysInfo.library_version : "");

    // ------------------------------------------------------------------
    // Setup RetroAchievements Client
    // ------------------------------------------------------------------
    LoadRAConfig();
    
    m_rcClient = rc_client_create(RAReadMemory, RAServerCall);
    if (m_rcClient) {
        rc_client_set_event_handler(m_rcClient, [](const rc_client_event_t* event, rc_client_t* client) {
            if (!s_instance) return;
            switch (event->type) {
                case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                    if (event->achievement) {
                        std::string title = event->achievement->title;
                        std::string desc = event->achievement->description;
                        std::string badge = event->achievement->badge_name;
                        s_instance->PushRANotification(title, desc, badge);
                        if (s_instance->m_trophySound) {
                            Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                        }
                        tico_debug_log("RA: Achievement triggered: %s (badge: %s)", title.c_str(), badge.c_str());
                    }
                    break;
                case RC_CLIENT_EVENT_GAME_COMPLETED:
                    s_instance->PushRANotification("Game Mastered!", "All achievements unlocked!", "ra_icon");
                    if (s_instance->m_trophySound) {
                        Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                    }
                    break;
                case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                    if (event->leaderboard) {
                        s_instance->PushRANotification("Leaderboard", event->leaderboard->title, "ra_icon");
                    }
                    break;
                case RC_CLIENT_EVENT_SERVER_ERROR:
                    if (event->server_error) {
                        tico_debug_log("RA: Server error: %s", event->server_error->error_message);
                    }
                    break;
                default:
                    break;
            }
        });
        rc_client_set_hardcore_enabled(m_rcClient, m_raHardcore);
        
        StartRAWorker();
        
        if (!m_raUsername.empty() && !m_raToken.empty()) {
            tico_debug_log("RA: Existent token found. Auto login as %s...", m_raUsername.c_str());
            rc_client_begin_login_with_token(m_rcClient, m_raUsername.c_str(), m_raToken.c_str(),
                [](int res, const char* err, rc_client_t* c, void* ud) {
                    TicoCore* self = (TicoCore*)ud;
                    if (res == RC_OK) {
                        tico_debug_log("RA login success with token!");
                        // Token valid, let's identify the game
                        if (self->m_gameLoaded && !self->m_gamePath.empty()) {
                            RAIdentifyGame(c, self);
                        }
                    } else if (res == RC_INVALID_CREDENTIALS && !self->m_raPassword.empty()) {
                        tico_debug_log("RA token invalid or expired. Trying password...");
                        RALoginWithPassword(c, self);
                    } else {
                        tico_debug_log("RA login failed -> %s", err ? err : "Unknown");
                        self->PushRANotification("Login Failed", "Check your credentials.", "ra_icon");
                    }
                }, this);
        } else if (!m_raUsername.empty() && !m_raPassword.empty()) {
            tico_debug_log("RA: Auto login using password...");
            RALoginWithPassword(m_rcClient, this);
        }
    }

    m_initialized = true;
    return true;
}

void TicoCore::SetHWRenderContext(SDL_Window *window, EGLContext mainCtx, EGLContext hwCtx)
{
    m_window = window;
    m_mainContext = mainCtx;
    m_hwContext = hwCtx;
    m_eglDisplay = eglGetCurrentDisplay();
    m_eglSurface = eglGetCurrentSurface(EGL_DRAW);
}

bool TicoCore::InitEGLDualContext()
{
    m_eglDisplay = eglGetCurrentDisplay();
    EGLContext currentCtx = eglGetCurrentContext();

    if (m_eglDisplay == EGL_NO_DISPLAY || currentCtx == EGL_NO_CONTEXT)
    {
        tico_debug_log("ERROR: Failed to get current EGL context");
        return false;
    }

    m_mainContext = currentCtx;
    m_eglSurface = eglGetCurrentSurface(EGL_DRAW);
    m_hwContext = m_mainContext; // Single context mode

    int fboW = m_fboWidth > 0 ? m_fboWidth : m_frameWidth;
    int fboH = m_fboHeight > 0 ? m_fboHeight : m_frameHeight;

    // Create HW render texture
    glGenTextures(1, &m_frameTexture);
    glBindTexture(GL_TEXTURE_2D, m_frameTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fboW, fboH, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create FBO
    if (m_fbo == 0)
    {
        glGenFramebuffers(1, &m_fbo);
        glGenRenderbuffers(1, &m_fbo_rbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glBindRenderbuffer(GL_RENDERBUFFER, m_fbo_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fboW, fboH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_fbo_rbo);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_frameTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        tico_debug_log("ERROR: FBO incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glViewport(0, 0, fboW, fboH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    tico_debug_log("Created HW render texture: %u (%dx%d) FBO: %u",
             m_frameTexture, fboW, fboH, m_fbo);

    return true;
}

void TicoCore::BindHWContext(bool enable)
{
    (void)enable; // Single context mode - no-op
}

void TicoCore::DestroyHWRenderContext()
{
    if (!m_hwRender || !s_hwRenderCallback.context_destroy)
        return;

    tico_debug_log("Calling context_destroy...");
    glFinish();
    s_hwRenderCallback.context_destroy();
    tico_debug_log("context_destroy done");

    s_hwRenderCallback = {};
    m_hwRender = false;
}

//==============================================================================
// Game Loading
//==============================================================================

bool TicoCore::LoadGame(const std::string &path)
{
    tico_debug_log("=== TicoCore::LoadGame ===");
    tico_debug_log("  path: %s", path.c_str());

    m_gamePath = path;

    if (!m_initialized)
    {
        tico_debug_log("Not initialized, calling Init()");
        if (!Init())
        {
            tico_debug_log("ERROR: Init() failed");
            return false;
        }
    }

    tico_debug_log("Opening ROM file...");

    // need_fullpath = false: load ROM into memory
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("ERROR: Failed to open file: %s", path.c_str());
        return false;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        tico_debug_log("ERROR: File is empty: %s", path.c_str());
        return false;
    }

    tico_debug_log("ROM size: %zu bytes (%.1f MB)", fileSize, fileSize / (1024.0 * 1024.0));

    std::vector<uint8_t> romData(fileSize);
    size_t bytesRead = fread(romData.data(), 1, fileSize, fp);
    fclose(fp);

    if (bytesRead != fileSize)
    {
        tico_debug_log("ERROR: Short read: %zu of %zu bytes", bytesRead, fileSize);
        return false;
    }

    struct retro_game_info gameInfo = {};
    gameInfo.path = path.c_str();
    gameInfo.data = romData.data();
    gameInfo.size = fileSize;

    tico_debug_log("Calling retro_load_game...");
    tico_debug_log("  gameInfo.path = %s", gameInfo.path);
    tico_debug_log("  gameInfo.size = %zu", gameInfo.size);

    if (!retro_load_game(&gameInfo))
    {
        tico_debug_log("ERROR: retro_load_game failed");
        return false;
    }
    tico_debug_log("retro_load_game succeeded");

    // Get AV info
    tico_debug_log("Getting AV info...");
    struct retro_system_av_info avInfo = {};
    retro_get_system_av_info(&avInfo);

    m_frameWidth = avInfo.geometry.base_width;
    m_frameHeight = avInfo.geometry.base_height;
    m_aspectRatio = avInfo.geometry.aspect_ratio > 0
                        ? avInfo.geometry.aspect_ratio
                        : (float)m_frameWidth / m_frameHeight;
    m_fps = avInfo.timing.fps > 0 ? avInfo.timing.fps : 60.0;
    m_sampleRate = avInfo.timing.sample_rate > 0 ? avInfo.timing.sample_rate : 44100.0;

    m_fboWidth = m_frameWidth;
    m_fboHeight = m_frameHeight;

    tico_debug_log("AV info: %dx%d @ %.2f fps, %.0f Hz, aspect %.3f",
             m_frameWidth, m_frameHeight, m_fps, m_sampleRate, m_aspectRatio);

    // Set up FBO and trigger deferred context_reset
    if (m_hwRender)
    {
        tico_debug_log("Initializing HW render context...");
        if (InitEGLDualContext())
        {
            if (s_hwRenderCallback.context_reset)
            {
                tico_debug_log("Calling context_reset...");
                s_hwRenderCallback.context_reset();
                tico_debug_log("context_reset done");
            }
            else
            {
                tico_debug_log("WARNING: No context_reset callback!");
            }
        }
        else
        {
            tico_debug_log("ERROR: InitEGLDualContext failed");
        }
    }
    else
    {
        tico_debug_log("Software rendering mode (no HW render requested)");
    }

    // Set controller - GBA uses standard joypad
    tico_debug_log("Setting controller port devices...");
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(2, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(3, RETRO_DEVICE_JOYPAD);

    m_gameLoaded = true;
    m_paused = false;
    tico_debug_log("LoadGame: Complete!");

    // Load native save data, falling back to legacy .srm saves when needed.
    LoadSaveData();

    return true;
}

void TicoCore::UnloadGame()
{
    if (!m_gameLoaded)
        return;

    SaveSaveData();

    // retro_unload_game must run before DestroyHWRenderContext
    tico_debug_log("Calling retro_unload_game...");
    retro_unload_game();
    tico_debug_log("retro_unload_game done");

    DestroyHWRenderContext();

    m_gameLoaded = false;

    // Drain stale GL errors
    while (glGetError() != GL_NO_ERROR) {}

    // Delete FBO objects
    tico_debug_log("Deleting TicoCore GL objects (tex=%u fbo=%u rbo=%u)",
             m_frameTexture, m_fbo, m_fbo_rbo);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (m_frameTexture != 0)
    {
        glDeleteTextures(1, &m_frameTexture);
        m_frameTexture = 0;
    }

    if (m_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }

    if (m_fbo_rbo != 0)
    {
        glDeleteRenderbuffers(1, &m_fbo_rbo);
        m_fbo_rbo = 0;
    }

    // Unbind all GL state so the context is clean for the next user
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glUseProgram(0);
    for (int i = 15; i >= 0; --i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    // Reset GL state
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Drain all pending GPU work
    glFlush();
    glFinish();

    // Clear any accumulated errors
    while (glGetError() != GL_NO_ERROR) {}

    tico_debug_log("UnloadGame GL cleanup complete");
}

//==============================================================================
// Frame execution
//==============================================================================

void TicoCore::RunFrame()
{
    if (!m_gameLoaded || m_paused)
        return;

    if (m_isRewinding)
    {
        if (!m_rewindBuffer.empty())
        {
            auto& state = m_rewindBuffer.back();
            retro_unserialize(state.data(), state.size());
            m_rewindBuffer.pop_back();
        }
        retro_run();
    }
    else
    {
        m_rewindFrameCounter++;
        if (m_rewindFrameCounter >= 2) // Save state every 2 frames for smoother rewind
        {
            m_rewindFrameCounter = 0;
            size_t size = retro_serialize_size();
            if (size > 0 && size < 1024 * 1024 * 32) // Sanity check to not allocate gigabytes (mGBA states are small)
            {
                std::vector<uint8_t> state(size);
                if (retro_serialize(state.data(), size))
                {
                    m_rewindBuffer.push_back(std::move(state));
                    // Keep up to 5 seconds of rewind history (60fps / 2 * 5 = 150 states)
                    if (m_rewindBuffer.size() > 150)
                    {
                        m_rewindBuffer.erase(m_rewindBuffer.begin());
                    }
                }
            }
        }
        retro_run();
    }
    
    // RetroAchievements frame tick
    if (m_rcClient) {
        rc_client_do_frame(m_rcClient);
    }
    
    // Process async badge uploads
    ProcessPendingBadgeUploads();
    
    // Execute pending RA callbacks on main thread
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for(auto& cb : cbs) cb();

    // Unbind core's FBO so subsequent rendering targets the default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void TicoCore::ResizeFBO(int width, int height)
{
    if (m_frameTexture == 0 || m_fbo == 0)
        return;

    tico_debug_log("ResizeFBO: %dx%d", width, height);

    glBindTexture(GL_TEXTURE_2D, m_frameTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, m_fbo_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_frameTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_fbo_rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        tico_debug_log("ERROR: ResizeFBO incomplete: 0x%x", status);
    }
    else
    {
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void TicoCore::Reset()
{
    if (m_gameLoaded)
    {
        retro_reset();
    }
}

void TicoCore::Pause() { m_paused = true; }
void TicoCore::Resume() { m_paused = false; }

//==============================================================================
// Input
//==============================================================================

void TicoCore::SetInputState(unsigned port, unsigned id, bool pressed)
{
    if (port < 4 && id < 16)
    {
        m_inputState[port][id] = pressed;
    }
}

void TicoCore::SetAnalogState(unsigned port, unsigned index, unsigned id, int16_t value)
{
    if (port < 4 && index < 2 && id < 2)
    {
        m_analogState[port][index][id] = value;
    }
}

void TicoCore::ClearInputs()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));
}

//==============================================================================
// Save States
//==============================================================================

void TicoCore::SaveState(const std::string &path)
{
    if (!m_gameLoaded)
        return;

    BindHWContext(true);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    size_t size = retro_serialize_size();
    if (size == 0)
    {
        tico_debug_log("SaveState: size 0");
        BindHWContext(false);
        return;
    }

    std::vector<uint8_t> data(size);
    bool success = retro_serialize(data.data(), size);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    BindHWContext(false);

    if (success)
    {
        FILE *fp = fopen(path.c_str(), "wb");
        if (fp)
        {
            fwrite(data.data(), 1, size, fp);
            fclose(fp);
            tico_debug_log("Saved state to %s", path.c_str());
        }
        else
        {
            tico_debug_log("ERROR: Failed to open file for save state: %s", path.c_str());
        }
    }
    else
    {
        tico_debug_log("ERROR: retro_serialize failed");
    }
}

void TicoCore::LoadState(const std::string &path)
{
    if (!m_gameLoaded)
        return;

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("LoadState: File not found: %s", path.c_str());
        return;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        return;
    }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, fp) != fileSize)
    {
        fclose(fp);
        return;
    }
    fclose(fp);

    BindHWContext(true);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Flush audio
    if (m_audioFlushCallback)
    {
        tico_debug_log("Resetting SDL audio device...");
        m_audioFlushCallback();
    }

    bool success = retro_unserialize(data.data(), fileSize);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    BindHWContext(false);

    if (success)
    {
        tico_debug_log("Loaded state from %s", path.c_str());
        tico_debug_log("Running one frame to force display update...");
        retro_run();
    }
    else
    {
        tico_debug_log("ERROR: retro_unserialize failed");
    }
}

//==============================================================================
// Libretro Callbacks
//==============================================================================

bool TicoCore::EnvironmentCallback(unsigned cmd, void *data)
{
    if (!s_instance)
        return false;
    return s_instance->HandleEnvironment(cmd, data);
}

void TicoCore::VideoRefreshCallback(const void *data, unsigned width,
                                    unsigned height, size_t pitch)
{
    if (!s_instance)
        return;
    s_instance->HandleVideoRefresh(data, width, height, pitch);
}

void TicoCore::AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_instance && s_instance->m_audioSampleCallback)
    {
        s_instance->m_audioSampleCallback(left, right);
    }
}

size_t TicoCore::AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    if (s_instance && s_instance->m_audioSampleBatchCallback)
    {
        return s_instance->m_audioSampleBatchCallback(data, frames);
    }
    return frames;
}

void TicoCore::InputPollCallback()
{
    // Input is polled externally
}

int16_t TicoCore::InputStateCallback(unsigned port, unsigned device,
                                     unsigned index, unsigned id)
{
    if (!s_instance)
        return 0;
    return s_instance->HandleInputState(port, device, index, id);
}

void TicoCore::LogCallback(enum retro_log_level level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';

    switch (level)
    {
    case RETRO_LOG_ERROR:
        LOG_ERROR("CORE", "%s", buffer);
        break;
    case RETRO_LOG_WARN:
        LOG_WARN("CORE", "%s", buffer);
        break;
    case RETRO_LOG_INFO:
        LOG_INFO("CORE", "%s", buffer);
        break;
    default:
        LOG_DEBUG("CORE", "%s", buffer);
        break;
    }
}

bool TicoCore::SetRumbleStateCallback(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
#ifdef __SWITCH__
    if (!s_vibrationInitialized || port >= 4) 
        return false;
        
    float amplitude = (float)strength / 65535.0f;
    
    int target_device = 1;
    if (port == 0) {
        u8 opMode = appletGetOperationMode();
        target_device = (opMode == AppletOperationMode_Handheld) ? 0 : 1;
    } else {
        target_device = port + 1;
    }

    HidVibrationValue *v = s_currentVibration[target_device];

    if (effect == RETRO_RUMBLE_STRONG) {
        v[0].amp_low = amplitude;
        v[1].amp_low = amplitude;
    } else if (effect == RETRO_RUMBLE_WEAK) {
        v[0].amp_high = amplitude;
        v[1].amp_high = amplitude;
    }

    hidSendVibrationValues(s_vibrationHandles[target_device], v, 2);
    
    return true;
#else
    return false;
#endif
}

//==============================================================================
// Thread waits callback
//==============================================================================
bool TicoCore::ClearThreadWaitsCallback(unsigned cmd, void *data)
{
    // No-op stub — must exist to prevent NULL dereference in threaded renderer
    (void)cmd;
    (void)data;
    return true;
}

//==============================================================================
// Instance Callbacks - Environment Handler
//==============================================================================

bool TicoCore::HandleEnvironment(unsigned cmd, void *data)
{
    unsigned base_cmd = cmd & 0xFF;
    
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    {
        auto *cb = (struct retro_log_callback *)data;
        cb->log = LogCallback;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    {
        auto *cb = (retro_rumble_interface *)data;
        if (cb) {
            cb->set_rumble_state = SetRumbleStateCallback;
            tico_debug_log("ENV: Provided Rumble Interface");
            return true;
        }
        return false;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    {
        *(const char **)data = m_systemDir.c_str();
        tico_debug_log("ENV: GET_SYSTEM_DIRECTORY -> %s", m_systemDir.c_str());
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    {
        *(const char **)data = m_saveDir.c_str();
        tico_debug_log("ENV: GET_SAVE_DIRECTORY -> %s", m_saveDir.c_str());
        return true;
    }

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        const enum retro_pixel_format *fmt = (const enum retro_pixel_format *)data;
        if (fmt) {
            m_pixelFormat = *fmt;
            tico_debug_log("ENV: SET_PIXEL_FORMAT to %d", m_pixelFormat);
            return true;
        }
        return false;
    }

    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        auto *hw = (struct retro_hw_render_callback *)data;
        
        s_hwRenderCallback = *hw;
        m_hwRender = true;

        hw->get_current_framebuffer = []() -> uintptr_t
        {
            if (s_instance)
                return s_instance->m_fbo;
            return 0;
        };
        hw->get_proc_address = [](const char *sym) -> retro_proc_address_t
        {
            return (retro_proc_address_t)eglGetProcAddress(sym);
        };

        tico_debug_log("ENV: SET_HW_RENDER accepted - context_type=%d, version=%d.%d",
                 hw->context_type, hw->version_major, hw->version_minor);
        return true;
    }

    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    {
        const struct retro_memory_map *mem_map = (const struct retro_memory_map *)data;
        m_memoryMaps.clear();
        if (mem_map) {
            for (unsigned i = 0; i < mem_map->num_descriptors; i++) {
                const auto& desc = mem_map->descriptors[i];
                if (desc.ptr) {
                    TicoMemoryMap map;
                    map.start = desc.start;
                    map.length = desc.len;
                    map.ptr = (uint8_t*)desc.ptr;
                    m_memoryMaps.push_back(map);
                }
            }
            tico_debug_log("ENV: SET_MEMORY_MAPS (%u descriptors processed)", mem_map->num_descriptors);
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto *var = (struct retro_variable *)data;
        if (!var || !var->key)
            return false;

        if (!m_configLoaded)
            LoadConfig();

        auto it = m_configOptions.find(var->key);
        if (it != m_configOptions.end())
        {
            var->value = it->second.c_str();
            return true;
        }

        // Key not found in config - return false so the core uses defaults
        var->value = nullptr;
        return false;
    }

    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        auto *avInfo = (struct retro_system_av_info *)data;
        m_frameWidth = avInfo->geometry.base_width;
        m_frameHeight = avInfo->geometry.base_height;
        if (avInfo->geometry.aspect_ratio > 0)
        {
            m_aspectRatio = avInfo->geometry.aspect_ratio;
        }
        m_fps = avInfo->timing.fps > 0 ? avInfo->timing.fps : 60.0;
        int newMaxW = avInfo->geometry.max_width > 0 ? (int)avInfo->geometry.max_width : m_frameWidth;
        int newMaxH = avInfo->geometry.max_height > 0 ? (int)avInfo->geometry.max_height : m_frameHeight;
        if (newMaxW != m_fboWidth || newMaxH != m_fboHeight)
        {
            m_fboWidth = newMaxW;
            m_fboHeight = newMaxH;
            ResizeFBO(m_fboWidth, m_fboHeight);
        }
        tico_debug_log("ENV: SET_SYSTEM_AV_INFO: base %dx%d, FBO %dx%d @ %.2f fps",
                 m_frameWidth, m_frameHeight, m_fboWidth, m_fboHeight, m_fps);
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        auto *geom = (struct retro_game_geometry *)data;
        m_frameWidth = geom->base_width;
        m_frameHeight = geom->base_height;
        if (geom->aspect_ratio > 0)
        {
            m_aspectRatio = geom->aspect_ratio;
        }
        int newMaxW = geom->max_width > 0 ? (int)geom->max_width : m_frameWidth;
        int newMaxH = geom->max_height > 0 ? (int)geom->max_height : m_frameHeight;
        if (newMaxW != m_fboWidth || newMaxH != m_fboHeight)
        {
            m_fboWidth = newMaxW;
            m_fboHeight = newMaxH;
            ResizeFBO(m_fboWidth, m_fboHeight);
        }
        return true;
    }
    
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        auto *msg = (const retro_message *)data;
        if (msg && msg->msg)
        {
            m_osdMessage = msg->msg;
            m_osdFrames = msg->frames;
        }
        return true;
    }
    
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    {
        auto *msg = (const retro_message_ext *)data;
        if (msg && msg->msg)
        {
            m_osdMessage = msg->msg;
            m_osdFrames = msg->duration;
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = m_variablesUpdated;
        m_variablesUpdated = false;
        return true;

    //==================================================================
    // Additional environment commands required by mgba
    //==================================================================

    // GLSM/core options - accept silently
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return true;

    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;

    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        return true;

    // Perf interface - return false (no perf counters, core handles NULL gracefully)
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        return false;

    // Clear thread waits callback - critical for threaded renderer
    // Without this, retro_unload_game crashes on NULL dereference
    case RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB:
    {
        if (data) {
            *(retro_environment_t *)data = ClearThreadWaitsCallback;
            tico_debug_log("ENV: GET_CLEAR_ALL_THREAD_WAITS_CB provided");
            return true;
        }
        return false;
    }

    // Poll type override - accept silently (used by threaded renderer)
    case RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE:
        return true;

    // Core options V2 - accept to signal category support
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        return true;

    // Core options update display callback
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;

    // Input descriptors - accept silently
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;

    // Support no game - not applicable
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;

    // Get username
    case RETRO_ENVIRONMENT_GET_USERNAME:
        *(const char**)data = "Player";
        return true;

    // Get language
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned*)data = 0; // English
        return true;

    // Frame time callback
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        return true;



    // Save state in background
    case RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND:
        return true;

    default:
        // Log unhandled commands for debugging
        if (base_cmd < 100) {
            tico_debug_log("ENV: Unhandled cmd %u (0x%x) -> false", cmd, cmd);
        }
        break;
    }

    return false;
}

void TicoCore::HandleVideoRefresh(const void *data, unsigned width,
                                  unsigned height, size_t pitch)
{
    if (!data && !m_hwRender)
        return;

    // Resize FBO if dimensions changed
    if ((int)width != m_frameWidth || (int)height != m_frameHeight)
    {
        m_frameWidth = width;
        m_frameHeight = height;
        m_fboWidth = width;
        m_fboHeight = height;

        if (m_hwRender && m_frameTexture != 0)
        {
            ResizeFBO(width, height);
        }
    }

    // For HW render, the core renders directly to our FBO
    // For SW render, upload pixel data
    if (!m_hwRender && data)
    {
        if (m_frameTexture == 0)
        {
            glGenTextures(1, &m_frameTexture);
        }

        glBindTexture(GL_TEXTURE_2D, m_frameTexture);

        GLint format = GL_RGBA;
        GLint type = GL_UNSIGNED_BYTE;
        int bpp = 4;

        if (m_pixelFormat == RETRO_PIXEL_FORMAT_0RGB1555) {
            format = GL_BGRA;
            type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
            bpp = 2;
        } else if (m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
            format = GL_BGRA;
            type = GL_UNSIGNED_INT_8_8_8_8_REV;
            bpp = 4;
        } else if (m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
            format = GL_RGB;
            type = GL_UNSIGNED_SHORT_5_6_5;
            bpp = 2;
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / bpp);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     format, type, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        
        // Force alpha to 1.0 since mGBA formats (0RGB1555 and XRGB8888) have alpha=0
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }

    // Apply post-processing shader if pipeline is ready
    if (m_shaderPipelineReady && m_frameTexture != 0 && m_activeShader != ShaderType::None)
    {
        ApplyShader(m_frameWidth, m_frameHeight);
    }
}

int16_t TicoCore::HandleInputState(unsigned port, unsigned device,
                                   unsigned index, unsigned id)
{
    if (port >= 4)
        return 0;

    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id < 16)
        {
            return m_inputState[port][id] ? 1 : 0;
        }
    }
    else if (device == RETRO_DEVICE_ANALOG)
    {
        if (index < 2 && id < 2)
        {
            return m_analogState[port][index][id];
        }
    }

    return 0;
}

//==============================================================================
// Configuration
//==============================================================================

void TicoCore::LoadConfig()
{
    if (m_configLoaded)
        return;

    const char *configPath;
#ifdef __SWITCH__
    configPath = "sdmc:/tico/config/cores/mgba.jsonc";
#else
    configPath = "tico/config/cores/mgba.jsonc";
#endif

    std::ifstream f(configPath);
    if (!f.good())
    {
        tico_debug_log("No config found at %s. Using defaults.", configPath);
        m_configLoaded = true; // Mark as loaded so we don't retry
        return;
    }

    nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
    if (j.is_discarded())
    {
        tico_debug_log("ERROR: Failed to parse config at %s", configPath);
        m_configLoaded = true;
        return;
    }

    for (auto &el : j.items())
    {
        if (el.value().is_string())
        {
            m_configOptions[el.key()] = el.value().get<std::string>();
        }
        else if (el.value().is_boolean())
        {
            m_configOptions[el.key()] = el.value().get<bool>() ? "true" : "false";
        }
        else if (el.value().is_number())
        {
            m_configOptions[el.key()] = std::to_string(el.value().get<float>());
        }
    }

    m_configLoaded = true;
    tico_debug_log("Loaded %lu options from %s", m_configOptions.size(), configPath);
}

std::string TicoCore::GetConfigValue(const std::string &key, const std::string &defaultVal)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        return it->second;
    }
    return defaultVal;
}

bool TicoCore::GetVariable(const char *key, const char **value)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        *value = it->second.c_str();
        return true;
    }
    return false;
}

void TicoCore::LoadRAConfig()
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    std::ifstream file(accountsPath);
    if (!file.is_open()) {
        tico_debug_log("WARN: accounts.jsonc not found at %s", accountsPath.c_str());
        return;
    }

    tico_debug_log("RA: Found accounts.jsonc at %s", accountsPath.c_str());
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false, true);
    if (!j.is_discarded() && j.is_object()) {
        m_raEnabled = j.value("ra_enabled", false);
        m_raUsername = j.value("ra_username", "");
        m_raToken = j.value("ra_token", "");
        m_raPassword = j.value("ra_password", "");
        m_raHardcore = j.value("ra_hardcore_mode", false);
        
        // Read alert position
        std::string posStr = j.value("ra_alert_position", "top_right");
        if (posStr == "top_left") m_raAlertPosition = RAAlertPosition::TopLeft;
        else if (posStr == "top_right") m_raAlertPosition = RAAlertPosition::TopRight;
        else if (posStr == "bottom_left") m_raAlertPosition = RAAlertPosition::BottomLeft;
        else if (posStr == "bottom_right") m_raAlertPosition = RAAlertPosition::BottomRight;
        
        tico_debug_log("RA: Config loaded (Enabled: %d, User: %s, HasToken: %d, HasPassword: %d)",
            m_raEnabled, m_raUsername.c_str(), !m_raToken.empty(), !m_raPassword.empty());
    } else {
        tico_debug_log("WARN: Failed to parse accounts.jsonc for RA settings.");
    }
}

void TicoCore::SaveRAToken(const std::string& token)
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    nlohmann::json j = nlohmann::json::object();
    
    // Read existing config
    std::ifstream inFile(accountsPath);
    if (inFile.is_open()) {
        auto parsed = nlohmann::json::parse(inFile, nullptr, false, true);
        inFile.close();
        if (!parsed.is_discarded()) j = parsed;
    }
    
    // Update token
    j["ra_token"] = token;
    m_raToken = token;
    
    // Write back
    std::ofstream outFile(accountsPath);
    if (outFile.is_open()) {
        outFile << j.dump(4);
        outFile.close();
        tico_debug_log("RA: Token saved to accounts.jsonc");
    } else {
        tico_debug_log("RA: WARNING - Failed to save token to %s", accountsPath.c_str());
    }
}

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core)
{
    uint32_t console_id = 5; // Default to Game Boy Advance
    size_t dot_pos = core->m_gamePath.find_last_of('.');
    if (dot_pos != std::string::npos) {
        std::string ext = core->m_gamePath.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".gb") {
            console_id = 3; // Game Boy
        } else if (ext == ".gbc") {
            console_id = 4; // Game Boy Color
        } else if (ext == ".gba") {
            console_id = 5; // Game Boy Advance
        }
    }
    core->m_consoleId = console_id;

    tico_debug_log("RA: Identifying game... (Console ID: %u)", console_id);
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(), nullptr, 0,
        [](int result, const char* error_message, rc_client_t* client, void* userdata) {
            TicoCore* core = (TicoCore*)userdata;
            if (result == RC_OK) {
                tico_debug_log("RA: Game loaded and identified!");
                const rc_client_game_t* game = rc_client_get_game_info(client);
                if (game && game->title) {
                    core->PushRANotification("RetroAchievements",
                        std::string("Playing: ") + game->title, "ra_icon");
                }
                // Preload all achievement badges in the background
                core->PreloadRABadges();
            } else {
                tico_debug_log("RA: Failed to identify game: %s", error_message ? error_message : "Unknown");
                core->PushRANotification("RetroAchievements", 
                    "Rom hash doesn't match or unable to recognize the game, achievements disabled.", "ra_icon");
            }
        }, core);
}

void TicoCore::RALoginWithPassword(rc_client_t* c, TicoCore* core)
{
    if (core->m_raPassword.empty()) {
        tico_debug_log("RA: No password configured. Continuing without RA.");
        return;
    }
    
    tico_debug_log("RA: Logging in with password...");
    rc_client_begin_login_with_password(c, core->m_raUsername.c_str(), core->m_raPassword.c_str(),
        [](int res, const char* err, rc_client_t* c, void* ud) {
            TicoCore* core = (TicoCore*)ud;
            if (res == RC_OK) {
                const rc_client_user_t* user = rc_client_get_user_info(c);
                if (user && user->token) {
                    tico_debug_log("RA: Password login successful! Saving token...");
                    core->SaveRAToken(user->token);
                } else {
                    tico_debug_log("RA: Password login OK but no token returned");
                }
                RAIdentifyGame(c, core);
            } else {
                tico_debug_log("RA: Password login failed: %s. Continuing without RA.", err ? err : "Unknown");
                core->PushRANotification("RetroAchievements", 
                    "Failed to authenticate, check your username/password and try again.", "ra_icon");
            }
        }, core);
}

void TicoCore::PushRANotification(const std::string& title, const std::string& desc,
                                   const std::string& badge)
{
    RANotification n;
    n.title = title;
    n.description = desc;
    n.badge_name = badge;
    n.timer = 0.0f;
    
    // Look up badge texture
    if (badge == "ra_icon") {
        n.textureId = m_raIconTexture;
    } else if (!badge.empty()) {
        n.textureId = GetRABadgeTexture(badge);
    }
    
    // Cap at 5 visible notifications
    if (m_raNotifications.size() >= 5) {
        m_raNotifications.erase(m_raNotifications.begin());
    }
    m_raNotifications.push_back(std::move(n));
    tico_debug_log("RA: Notification pushed: %s - %s (badge: %s, tex: %u)",
        title.c_str(), desc.c_str(), badge.c_str(), n.textureId);
}

unsigned int TicoCore::GetRABadgeTexture(const std::string& badge_name)
{
    // Check cache first
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;
    
    // Try loading from SD card cache
    std::string path = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data) {
        unsigned int tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);
        m_raBadgeCache[badge_name] = tex;
        return tex;
    }
    return 0;
}

void TicoCore::DownloadAndCacheBadge(const std::string& badge_name)
{
    // Check if already cached on disk
    std::string cachePath = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    FILE* check = fopen(cachePath.c_str(), "rb");
    if (check) { fclose(check); return; } // already on disk
    
    // Download from RA
    std::string url = "https://media.retroachievements.org/Badge/" + badge_name + ".png";
    std::string response;
    
    CURL* curl = curl_easy_init();
    if (!curl) return;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || httpCode != 200 || response.empty()) {
        tico_debug_log("RA: Failed to download badge %s (http %ld)", badge_name.c_str(), httpCode);
        return;
    }
    
    // Ensure directory exists
    mkdir("sdmc:/tico/assets/ra", 0777);
    
    // Save to disk
    FILE* fp = fopen(cachePath.c_str(), "wb");
    if (fp) {
        fwrite(response.data(), 1, response.size(), fp);
        fclose(fp);
        tico_debug_log("RA: Cached badge %s (%zu bytes)", badge_name.c_str(), response.size());
    }
}

void TicoCore::PreloadRABadges()
{
    if (!m_rcClient) return;
    
    tico_debug_log("RA: Preloading achievement badges...");
    
    // Get all achievement lists
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(m_rcClient,
        RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) {
        tico_debug_log("RA: No achievement list to preload");
        return;
    }
    
    // Collect all unique badge names
    std::vector<std::string> badges;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
            const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
            if (ach && ach->badge_name[0]) {
                std::string bn = ach->badge_name;
                // Check if not already cached on disk
                std::string path = "sdmc:/tico/assets/ra/" + bn + ".png";
                FILE* check = fopen(path.c_str(), "rb");
                if (check) { fclose(check); continue; }
                badges.push_back(bn);
            }
        }
    }
    rc_client_destroy_achievement_list(list);
    
    if (badges.empty()) {
        tico_debug_log("RA: All badges already cached");
        return;
    }
    
    tico_debug_log("RA: Need to download %zu badges", badges.size());
    
    // Queue downloads on the RA worker thread (via a regular job mechanism)
    // We'll use a simple approach: spawn them as background tasks
    for (const auto& badge : badges) {
        // Push to job queue so worker thread does the download
        std::string badgeCopy = badge;
        {
            std::lock_guard<std::mutex> lock(m_raJobMutex);
            // We abuse the job queue: url = badge download URL, post_data = badge_name,
            // callback = nullptr (special marker for badge download)
            RAJob job;
            job.url = "__badge__";
            job.post_data = badgeCopy;
            job.callback = nullptr;
            job.callback_data = nullptr;
            m_raJobQueue.push_back(std::move(job));
        }
        m_raJobCond.notify_one();
    }
}

void TicoCore::LoadRAIcon()
{
    // Try loading ra.svg - but nanosvg is only in TicoOverlay.
    // Instead, try loading a cached PNG version, or just skip if not available.
    // The SVG will be loaded by TicoOverlay which has nanosvg.
    tico_debug_log("RA: LoadRAIcon called (will be loaded by overlay)");
}

void TicoCore::ProcessPendingBadgeUploads()
{
    std::vector<std::pair<std::string, std::vector<unsigned char>>> uploads;
    {
        std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
        if (m_raPendingBadgeUploads.empty()) return;
        uploads = std::move(m_raPendingBadgeUploads);
    }
    
    for (auto& [name, data] : uploads) {
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
        if (pixels) {
            unsigned int tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(pixels);
            m_raBadgeCache[name] = tex;
        }
    }
}

//==============================================================================
// Shader Pipeline
//==============================================================================

// Shader sources are now in TicoShaders.cpp

unsigned int TicoCore::CompileShaderProgram(const char *vsSrc, const char *fsSrc)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSrc, NULL);
    glCompileShader(vs);

    GLint success = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(vs, 512, NULL, log);
        tico_debug_log("Shader VS compile error: %s", log);
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSrc, NULL);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(fs, 512, NULL, log);
        tico_debug_log("Shader FS compile error: %s", log);
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, 512, NULL, log);
        tico_debug_log("Shader link error: %s", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void TicoCore::InitShaderPipeline()
{
    if (m_shaderPipelineReady) return;

    // Create fullscreen quad
    float quadVerts[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 0.0f
    };

    glGenVertexArrays(1, &m_shaderVAO);
    glGenBuffers(1, &m_shaderVBO);
    glBindVertexArray(m_shaderVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_shaderVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // Create output FBO + texture (will be sized on first use)
    glGenFramebuffers(1, &m_shaderFBO);
    glGenTextures(1, &m_shaderTexture);

    // Compile default passthrough shader
    m_shaderProgram = CompileShaderProgram(
        GetShaderVertexSource(),
        GetShaderFragmentSource(ShaderType::None));

    m_shaderPipelineReady = true;
    tico_debug_log("Shader pipeline initialized");
}

void TicoCore::DestroyShaderPipeline()
{
    if (m_shaderProgram) { glDeleteProgram(m_shaderProgram); m_shaderProgram = 0; }
    if (m_shaderFBO) { glDeleteFramebuffers(1, &m_shaderFBO); m_shaderFBO = 0; }
    if (m_shaderTexture) { glDeleteTextures(1, &m_shaderTexture); m_shaderTexture = 0; }
    if (m_shaderVAO) { glDeleteVertexArrays(1, &m_shaderVAO); m_shaderVAO = 0; }
    if (m_shaderVBO) { glDeleteBuffers(1, &m_shaderVBO); m_shaderVBO = 0; }
    m_shaderPipelineReady = false;
    m_shaderTexWidth = 0;
    m_shaderTexHeight = 0;
}

void TicoCore::SetShader(ShaderType type)
{
    if (type == m_activeShader) return;
    m_activeShader = type;

    if (!m_shaderPipelineReady) return;

    // Recompile shader program
    if (m_shaderProgram) {
        glDeleteProgram(m_shaderProgram);
    }

    m_shaderProgram = CompileShaderProgram(
        GetShaderVertexSource(),
        GetShaderFragmentSource(type));

    // If None, clear the shader texture so GetFrameTextureID returns raw
    if (type == ShaderType::None) {
        m_shaderTexWidth = 0;
        m_shaderTexHeight = 0;
    }

    const char *names[] = {"None", "LCD", "xBRZ", "Eagle", "Dot", "LCD Grid V2"};
    int idx = (int)type;
    if (idx >= 0 && idx < 6) {
        tico_debug_log("Shader set to: %s", names[idx]);
    }
}

void TicoCore::ApplyShader(int srcWidth, int srcHeight)
{
    if (!m_shaderPipelineReady || m_shaderProgram == 0 || m_frameTexture == 0)
        return;

    // Output resolution depends on shader type
    int outW = srcWidth;
    int outH = srcHeight;
    if (m_activeShader == ShaderType::xBRZ || m_activeShader == ShaderType::Eagle) {
        outW = srcWidth * 4;
        outH = srcHeight * 4;
    } else if (m_activeShader == ShaderType::LCD ||
               m_activeShader == ShaderType::Dot ||
               m_activeShader == ShaderType::LcdGridV2) {
        outW = 1280;
        outH = 720;
    }

    // Resize output texture if needed
    if (m_shaderTexWidth != outW || m_shaderTexHeight != outH)
    {
        glBindTexture(GL_TEXTURE_2D, m_shaderTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, outW, outH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, m_shaderFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_shaderTexture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_shaderTexWidth = outW;
        m_shaderTexHeight = outH;
    }

    // Save current GL state
    GLint prevFBO = 0, prevViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // Render to shader FBO
    glBindFramebuffer(GL_FRAMEBUFFER, m_shaderFBO);
    glViewport(0, 0, outW, outH);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_shaderProgram);

    // Set uniforms
    GLint locSource = glGetUniformLocation(m_shaderProgram, "Source");
    GLint locTexSize = glGetUniformLocation(m_shaderProgram, "TextureSize");
    GLint locOutSize = glGetUniformLocation(m_shaderProgram, "OutputSize");
    if (locSource >= 0) glUniform1i(locSource, 0);
    if (locTexSize >= 0) glUniform2f(locTexSize, (float)srcWidth, (float)srcHeight);
    if (locOutSize >= 0) glUniform2f(locOutSize, (float)outW, (float)outH);

    // Bind source game texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_frameTexture);

    // For xBRZ/Eagle, use nearest filtering on the source
    if (m_activeShader == ShaderType::xBRZ || m_activeShader == ShaderType::Eagle) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // Draw fullscreen quad
    glBindVertexArray(m_shaderVAO);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);

    // Restore source texture filtering
    if (m_activeShader == ShaderType::xBRZ || m_activeShader == ShaderType::Eagle) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    glUseProgram(0);

    // Restore previous GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}
