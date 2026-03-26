#ifndef ITEMS_H
#define ITEMS_H

#include <memory>
#include <string>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "goose_math.h"

struct ItemData {
    enum Type { MEME, TEXT } type;
    GdkPixbuf* pixbuf = nullptr; // For Memes
    std::shared_ptr<const std::string> textContent; // Shared note text to avoid duplicate copies
    int w = 0, h = 0;

    ItemData();
    ~ItemData();
    const std::string& Text() const {
        static const std::string empty;
        return textContent ? *textContent : empty;
    }
};

struct DroppedItem {
    ItemData* data;
    Vector2 pos;
    float rotation;
    double timeDropped;
    bool isExpired(double time) { return (time - timeDropped) > 15.0; } // Disappear after 15s
};

#endif // ITEMS_H
