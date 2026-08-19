#ifndef ITEMS_H
#define ITEMS_H

#include <memory>
#include <string>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include "goose_math.h"

// Notepad note layout: original 200x150 box is the floor size; text that
// doesn't fit grows up to the max, instead of clipping past its border.
inline constexpr const char* kNoteFontDescription = "Sans 10";
inline constexpr int kNoteMinWidth = 200;
inline constexpr int kNoteMinHeight = 150;
inline constexpr int kNoteMaxWidth = 240;
inline constexpr int kNoteMaxHeight = 400;
inline constexpr int kNotePadding = 10;

struct ItemData {
    enum Type { MEME, TEXT } type;
    GdkPixbuf* pixbuf = nullptr; // For Memes
    std::shared_ptr<const std::string> textContent; // Shared note text to avoid duplicate copies
    int w = 0, h = 0;

    ItemData();
    ~ItemData();
    cairo_surface_t* Surface();
    ItemData(const ItemData&) = delete;
    ItemData& operator=(const ItemData&) = delete;
    const std::string& Text() const {
        static const std::string empty;
        return textContent ? *textContent : empty;
    }

private:
    cairo_surface_t* surface = nullptr;
};

struct DroppedItem {
    ItemData* data;
    Vector2 pos;
    float rotation;
    double timeDropped;
    bool isExpired(double time) { return (time - timeDropped) > 15.0; } // Disappear after 15s
};

#endif // ITEMS_H
