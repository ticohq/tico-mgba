/// @file TicoCore.cpp
/// @brief Simplified libretro frontend for mgba with tico overlay
/// Software-rendered core: frames go to TicoShaderChain, saves are libretro's
/// <game>.srm (an older tico's <game>.sav is renamed to it)

#include "TicoCore.h"
#include "TicoSession.h"
#include "overlay/tico_config.h"
#include "TicoVulkan.h"
#include <archive.h>
#include <archive_entry.h>
#include "TicoConfig.h"
#include "TicoSafeFile.h"
#include "TicoUtils.h"
#include <algorithm>
#include <json.hpp>
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>
#include <ctime>
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
#include <switch.h>

#endif



#define tico_debug_log(...) LOG_CORE(__VA_ARGS__)

// Earlier versions kept in backups/ beside each file: the last few sessions'
// saves, and the state each slot held before it was saved over.
static constexpr int kSaveBackups = 3;
static constexpr int kStateBackups = 1;

// The cartridge clock (S-RTC, SPC7110), kept beside the save as <rom>.rtc
// like RetroArch does. Only those few carts have one.
static std::string RtcPath(const std::string &gamePath)
{
    std::string filename = gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);
    return TicoConfig::SavesPath() + filename + ".rtc";
}

void TicoCore::LoadRtcData()
{
    const size_t size = retro_get_memory_size(RETRO_MEMORY_RTC);
    void *data = retro_get_memory_data(RETRO_MEMORY_RTC);
    if (!size || !data)
        return;

    const std::string path = RtcPath(m_gamePath);
    std::ifstream file(path, std::ios::binary);
    if (file && file.read((char *)data, size))
        tico_debug_log("Loaded RTC from %s", path.c_str());
}

void TicoCore::SaveRtcData()
{
    const size_t size = retro_get_memory_size(RETRO_MEMORY_RTC);
    const void *data = retro_get_memory_data(RETRO_MEMORY_RTC);
    if (!size || !data)
        return;

    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    const std::string path = RtcPath(m_gamePath);
    if (TicoSafeFile::Write(path, data, size, kSaveBackups))
        tico_debug_log("Saved RTC to %s", path.c_str());
    else
        tico_debug_log("ERROR: could not save RTC to %s", path.c_str());
}

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

    // libretro's own name for it (.srm). Before, tico wrote <game>.sav: that
    // file becomes the .srm the first time the game runs.
    const std::string savePathSrm = TicoConfig::SavesPath() + filename + ".srm";
    const std::string savePathSav = TicoConfig::SavesPath() + filename + ".sav";
    struct stat srmStat;
    struct stat savStat;
    if (stat(savePathSrm.c_str(), &srmStat) != 0 && stat(savePathSav.c_str(), &savStat) == 0)
    {
        if (rename(savePathSav.c_str(), savePathSrm.c_str()) == 0)
            tico_debug_log("Renamed %s to .srm", savePathSav.c_str());
    }

    std::ifstream fileSrm(savePathSrm, std::ios::binary);
    if (fileSrm)
    {
        fileSrm.read((char *)data, size);
        tico_debug_log("Loaded SRAM from %s", savePathSrm.c_str());
        return;
    }

    // The rename failed (say, a read-only card): read the old file as it is.
    std::ifstream fileSav(savePathSav, std::ios::binary);
    if (fileSav)
    {
        fileSav.read((char *)data, size);
        tico_debug_log("Loaded SRAM from %s (legacy name)", savePathSav.c_str());
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
    TicoConfig::MakeDirs(TicoConfig::SavesPath());

    std::string savePath = TicoConfig::SavesPath() + filename + ".srm";

    if (TicoSafeFile::Write(savePath, data, size, kSaveBackups))
        tico_debug_log("Saved SRAM to %s", savePath.c_str());
    else
        tico_debug_log("ERROR: could not save SRAM to %s", savePath.c_str());
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
static const char *RAUserAgent();

// HW render callback storage

//==============================================================================
// RetroAchievements Callbacks
//==============================================================================
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    if (!s_instance) return 0;

    // rcheevos addresses GBA memory as one flat range: IWRAM at 0x00000,
    // EWRAM at 0x08000 and save RAM at 0x48000. mGBA's memory map uses the
    // bus addresses, so translate. GB/GBC addresses are the bus ones already.
    uint32_t real_address = address;
    if (s_instance->m_consoleId == 5) { // RC_CONSOLE_GAMEBOY_ADVANCE
        if (address < 0x08000)
            real_address = 0x03000000 + address;
        else if (address < 0x48000)
            real_address = 0x02000000 + (address - 0x08000);
        else if (address < 0x58000)
            real_address = 0x0E000000 + (address - 0x48000);
    }

    // mGBA describes its memory through RETRO_ENVIRONMENT_SET_MEMORY_MAPS
    if (!s_instance->m_memoryMaps.empty()) {
        for (const auto& map : s_instance->m_memoryMaps) {
            if (real_address >= map.start && real_address + num_bytes <= map.start + map.length) {
                memcpy(buffer, map.ptr + (real_address - map.start), num_bytes);
                return num_bytes;
            }
        }
        return 0;
    }

    // Without a memory map: system RAM from 0, save RAM after it.
    if (address < 0x20000) {
        // WRAM (typically 128KB = 0x20000)
        uint8_t* wram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        size_t wram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        if (wram && address + num_bytes <= wram_size) {
            memcpy(buffer, wram + address, num_bytes);
            return num_bytes;
        }
    } else {
        // SRAM
        uint8_t* sram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        uint32_t sram_addr = address - 0x20000;
        if (sram && sram_addr + num_bytes <= sram_size) {
            memcpy(buffer, sram + sram_addr, num_bytes);
            return num_bytes;
        }
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
        
        
        // Do the HTTP request on this worker thread
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string errorMsg;
        std::string requestUrl = job.url;
        
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
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

#ifndef TICO_APP_VERSION
#define TICO_APP_VERSION "dev"
#endif

// How RetroAchievements identifies this client: the frontend, the libretro
// core and the rcheevos integration, like other libretro frontends report it.
static const char *RAUserAgent()
{
    static std::string agent;
    if (agent.empty())
    {
        retro_system_info info = {};
        retro_get_system_info(&info);
        agent = std::string("tico-mgba/") + TICO_APP_VERSION + " (Nintendo Switch) mgba_libretro/" +
                (info.library_version ? info.library_version : "unknown");
        char clause[64] = "";
        if (rc_client_get_user_agent_clause(nullptr, clause, sizeof(clause)) > 0)
            agent += std::string(" ") + clause;
    }
    return agent.c_str();
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
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
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
// Content paths
//==============================================================================

namespace {
std::string ContentRoot(const char *key, const char *defaultRoot)
{
    static nlohmann::json config = [] {
#ifdef __SWITCH__
        std::ifstream f("sdmc:/tico/config/cores/mgba.jsonc");
#else
        std::ifstream f("tico/config/cores/mgba.jsonc");
#endif
        nlohmann::json j = f.good() ? nlohmann::json::parse(f, nullptr, false, true)
                                    : nlohmann::json::object();
        return j.is_object() ? j : nlohmann::json::object();
    }();

    std::string root = defaultRoot;
    auto it = config.find(key);
    if (it != config.end() && it->is_string() && !it->get<std::string>().empty())
        root = it->get<std::string>();
    if (root.back() != '/')
        root += '/';
    return root;
}
} // namespace

namespace TicoConfig {
// BIOS files (gba_bios.bin, gb_bios.bin, gbc_bios.bin, sgb_bios.bin) live in
// <system>/gba/, the module's system_dir, for every console it runs.
std::string SystemPath() { return ContentRoot("tico_system_path", "sdmc:/tico/system/") + "gba/"; }
std::string SavesPath() { return tico::UserContentRoot(ContentRoot("tico_saves_path", "sdmc:/tico/saves/"), true) + CURRENT_SLUG + "/"; }
std::string StatesPath() { return tico::UserContentRoot(ContentRoot("tico_states_path", "sdmc:/tico/states/"), false) + CURRENT_SLUG + "/"; }

void MakeDirs(const std::string &path)
{
    // A custom root may not exist yet, so create every missing level.
    for (size_t at = path.find('/', path.find(":/") != std::string::npos ? path.find(":/") + 2 : 1);
         at != std::string::npos; at = path.find('/', at + 1))
        mkdir(path.substr(0, at).c_str(), 0777);
}
} // namespace TicoConfig

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));

    m_systemDir = TicoConfig::SystemPath();
    m_saveDir = TicoConfig::SavesPath();
    TicoConfig::MakeDirs(m_systemDir);
    TicoConfig::MakeDirs(m_saveDir);
}

//==============================================================================
// Motion sensors (gyro and tilt cartridges: WarioWare: Twisted!, Yoshi
// Topsy-Turvy, Kirby Tilt 'n' Tumble), from the Switch's six-axis sensor
//==============================================================================
#ifdef __SWITCH__
static HidSixAxisSensorHandle s_sixAxisHandheld;
static HidSixAxisSensorHandle s_sixAxisFullKey;
static HidSixAxisSensorHandle s_sixAxisJoyDual[2];
static bool s_sixAxisStarted = false;

static void StartSixAxis()
{
    if (s_sixAxisStarted)
        return;
    hidGetSixAxisSensorHandles(&s_sixAxisHandheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&s_sixAxisFullKey, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(s_sixAxisJoyDual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidStartSixAxisSensor(s_sixAxisHandheld);
    hidStartSixAxisSensor(s_sixAxisFullKey);
    hidStartSixAxisSensor(s_sixAxisJoyDual[0]);
    hidStartSixAxisSensor(s_sixAxisJoyDual[1]);
    s_sixAxisStarted = true;
}

static void StopSixAxis()
{
    if (!s_sixAxisStarted)
        return;
    hidStopSixAxisSensor(s_sixAxisHandheld);
    hidStopSixAxisSensor(s_sixAxisFullKey);
    hidStopSixAxisSensor(s_sixAxisJoyDual[0]);
    hidStopSixAxisSensor(s_sixAxisJoyDual[1]);
    s_sixAxisStarted = false;
}

// The sensor player 1 is holding: the console in handheld mode, otherwise the
// Pro Controller, or the right Joy-Con of a pair.
static bool ReadSixAxis(HidSixAxisSensorState &state)
{
    if (!s_sixAxisStarted)
        return false;
    HidSixAxisSensorHandle handle;
    if (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld)
        handle = s_sixAxisHandheld;
    else
    {
        const u32 style = hidGetNpadStyleSet(HidNpadIdType_No1);
        if (style & HidNpadStyleTag_NpadFullKey)
            handle = s_sixAxisFullKey;
        else if (style & HidNpadStyleTag_NpadJoyDual)
            handle = s_sixAxisJoyDual[1];
        else
            return false;
    }
    return hidGetSixAxisSensorStates(handle, &state, 1) > 0;
}
#endif

static bool RETRO_CALLCONV SetSensorState(unsigned port, enum retro_sensor_action action, unsigned rate)
{
    (void)rate;
    if (port != 0)
        return false;
    switch (action)
    {
    case RETRO_SENSOR_ACCELEROMETER_ENABLE:
    case RETRO_SENSOR_GYROSCOPE_ENABLE:
#ifdef __SWITCH__
        StartSixAxis();
        return true;
#else
        return false;
#endif
    case RETRO_SENSOR_ACCELEROMETER_DISABLE:
    case RETRO_SENSOR_GYROSCOPE_DISABLE:
        return true; // stopped with the game
    default:
        return false; // no light sensor: Boktai uses the Solar Sensor setting
    }
}

// libretro: acceleration in g, rotation in radians per second. The Switch
// reports rotation in turns per second.
static float RETRO_CALLCONV GetSensorInput(unsigned port, unsigned id)
{
#ifdef __SWITCH__
    HidSixAxisSensorState state;
    if (port != 0 || !ReadSixAxis(state))
        return 0.0f;
    constexpr float kTurn = 6.28318530718f;
    switch (id)
    {
    case RETRO_SENSOR_ACCELEROMETER_X: return state.acceleration.x;
    case RETRO_SENSOR_ACCELEROMETER_Y: return state.acceleration.y;
    case RETRO_SENSOR_ACCELEROMETER_Z: return state.acceleration.z;
    case RETRO_SENSOR_GYROSCOPE_X: return state.angular_velocity.x * kTurn;
    case RETRO_SENSOR_GYROSCOPE_Y: return state.angular_velocity.y * kTurn;
    case RETRO_SENSOR_GYROSCOPE_Z: return state.angular_velocity.z * kTurn;
    default: return 0.0f;
    }
#else
    (void)port;
    (void)id;
    return 0.0f;
#endif
}

TicoCore::~TicoCore()
{
    tico_debug_log("~TicoCore: destroying (gameLoaded=%d, initialized=%d)",
             m_gameLoaded, m_initialized);

    UnloadGame();

    if (m_initialized)
    {
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


    // Ensure the system directory exists (BS-X and Sufami Turbo BIOS)
    struct stat st = {0};
    if (stat(m_systemDir.c_str(), &st) == -1) {
        mkdir(m_systemDir.c_str(), 0777);
    }
    tico_debug_log("System dir: %s", m_systemDir.c_str());

    // Load configuration to ensure variables are ready for init
    LoadConfig();
    tico_debug_log("Config loaded, %lu options", m_configOptions.size());

    bool soundEnabled = false;
    tico::SettingsStream audioIn("audio"); // The user's, from tico
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
                case RC_CLIENT_EVENT_RESET:
                    // rc_client asks for a reset when hardcore turns on mid-game,
                    // so nothing from the softcore session carries over.
                    tico_debug_log("RA: reset requested by rc_client");
                    s_instance->Reset();
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

//==============================================================================
// ROM files
//==============================================================================

static bool HasExtension(const std::string &name, const char *ext)
{
    const size_t n = strlen(ext);
    if (name.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)name[name.size() - n + i]) != ext[i])
            return false;
    return true;
}

bool TicoCore::IsArchivePath(const std::string &path)
{
    return HasExtension(path, ".zip") || HasExtension(path, ".7z") || HasExtension(path, ".rar");
}

// A file the core loads: the extensions the module lists for gba.
static bool IsRomName(const std::string &name)
{
    for (const char *ext : {".gba", ".gb", ".gbc", ".sgb"})
        if (HasExtension(name, ext))
            return true;
    return false;
}

bool TicoCore::ReadRomFile(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("ERROR: Failed to open file: %s", path.c_str());
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize <= 0)
    {
        fclose(fp);
        tico_debug_log("ERROR: File is empty: %s", path.c_str());
        return false;
    }
    out.resize((size_t)fileSize);
    const size_t bytesRead = fread(out.data(), 1, out.size(), fp);
    fclose(fp);
    if (bytesRead != out.size())
    {
        tico_debug_log("ERROR: Short read: %zu of %zu bytes", bytesRead, out.size());
        return false;
    }
    return true;
}

// The first GBA/GB/GBC ROM in a .zip, .7z or .rar (libarchive from portlibs).
bool TicoCore::ReadRomFromArchive(const std::string &path, std::vector<uint8_t> &out)
{
    struct archive *ar = archive_read_new();
    archive_read_support_format_zip(ar);
    archive_read_support_format_7zip(ar);
    archive_read_support_format_rar(ar);
    archive_read_support_format_rar5(ar);
    archive_read_support_filter_all(ar);
    if (archive_read_open_filename(ar, path.c_str(), 64 * 1024) != ARCHIVE_OK)
    {
        tico_debug_log("ERROR: Not a readable archive: %s (%s)", path.c_str(),
                       archive_error_string(ar));
        archive_read_free(ar);
        return false;
    }
    constexpr size_t kMaxRom = 64u * 1024u * 1024u;
    bool found = false;
    struct archive_entry *entry = nullptr;
    while (!found && archive_read_next_header(ar, &entry) == ARCHIVE_OK)
    {
        const char *name = archive_entry_pathname(entry);
        if (!name || archive_entry_filetype(entry) != AE_IFREG)
            continue;
        const std::string entryName = name;
        if (!IsRomName(entryName))
            continue;
        out.clear();
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0)
            out.reserve((size_t)archive_entry_size(entry));
        uint8_t chunk[64 * 1024];
        la_ssize_t read;
        while ((read = archive_read_data(ar, chunk, sizeof(chunk))) > 0 && out.size() <= kMaxRom)
            out.insert(out.end(), chunk, chunk + read);
        found = read == 0 && !out.empty() && out.size() <= kMaxRom;
        if (found)
            tico_debug_log("Loaded %s from %s", entryName.c_str(), path.c_str());
    }
    archive_read_free(ar);
    return found;
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

    // need_fullpath = false: load ROM into memory. A .zip, .7z or .rar holds
    // the ROM, which the core needs unpacked.
    if (IsArchivePath(path))
    {
        if (!ReadRomFromArchive(path, m_romData))
        {
            tico_debug_log("ERROR: No ROM found in %s", path.c_str());
            return false;
        }
    }
    else if (!ReadRomFile(path, m_romData))
    {
        return false;
    }
    tico_debug_log("ROM size: %zu bytes (%.1f MB)", m_romData.size(),
                   m_romData.size() / (1024.0 * 1024.0));

    struct retro_game_info gameInfo = {};
    gameInfo.path = path.c_str();
    gameInfo.data = m_romData.data();
    gameInfo.size = m_romData.size();


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

    tico_debug_log("AV info: %dx%d @ %.2f fps, %.0f Hz, aspect %.3f",
             m_frameWidth, m_frameHeight, m_fps, m_sampleRate, m_aspectRatio);

    // Set controller
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
    LoadRtcData();

    // Cheats start off; a marker left from last time means that session died
    // with cheats on (see ArmCheatGuard)
    LoadCheats();
    struct stat guard;
    if (stat(CheatFilePath(".crashguard").c_str(), &guard) == 0)
    {
        m_cheatsRecovered = true;
        tico_debug_log("CHEATS: the previous session ended uncleanly with cheats on");
    }
    DisarmCheatGuard();

    return true;
}

void TicoCore::UnloadGame()
{
#ifdef __SWITCH__
    StopSixAxis();
#endif
    if (!m_gameLoaded)
        return;

    SaveSaveData();
    SaveRtcData();
    DisarmCheatGuard(); // a clean exit, whatever cheats were on

    // retro_unload_game must run before DestroyHWRenderContext
    tico_debug_log("Calling retro_unload_game...");
    retro_unload_game();
    tico_debug_log("retro_unload_game done");

    m_gameLoaded = false;

}

//==============================================================================
// Frame execution
//==============================================================================

void TicoCore::RunFrame()
{
    if (!m_gameLoaded || m_paused)
        return;

    retro_run();

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
}

void TicoCore::Reset()
{
    if (m_gameLoaded)
    {
        retro_reset();
        // achievement progress restarts with the game
        if (m_rcClient)
            rc_client_reset(m_rcClient);
    }
}

//==============================================================================
// Cheats
//==============================================================================
extern "C" {
// mGBA's libretro core (tico additions): each cheat in a set of its own, with
// cheats hooking the same ROM address sharing one hook, and a reset that
// restores the ROM bytes hooks patched. retro_cheat_set/retro_cheat_reset
// merge every cheat into one set and leave breakpoints behind, which freezes
// games combining cheats that share a master code.
int tico_add_cheat_set(const char *name, const char *const *lines, int nLines, bool enabled, int type);
void tico_cheat_reset(void);
}

// sdmc:/tico/cheats/gba/<game><extension>
std::string TicoCore::CheatFilePath(const char *extension) const
{
    std::string name = m_gamePath;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    const std::string dir = "sdmc:/tico/cheats/" + TicoConfig::CURRENT_SLUG + "/";
    TicoConfig::MakeDirs(dir);
    return dir + name + extension;
}

// mGBA's .cheats: "# Name" starts a cheat, its code lines follow, and "!..."
// lines are directives ("!GSAv1"/"!PARv3" pick the codec, "!disabled" is
// ignored since every cheat starts off).
void TicoCore::LoadCheats()
{
    m_cheats.clear();
    std::ifstream file(CheatFilePath(".cheats"));
    if (!file.is_open())
        return;
    Cheat current;
    bool have = false;
    std::string line;
    while (std::getline(file, line))
    {
        const std::string text = TicoUtils::Trim(line);
        if (text.empty())
            continue;
        if (text[0] == '#')
        {
            if (have)
                m_cheats.push_back(current);
            current = Cheat();
            current.name = TicoUtils::Trim(text.substr(1));
            have = true;
        }
        else if (text[0] == '!')
        {
            if (have && text != "!disabled")
                current.directives.push_back(text);
        }
        else if (text[0] == '[' || text.rfind("cheats", 0) == 0)
        {
            continue; // EZ-Flash and RetroArch .cht blocks aren't this format
        }
        else
        {
            if (!have)
            {
                current = Cheat();
                current.name = "Cheat";
                have = true;
            }
            current.codes.push_back(text);
        }
    }
    if (have)
        m_cheats.push_back(current);
    tico_debug_log("CHEATS: %zu in %s", m_cheats.size(), CheatFilePath(".cheats").c_str());
}

void TicoCore::ApplyCheats()
{
    if (!m_gameLoaded)
        return;
    tico_cheat_reset(); // also restores what the previous batch patched
    int applied = 0;
    for (const Cheat &cheat : m_cheats)
    {
        if (!cheat.enabled || cheat.codes.empty())
            continue;
        // GBACheatType: 0 autodetect, 2 GameShark, 3 Pro Action Replay
        int type = 0;
        for (const std::string &directive : cheat.directives)
        {
            if (directive.rfind("!GSAv1", 0) == 0)
                type = 2;
            else if (directive.rfind("!PARv3", 0) == 0)
                type = 3;
        }
        std::vector<const char *> lines;
        for (const std::string &code : cheat.codes)
            lines.push_back(code.c_str());
        const int result = tico_add_cheat_set(cheat.name.c_str(), lines.data(), (int)lines.size(), true, type);
        tico_debug_log("CHEATS: '%s' type %d -> %d", cheat.name.c_str(), type, result);
        ++applied;
    }
    if (applied > 0)
        ArmCheatGuard();
    else
        DisarmCheatGuard();
}

void TicoCore::ToggleCheat(size_t index)
{
    if (index >= m_cheats.size() || IsHardcoreActive())
        return;
    m_cheats[index].enabled = !m_cheats[index].enabled;
    ApplyCheats();
}

// A marker listing the cheats on, removed on a clean exit: found at the next
// launch, it means the session froze or was killed with them on.
void TicoCore::ArmCheatGuard()
{
    std::ofstream out(CheatFilePath(".crashguard"), std::ios::trunc);
    for (const Cheat &cheat : m_cheats)
        if (cheat.enabled)
            out << cheat.name << "\n";
}

void TicoCore::DisarmCheatGuard()
{
    if (!m_gamePath.empty())
        remove(CheatFilePath(".crashguard").c_str());
}

bool TicoCore::IsHardcoreActive() const
{
    return m_rcClient && rc_client_get_hardcore_enabled(m_rcClient);
}

bool TicoCore::CanPause(int &secondsRemaining)
{
    secondsRemaining = 0;
    if (!m_gameLoaded || !IsHardcoreActive())
        return true;
    uint32_t framesRemaining = 0;
    if (rc_client_can_pause(m_rcClient, &framesRemaining))
        return true;
    const double fps = m_fps > 0.0 ? m_fps : 60.0;
    secondsRemaining = (int)((framesRemaining + fps - 1.0) / fps);
    if (secondsRemaining < 1)
        secondsRemaining = 1;
    return false;
}

void TicoCore::Idle()
{
    ProcessPendingBadgeUploads();
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for (auto &cb : cbs)
        cb();
    if (m_rcClient)
        rc_client_idle(m_rcClient);
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

// rc_client's achievement progress (hit counts, measured values) for a state
// file, so loading it restores where every achievement stood.
static std::string ProgressPath(const std::string &statePath)
{
    return statePath + ".ra";
}

bool TicoCore::SaveState(const std::string &path)
{
    if (!m_gameLoaded)
        return false;

    size_t size = retro_serialize_size();
    if (size == 0)
    {
        tico_debug_log("SaveState: size 0");
        return false;
    }

    std::vector<uint8_t> data(size);
    if (!retro_serialize(data.data(), size))
    {
        tico_debug_log("ERROR: retro_serialize failed");
        return false;
    }

    // the slot's previous state stays in backups/ (one level)
    const bool written = TicoSafeFile::Write(path, data.data(), size, kStateBackups);
    if (!written)
    {
        tico_debug_log("ERROR: Failed to write save state: %s", path.c_str());
        return false;
    }
    tico_debug_log("Saved state to %s", path.c_str());

    const std::string progressPath = ProgressPath(path);
    const size_t progressSize = m_rcClient ? rc_client_progress_size(m_rcClient) : 0;
    std::vector<uint8_t> progress(progressSize);
    if (progressSize > 0 &&
        rc_client_serialize_progress_sized(m_rcClient, progress.data(), progressSize) == RC_OK)
    {
        if (FILE *pf = fopen(progressPath.c_str(), "wb"))
        {
            fwrite(progress.data(), 1, progressSize, pf);
            fclose(pf);
        }
    }
    else
    {
        // a stale file would restore progress from an older state
        remove(progressPath.c_str());
    }
    return written;
}

bool TicoCore::LoadState(const std::string &path)
{
    if (!m_gameLoaded)
        return false;

    // RetroAchievements hardcore forbids loading states.
    if (IsHardcoreActive())
    {
        tico_debug_log("LoadState: refused, hardcore mode is active");
        return false;
    }

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("LoadState: File not found: %s", path.c_str());
        return false;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        return false;
    }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, fp) != fileSize)
    {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Flush audio
    if (m_audioFlushCallback)
    {
        tico_debug_log("Resetting SDL audio device...");
        m_audioFlushCallback();
    }

    bool success = retro_unserialize(data.data(), fileSize);

    if (success)
    {
        tico_debug_log("Loaded state from %s", path.c_str());
        // Restore achievement progress with the state; a state saved without
        // it resets progress, so nothing from the abandoned timeline counts.
        if (m_rcClient)
        {
            std::vector<uint8_t> progress;
            if (FILE *pf = fopen(ProgressPath(path).c_str(), "rb"))
            {
                fseek(pf, 0, SEEK_END);
                const long progressSize = ftell(pf);
                fseek(pf, 0, SEEK_SET);
                if (progressSize > 0)
                {
                    progress.resize((size_t)progressSize);
                    if (fread(progress.data(), 1, progress.size(), pf) != progress.size())
                        progress.clear();
                }
                fclose(pf);
            }
            if (progress.empty() ||
                rc_client_deserialize_progress_sized(m_rcClient, progress.data(), progress.size()) != RC_OK)
                rc_client_deserialize_progress_sized(m_rcClient, nullptr, 0);
        }
        tico_debug_log("Running one frame to force display update...");
        retro_run();
    }
    else
    {
        tico_debug_log("ERROR: retro_unserialize failed");
    }
    return success;
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

#ifdef __SWITCH__
namespace
{
// The vibration motors of the controller a port reads, set up again whenever
// another controller (or the same one held differently) takes it.
struct RumblePad
{
    HidNpadIdType id = HidNpadIdType_No1;
    u32 style = 0;
    s32 count = 0;
    HidVibrationDeviceHandle handles[2] = {};
    HidVibrationValue values[2] = {};
};
RumblePad s_rumble[4];

// The controller a port reads: player 1 is the handheld Joy-Con when no
// controller is player 1, as the input does.
bool RumbleTarget(unsigned port, HidNpadIdType &id, u32 &style)
{
    id = static_cast<HidNpadIdType>(HidNpadIdType_No1 + port);
    u32 styles = hidGetNpadStyleSet(id);
    if (port == 0 && !styles)
    {
        id = HidNpadIdType_Handheld;
        styles = hidGetNpadStyleSet(id);
    }
    for (u32 tag : {(u32)HidNpadStyleTag_NpadHandheld, (u32)HidNpadStyleTag_NpadFullKey,
                    (u32)HidNpadStyleTag_NpadJoyDual, (u32)HidNpadStyleTag_NpadJoyLeft,
                    (u32)HidNpadStyleTag_NpadJoyRight})
        if (styles & tag)
        {
            style = tag;
            return true;
        }
    return false; // nothing there, or a controller without HD rumble
}
} // namespace
#endif

bool TicoCore::SetRumbleStateCallback(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
#ifdef __SWITCH__
    if (port >= 4)
        return false;
    HidNpadIdType id;
    u32 style;
    if (!RumbleTarget(port, id, style))
        return false;
    RumblePad &pad = s_rumble[port];
    if (!pad.count || pad.id != id || pad.style != style)
    {
        pad = RumblePad();
        const s32 count = (style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight) ? 1 : 2;
        if (R_FAILED(hidInitializeVibrationDevices(pad.handles, count, id, (HidNpadStyleTag)style)))
            return false;
        pad.id = id;
        pad.style = style;
        pad.count = count;
        for (HidVibrationValue &value : pad.values)
        {
            value.freq_low = 160.0f;
            value.freq_high = 320.0f;
        }
    }
    // Controls > Vibration and Vibration strength
    float amplitude = 0.0f;
    if (SwitchFrontend::TicoConfig::GetConfigValue("vibration", "enabled") != "disabled")
    {
        const int percent = std::clamp(
            std::atoi(SwitchFrontend::TicoConfig::GetConfigValue("vibration_strength", "100").c_str()), 0, 100);
        amplitude = (float)strength / 65535.0f * (float)percent / 100.0f;
    }
    for (s32 i = 0; i < pad.count; ++i)
    {
        if (effect == RETRO_RUMBLE_STRONG)
            pad.values[i].amp_low = amplitude;
        else if (effect == RETRO_RUMBLE_WEAK)
            pad.values[i].amp_high = amplitude;
    }
    return R_SUCCEEDED(hidSendVibrationValues(pad.handles, pad.values, pad.count));
#else
    (void)port;
    (void)effect;
    (void)strength;
    return false;
#endif
}

void TicoCore::StopRumble()
{
#ifdef __SWITCH__
    for (RumblePad &pad : s_rumble)
    {
        if (!pad.count)
            continue;
        for (HidVibrationValue &value : pad.values)
            value.amp_low = value.amp_high = 0.0f;
        hidSendVibrationValues(pad.handles, pad.values, pad.count);
    }
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

    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
    {
        auto *sensors = (struct retro_sensor_interface *)data;
        if (!sensors)
            return false;
        sensors->set_sensor_state = SetSensorState;
        sensors->get_sensor_input = GetSensorInput;
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
        // mGBA joins "<dir>/<bios>": hand it the directory without its slash
        static std::string dir;
        dir = m_systemDir;
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        *(const char **)data = dir.c_str();
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
        // Software rendering only: frames go through TicoShaderChain.
        return false;

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
            // Settings from older tico builds stored tico's enabled/disabled
            // for every switch; mGBA spells most of its switches ON/OFF (and
            // one yes/no), and reads anything else as off.
            static const std::map<std::string, std::pair<const char *, const char *>> kSwitches = {
                {"mgba_use_bios", {"ON", "OFF"}},          {"mgba_skip_bios", {"ON", "OFF"}},
                {"mgba_sgb_borders", {"ON", "OFF"}},       {"mgba_color_correction", {"GBA", "OFF"}},
                {"mgba_interframe_blending", {"mix", "OFF"}}, {"mgba_force_gbp", {"ON", "OFF"}},
                {"mgba_allow_opposing_directions", {"yes", "no"}},
            };
            const auto sw = kSwitches.find(var->key);
            if (sw != kSwitches.end() && (it->second == "enabled" || it->second == "disabled"))
                it->second = it->second == "enabled" ? sw->second.first : sw->second.second;
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
        tico_debug_log("ENV: SET_SYSTEM_AV_INFO: %dx%d @ %.2f fps", m_frameWidth, m_frameHeight, m_fps);
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

    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
    {
        if (data)
        {
            // 1 = video enabled, 2 = audio enabled
            *(int*)data = 1 | 2;
        }
        return true;
    }

    default:
        // Log unhandled commands for debugging
        if (base_cmd != 47 && base_cmd < 100) {
            tico_debug_log("ENV: Unhandled cmd %u (0x%x) -> false", cmd, cmd);
        }
        break;
    }

    return false;
}

void TicoCore::HandleVideoRefresh(const void *data, unsigned width,
                                  unsigned height, size_t pitch)
{
    // NULL data is a frame dupe: the chain keeps showing the previous frame.
    if (!data)
        return;
    m_frameWidth = width;
    m_frameHeight = height;
    if (m_videoCallback)
        m_videoCallback(data, width, height, pitch, m_pixelFormat);
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

void TicoCore::SetOption(const std::string &key, const std::string &value)
{
    std::string &stored = m_configOptions[key];
    if (stored == value)
        return;
    stored = value;
    m_variablesUpdated = true;
}

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
    // tico hands over only a token, in the sealed session (TicoSession.h);
    // the password stays with tico.
    const tico::Session& session = tico::CurrentSession();
    m_raEnabled = session.valid && session.raEnabled && !session.raToken.empty();
    m_raUsername = session.raUsername;
    m_raToken = session.raToken;
    m_raPassword.clear();
    m_raHardcore = session.raHardcore;
    tico_debug_log("RA: Session %s (Enabled: %d, User: %s)",
        session.valid ? "opened" : "missing", m_raEnabled, m_raUsername.c_str());
}

void TicoCore::SaveRAToken(const std::string& token)
{
    // A refreshed token lives for this run only; tico keeps the account's own.
    m_raToken = token;
}

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core)
{
    // mGBA runs three consoles: tell them apart from the ROM itself, so a game
    // in an archive is identified too. A GBA header has 0x96 at 0xB2; a Game
    // Boy one sets bit 7 of 0x143 for Color games.
    const std::vector<uint8_t> &rom = core->m_romData;
    uint32_t console_id = 5; // RC_CONSOLE_GAMEBOY_ADVANCE
    if (rom.size() > 0xB2 && rom[0xB2] == 0x96)
        console_id = 5;
    else if (rom.size() > 0x143)
        console_id = (rom[0x143] & 0x80) ? 6 : 4; // RC_CONSOLE_GAMEBOY_COLOR : RC_CONSOLE_GAMEBOY
    core->m_consoleId = console_id;

    tico_debug_log("RA: Identifying game... (Console ID: %u)", console_id);
    // hashed from the loaded ROM, so a zipped game is recognized too
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(),
        core->m_romData.empty() ? nullptr : core->m_romData.data(), core->m_romData.size(),
        [](int result, const char* error_message, rc_client_t* client, void* userdata) {
            TicoCore* core = (TicoCore*)userdata;
            if (result == RC_OK) {
                tico_debug_log("RA: Game loaded and identified!");
                const rc_client_game_t* game = rc_client_get_game_info(client);
                if (game && game->title) {
                    core->PushRANotification("RetroAchievements",
                        std::string("Playing: ") + game->title, "ra_icon");
                }
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
    // Badge fetching off in tico: the pop-up shows the placeholder.
    if (!tico::CurrentSession().raBadges) n.badge_name = "ra_icon";
    n.timer = 0.0f;
    
    // Look up badge texture
    if (n.badge_name == "ra_icon") {
        n.textureId = m_raIconTexture;
    } else if (!badge.empty()) {
        n.textureId = GetRABadgeTexture(n.badge_name);
    }
    
    // Cap at 5 visible notifications
    if (m_raNotifications.size() >= 5) {
        m_raNotifications.erase(m_raNotifications.begin());
    }
    m_raNotifications.push_back(std::move(n));
    tico_debug_log("RA: Notification pushed: %s - %s (badge: %s)",
        title.c_str(), desc.c_str(), badge.c_str());
}

ImTextureID TicoCore::GetRABadgeTexture(const std::string& badge_name)
{
    // Check cache first
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;
    
    // Try loading from SD card cache
    std::string path = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data) {
        ImTextureID tex = TicoVulkan::CreateTextureRGBA(data, w, h);
        stbi_image_free(data);
        m_raBadgeCache[badge_name] = tex;
        return tex;
    }
    return ImTextureID_Invalid;
}



void TicoCore::LoadRAIcon()
{
    // Try loading ra.svg - but nanosvg is only in the overlay.
    // Instead, try loading a cached PNG version, or just skip if not available.
    // The SVG will be loaded by the overlay, which has nanosvg.
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
            m_raBadgeCache[name] = TicoVulkan::CreateTextureRGBA(pixels, w, h);
            stbi_image_free(pixels);
        }
    }
}
