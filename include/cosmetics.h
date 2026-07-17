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
    Aviators
};

struct CosmeticColor {
    double r;
    double g;
    double b;
};

struct CosmeticItem {
    std::string id;
    std::string label;
    CosmeticSlot slot;
    CosmeticStyle style;
    CosmeticColor primary;
    CosmeticColor accent;
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
const std::vector<CosmeticItem>& Cosmetics_Items();
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
