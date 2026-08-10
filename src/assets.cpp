#include "assets.h"
#include "config.h"
#include <SDL.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

const std::string ASSET_ROOT_NAME = "Assets";
fs::path ASSET_ROOT;

ItemData::ItemData() = default;

ItemData::~ItemData() {
    if (surface) cairo_surface_destroy(surface);
    if (pixbuf) g_object_unref(pixbuf);
}

cairo_surface_t* ItemData::Surface() {
    if (surface || !pixbuf) return surface;

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    if (stride < cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width)) return nullptr;

    surface = cairo_image_surface_create_for_data(
        gdk_pixbuf_get_pixels(pixbuf),
        CAIRO_FORMAT_ARGB32,
        width,
        height,
        stride
    );
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        surface = nullptr;
    }
    return surface;
}

AssetManager g_assets;

AssetManager::~AssetManager() {
    for (auto& [_, pixbuf] : memeCache) {
        if (pixbuf) g_object_unref(pixbuf);
    }
    memeCache.clear();
    textCache.clear();

    for (Mix_Chunk* chunk : honks) {
        if (chunk) Mix_FreeChunk(chunk);
    }
    for (Mix_Chunk* chunk : pats) {
        if (chunk) Mix_FreeChunk(chunk);
    }
    honks.clear();
    pats.clear();

    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        Mix_CloseAudio();
        Mix_Quit();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void AssetManager::Init() {
    // Audio
    if (SDL_Init(SDL_INIT_AUDIO) == 0) {
        Mix_Init(MIX_INIT_MP3);
        Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);
        LoadAudio(honks, "Sound/NotEmbedded/Honk1.mp3");
        LoadAudio(honks, "Sound/NotEmbedded/Honk2.mp3");
        LoadAudio(honks, "Sound/NotEmbedded/Honk3.mp3");
        LoadAudio(honks, "Sound/NotEmbedded/Honk4.mp3");
    }

    // Memes
    ScanFolder("Images/Memes", memePaths, {".jpg", ".png", ".jpeg"});
    // Text
    ScanFolder("Text/NotepadMessages", textPaths, {".txt"});
}

ItemData* AssetManager::GetRandomMeme() {
    if (memePaths.empty()) return nullptr;
    return CreateMemeItem(memePaths[rand() % memePaths.size()]);
}
 
GdkPixbuf* AssetManager::NormalizeMemePixbuf(GdkPixbuf* pixbuf, std::string* errorOut) {
    if (errorOut) errorOut->clear();
    if (!pixbuf) {
        if (errorOut) *errorOut = "image pixbuf is null";
        return nullptr;
    }

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    if (width <= 0 || height <= 0) {
        if (errorOut) *errorOut = "image has invalid dimensions";
        return nullptr;
    }

    GdkPixbuf* normalized = static_cast<GdkPixbuf*>(g_object_ref(pixbuf));
    if (width > 300) {
        const float ratio = 300.0f / width;
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
            normalized, 300, std::max(1, static_cast<int>(height * ratio)), GDK_INTERP_BILINEAR);
        g_object_unref(normalized);
        normalized = scaled;
        if (!normalized) {
            if (errorOut) *errorOut = "could not scale image";
            return nullptr;
        }
    }

    if (!gdk_pixbuf_get_has_alpha(normalized)) {
        GdkPixbuf* withAlpha = gdk_pixbuf_add_alpha(normalized, FALSE, 0, 0, 0);
        g_object_unref(normalized);
        normalized = withAlpha;
        if (!normalized) {
            if (errorOut) *errorOut = "could not convert image to RGBA";
            return nullptr;
        }
    }

    if (gdk_pixbuf_get_n_channels(normalized) != 4) {
        if (errorOut) *errorOut = "image could not be normalized to four channels";
        g_object_unref(normalized);
        return nullptr;
    }

    return normalized;
}

ItemData* AssetManager::CreateMemeItem(const std::string& path, std::string* errorOut) {
    if (errorOut) errorOut->clear();

    GdkPixbuf* pb = nullptr;
    auto cached = memeCache.find(path);
    if (cached != memeCache.end()) {
        pb = cached->second;
    } else {
        GError* err = nullptr;
        GdkPixbuf* loaded = gdk_pixbuf_new_from_file(path.c_str(), &err);
        if (!loaded) {
            if (errorOut) {
                *errorOut = err && err->message ? err->message : "could not load image";
            }
            if (err) g_error_free(err);
            return nullptr;
        }

        pb = NormalizeMemePixbuf(loaded, errorOut);
        g_object_unref(loaded);
        if (!pb) return nullptr;
        memeCache[path] = pb;
    }

    ItemData* item = new ItemData();
    item->type = ItemData::MEME;
    item->pixbuf = static_cast<GdkPixbuf*>(g_object_ref(pb));
    item->w = gdk_pixbuf_get_width(pb);
    item->h = gdk_pixbuf_get_height(pb);
    return item;
}

ItemData* AssetManager::CreateTransientMemeItem(GdkPixbuf* pixbuf, std::string* errorOut) {
    GdkPixbuf* normalized = NormalizeMemePixbuf(pixbuf, errorOut);
    if (!normalized) return nullptr;

    ItemData* item = new ItemData();
    item->type = ItemData::MEME;
    item->pixbuf = normalized;
    item->w = gdk_pixbuf_get_width(normalized);
    item->h = gdk_pixbuf_get_height(normalized);
    return item;
}

ItemData* AssetManager::GetRandomText() {
    if(textPaths.empty()) return nullptr;
    std::string p = textPaths[rand() % textPaths.size()];
    std::shared_ptr<const std::string> text;

    auto cached = textCache.find(p);
    if (cached != textCache.end()) {
        text = cached->second;
    } else {
        std::ifstream f(p);
        std::stringstream buffer;
        buffer << f.rdbuf();
        text = std::make_shared<const std::string>(buffer.str());
        textCache[p] = text;
    }

    ItemData* item = new ItemData();
    item->type = ItemData::TEXT;
    item->textContent = std::move(text);
    item->w = 200; // Fixed width for notepad
    item->h = 150;
    return item;
}

ItemData* AssetManager::CreateTextItem(const std::string& text) {
    ItemData* item = new ItemData();
    item->type = ItemData::TEXT;
    item->textContent = std::make_shared<const std::string>(text);
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
