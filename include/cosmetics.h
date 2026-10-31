#pragma once

#include <cairo.h>
#include <string>
#include <vector>
#include "goose_math.h"

enum class CosmeticSlot { Hat, Glasses };
enum class CosmeticStyle {
    BaseballCap,
    TopHat,
    PartyHat,
    Crown,
    RoundGlasses,
    Shades,
    Aviators,
    Custom  // drawn from shapes
};

struct CosmeticColor {
    double r;
    double g;
    double b;
};

enum class ShapeKind { Polygon, Ellipse, Rect };
// Fill: role color plus the ink outline. Flat: role color only. Line: a
// stroke in the role color. Glass: accent at 26% alpha with a primary rim.
enum class ShapePaint { Fill, Flat, Line, Glass };

struct ShapePoint {
    double x;
    double y;
};

// One drawn piece of a custom item, in the same local units as the built-in
// shapes. Polygon: points. Ellipse: {center, radii}. Rect: {origin, size}.
struct CosmeticShape {
    ShapeKind kind = ShapeKind::Polygon;
    bool accent = false;   // color role: accent instead of primary
    ShapePaint paint = ShapePaint::Fill;
    bool pair = false;     // glasses: repeat for both eyes, mirrored on the left
    bool bridge = false;   // glasses: hidden when the two lenses collapse to one
    bool flip = false;     // hats: mirrored when the goose faces left
    bool smooth = false;   // polygon: round the corners
    bool closed = false;   // polygon lines: close the path
    double width = 0.0;    // stroke width, 0 for the paint's default
    double radius = 0.0;   // rect corner radius
    std::vector<ShapePoint> points;
};

struct CosmeticItem {
    std::string id;
    std::string label;
    CosmeticSlot slot;
    CosmeticStyle style;
    CosmeticColor primary;
    CosmeticColor accent;
    std::vector<CosmeticShape> shapes;  // only for CosmeticStyle::Custom
};

struct GooseSkin {
    std::string hat = "none";
    std::string glasses = "none";
};

struct SkinProfile {
    std::string id;
    std::string label;
    GooseSkin skin;
    bool builtIn = false;
};

struct CosmeticPose {
    Vector2 head;
    Vector2 forward;
    Vector2 side;
    float back = 0.0f;
};

void Cosmetics_Initialize();
// Re-reads skins.ini, replacing every custom item and saved look in memory.
void Cosmetics_Reload();
const std::vector<CosmeticItem>& Cosmetics_Items();
const std::vector<CosmeticItem>& Cosmetics_CustomItems();
const std::vector<SkinProfile>& Cosmetics_BuiltInProfiles();
const std::vector<SkinProfile>& Cosmetics_CustomProfiles();
const CosmeticItem* Cosmetics_FindItem(const std::string& id);
const SkinProfile* Cosmetics_FindProfile(const std::string& id);
const char* Cosmetics_SlotName(CosmeticSlot slot);
bool Cosmetics_ParseSlot(const std::string& text, CosmeticSlot* slotOut);
bool Cosmetics_SetItem(GooseSkin& skin, CosmeticSlot slot, const std::string& itemId,
                        std::string* errorOut = nullptr);
bool Cosmetics_ApplyProfile(GooseSkin& skin, const std::string& profileId,
                            std::string* errorOut = nullptr);
std::string Cosmetics_MatchingProfile(const GooseSkin& skin);
bool Cosmetics_SaveProfile(const std::string& profileId, const GooseSkin& skin,
                            std::string* errorOut = nullptr);
bool Cosmetics_DeleteProfile(const std::string& profileId,
                              std::string* errorOut = nullptr);
std::string Cosmetics_ConfigPath();
void Cosmetics_Draw(cairo_t* cr, const GooseSkin& skin, const CosmeticPose& pose);
