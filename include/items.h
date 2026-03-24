#ifndef ITEMS_H
#define ITEMS_H

#include <string>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "goose_math.h"

struct ItemData {
    enum Type { MEME, TEXT } type;
    GdkPixbuf* pixbuf = nullptr; // For Memes
    std::string textContent;     // For Notepad
    int w = 0, h = 0;

    ItemData();
    ~ItemData();
};

struct DroppedItem {
    ItemData* data;
    Vector2 pos;
    float rotation;
    double timeDropped;
    bool isExpired(double time) { return (time - timeDropped) > 15.0; } // Disappear after 15s
};

#endif // ITEMS_H
