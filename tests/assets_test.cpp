#include "assets.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

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

    // Surface() must yield native-endian premultiplied ARGB32 with correct
    // channel order (no R/B swap) so screenshots render true-color, not tinted.
    cairo_surface_t* opaque = item->Surface();
    assert(opaque != nullptr);
    assert(cairo_image_surface_get_format(opaque) == CAIRO_FORMAT_ARGB32);
    {
        cairo_surface_flush(opaque);
        const uint32_t px = *reinterpret_cast<const uint32_t*>(
            cairo_image_surface_get_data(opaque));
        assert(px == 0xff112233u); // A=ff, R=11, G=22, B=33
    }
    delete item;

    // A half-transparent green pixel must premultiply, not stay full-bright.
    GdkPixbuf* alphaSrc = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
    assert(alphaSrc != nullptr);
    gdk_pixbuf_fill(alphaSrc, 0x00ff0080u); // R=00 G=ff B=00 A=80
    ItemData* alphaItem = assets.CreateTransientMemeItem(alphaSrc, &error);
    g_object_unref(alphaSrc);
    assert(alphaItem != nullptr);
    cairo_surface_t* translucent = alphaItem->Surface();
    assert(translucent != nullptr);
    cairo_surface_flush(translucent);
    {
        const uint32_t px = *reinterpret_cast<const uint32_t*>(
            cairo_image_surface_get_data(translucent));
        const uint32_t a = (px >> 24) & 0xff;
        const uint32_t g = (px >> 8) & 0xff;
        assert(a == 0x80u);
        assert(g == (0xffu * 0x80u + 127u) / 255u); // premultiplied green
    }
    delete alphaItem;

    // Issue #10: text that fits the original 200x150 box keeps that size.
    ItemData* shortNote = assets.CreateTextItem("Hi");
    assert(shortNote != nullptr);
    assert(shortNote->type == ItemData::TEXT);
    assert(shortNote->w == kNoteMinWidth);
    assert(shortNote->h == kNoteMinHeight);
    delete shortNote;

    ItemData* fittingNote = assets.CreateTextItem(
        "Remember to water the plants and take out the trash before you "
        "leave for work this morning.");
    assert(fittingNote != nullptr);
    assert(fittingNote->h == kNoteMinHeight);
    delete fittingNote;

    // Text that doesn't fit grows, capped so it can't swallow the screen.
    const std::string longText(2000, 'x');
    ItemData* longNote = assets.CreateTextItem(longText);
    assert(longNote != nullptr);
    assert(longNote->w <= kNoteMaxWidth);
    assert(longNote->h > kNoteMinHeight);
    assert(longNote->h <= kNoteMaxHeight);
    delete longNote;

    return 0;
}
