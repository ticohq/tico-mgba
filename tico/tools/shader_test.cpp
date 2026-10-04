/// @file shader_test.cpp
/// @brief Desktop tool: run slang presets headlessly and report results.
///
///   shader_test <source.png> <out_dir|-> <preset.slangp>...
///   shader_test <source.png> <out_dir|-> @list.txt
///
/// Feeds the image as the core frame (XRGB8888), renders 60 frames of each
/// preset at 960x720, and prints one line per preset: OK/FAIL, load time, mean
/// brightness of the result. With an out_dir each result is saved as a PNG.

#include "TicoShaderChain.h"
#include "TicoVulkan.h"
#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"

#include <SDL.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s source.png out_dir|- preset.slangp... | @list\n", argv[0]);
        return 2;
    }
    int w, h, ch;
    unsigned char *rgba = stbi_load(argv[1], &w, &h, &ch, 4);
    if (!rgba)
    {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    std::vector<uint32_t> xrgb(w * h);
    for (int i = 0; i < w * h; i++)
        xrgb[i] = (rgba[i * 4] << 16) | (rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
    stbi_image_free(rgba);

    std::vector<std::string> presets;
    for (int i = 3; i < argc; i++)
    {
        if (argv[i][0] == '@')
        {
            std::ifstream list(argv[i] + 1);
            std::string line;
            while (std::getline(list, line))
                if (!line.empty())
                    presets.push_back(line);
        }
        else
            presets.push_back(argv[i]);
    }
    const std::string outDir = argv[2];

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *window = SDL_CreateWindow("shader_test", 0, 0, 960, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(960, 720);
    if (!TicoVulkan::Init(window, 960, 720))
        return 1;
    TicoShaderChain chain;
    if (!chain.Init())
        return 1;

    int failed = 0;
    for (size_t n = 0; n < presets.size(); n++)
    {
        const std::string &path = presets[n];
        std::string error;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = chain.LoadPreset(path, error);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double mean = 0.0;
        if (ok)
        {
            for (int f = 0; f < 60; f++)
            {
                chain.SetSourceFrame(xrgb.data(), w, h, w * 4, RETRO_PIXEL_FORMAT_XRGB8888);
                VkCommandBuffer cmd = TicoVulkan::BeginFrame();
                if (!cmd)
                    continue;
                ImGui::NewFrame();
                ImGui::Render();
                chain.Process(cmd, 960, 720, 4.0f / 3.0f, 60.0);
                TicoVulkan::EndFrame(nullptr);
            }
            std::string png = outDir == "-" ? "shader_test_last.png"
                                             : outDir + "/" + std::to_string(n) + ".png";
            if (chain.SaveOutputPNG(png))
            {
                int ow, oh, oc;
                unsigned char *out = stbi_load(png.c_str(), &ow, &oh, &oc, 3);
                if (out)
                {
                    for (int i = 0; i < ow * oh * 3; i++)
                        mean += out[i];
                    mean /= (double)ow * oh * 3;
                    stbi_image_free(out);
                }
            }
        }
        if (!ok)
            failed++;
        printf("%s\t%.0fms\t%.1f\t%s\t%s\n", ok ? "OK" : "FAIL", ms, mean, path.c_str(),
               ok ? "" : error.substr(0, error.find('\n', 200)).c_str());
        fflush(stdout);
    }
    printf("# %zu presets, %d failed\n", presets.size(), failed);

    chain.Shutdown();
    TicoSlang::Shutdown();
    TicoVulkan::Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return failed ? 1 : 0;
}
