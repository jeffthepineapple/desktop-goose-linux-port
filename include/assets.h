#pragma once
#include <gtk/gtk.h>
#include <SDL_mixer.h>
#include <vector>
#include <string>
#include <filesystem>
#include <list>
#include "items.h"

namespace fs = std::filesystem;

extern const std::string ASSET_ROOT_NAME;
extern fs::path ASSET_ROOT;


class AssetManager {
public:
    std::vector<Mix_Chunk*> honks, pats;
    std::vector<std::string> memePaths;
    std::vector<std::string> textPaths;

    void Init();
    ItemData* GetRandomMeme();
    ItemData* GetRandomText();
    ItemData* CreateTextItem(const std::string& text);
    void Honk();
    void Pat();

private:
    void LoadAudio(std::vector<Mix_Chunk*>& v, std::string p);
    void ScanFolder(std::string rel, std::vector<std::string>& out, std::vector<std::string> exts);
};

extern AssetManager g_assets;
