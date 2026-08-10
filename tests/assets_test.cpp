#include "assets.h"

#include <cassert>
#include <cstddef>

int main() {
    AssetManager assets;
    const std::size_t cacheSize = assets.memeCache.size();

    GdkPixbuf* source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 600, 200);
    assert(source != nullptr);
    gdk_pixbuf_fill(source, 0x112233ff);

    std::string error;
    ItemData* item = assets.CreateTransientMemeItem(source, &error);
    assert(item != nullptr);
    assert(error.empty());
    assert(item->type == ItemData::MEME);
    assert(item->w == 300);
    assert(item->h == 100);
    assert(item->pixbuf != nullptr);
    assert(gdk_pixbuf_get_n_channels(item->pixbuf) == 4);
    assert(assets.memeCache.size() == cacheSize);

    g_object_unref(source);

    const guchar* pixels = gdk_pixbuf_read_pixels(item->pixbuf);
    assert(pixels != nullptr);
    assert(pixels[0] == 0x11);
    assert(pixels[1] == 0x22);
    assert(pixels[2] == 0x33);
    assert(pixels[3] == 0xff);
    assert(assets.memeCache.size() == cacheSize);

    delete item;
    return 0;
}
