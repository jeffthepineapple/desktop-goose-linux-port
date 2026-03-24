#include "assets.h"
#include "config.h"
#include <SDL.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

const std::string ASSET_ROOT_NAME = "Assets";
fs::path ASSET_ROOT;

ItemData::ItemData() : pixbuf(nullptr), w(0), h(0) {}
ItemData::~ItemData() { if(pixbuf) g_object_unref(pixbuf); }

AssetManager g_assets;

void AssetManager::Init() {
    // Audio
    if (SDL_Init(SDL_INIT_AUDIO) == 0) {
        Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);
        LoadAudio(honks, "Sound/NotEmbedded/Honk1.mp3");
        LoadAudio(honks, "Sound/NotEmbedded/Honk2.mp3");
        LoadAudio(pats, "Sound/NotEmbedded/Pat1.mp3");
        LoadAudio(pats, "Sound/NotEmbedded/Pat2.mp3");
    }

    // Memes
    ScanFolder("Images/Memes", memePaths, {".jpg", ".png", ".jpeg"});
    // Text
    ScanFolder("Text/NotepadMessages", textPaths, {".txt"});
}

ItemData* AssetManager::GetRandomMeme() {
    if(memePaths.empty()) return nullptr;
    std::string p = memePaths[rand() % memePaths.size()];
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file(p.c_str(), &err);
    if(!pb) return nullptr;

    // Scale down huge images
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    if(w > 300) {
        float ratio = 300.0f / w;
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(pb, 300, h * ratio, GDK_INTERP_BILINEAR);
        g_object_unref(pb);
        pb = scaled;
    }

    // Ensure 4 bytes per pixel (RGBA) to satisfy Cairo's stride requirements
    if (!gdk_pixbuf_get_has_alpha(pb)) {
        GdkPixbuf* withAlpha = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
        g_object_unref(pb);
        pb = withAlpha;
    }

    ItemData* item = new ItemData();
    item->type = ItemData::MEME;
    item->pixbuf = pb;
    item->w = gdk_pixbuf_get_width(pb);
    item->h = gdk_pixbuf_get_height(pb);
    return item;
}

ItemData* AssetManager::GetRandomText() {
    if(textPaths.empty()) return nullptr;
    std::string p = textPaths[rand() % textPaths.size()];
    std::ifstream f(p);
    std::stringstream buffer;
    buffer << f.rdbuf();

    ItemData* item = new ItemData();
    item->type = ItemData::TEXT;
    item->textContent = buffer.str();
    item->w = 200; // Fixed width for notepad
    item->h = 150;
    return item;
}

ItemData* AssetManager::CreateTextItem(const std::string& text) {
    ItemData* item = new ItemData();
    item->type = ItemData::TEXT;
    item->textContent = text;
    item->w = 200;
    item->h = 150;
    return item;
}

void AssetManager::Honk() { if(g_config.audioEnabled && !honks.empty()) Mix_PlayChannel(-1, honks[rand()%honks.size()], 0); }
void AssetManager::Pat()  { if(g_config.audioEnabled && !pats.empty())  Mix_PlayChannel(-1, pats[rand()%pats.size()], 0); }

void AssetManager::LoadAudio(std::vector<Mix_Chunk*>& v, std::string p) {
    fs::path path = ASSET_ROOT / p;
    if(fs::exists(path)) v.push_back(Mix_LoadWAV(path.string().c_str()));
    // Use absolute path string.
} 

void AssetManager::ScanFolder(std::string rel, std::vector<std::string>& out, std::vector<std::string> exts) {
    fs::path p = ASSET_ROOT / rel;
    if(!fs::exists(p)) return;
    for(const auto& entry : fs::directory_iterator(p)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        for(const auto& e : exts) {
            if(ext == e) {
                out.push_back(entry.path().string());
                break;
            }
        }
    }
}
