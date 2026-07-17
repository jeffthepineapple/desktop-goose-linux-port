#include "cosmetics.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include "config.h"

namespace fs = std::filesystem;

namespace {

bool g_initialized = false;
std::vector<SkinProfile> g_customProfiles;

const std::vector<CosmeticItem> BUILT_IN_ITEMS = {
    {"red-cap", "Red field cap", CosmeticSlot::Hat, CosmeticStyle::BaseballCap,
     {0.82, 0.12, 0.10}, {0.98, 0.68, 0.16}},
    {"blue-cap", "Blue flight cap", CosmeticSlot::Hat, CosmeticStyle::BaseballCap,
     {0.10, 0.35, 0.72}, {0.35, 0.78, 0.95}},
    {"top-hat", "Midnight top hat", CosmeticSlot::Hat, CosmeticStyle::TopHat,
     {0.08, 0.09, 0.12}, {0.82, 0.18, 0.18}},
    {"party-hat", "Confetti party hat", CosmeticSlot::Hat, CosmeticStyle::PartyHat,
     {0.12, 0.72, 0.78}, {0.98, 0.55, 0.10}},
    {"crown", "Golden crown", CosmeticSlot::Hat, CosmeticStyle::Crown,
     {0.96, 0.68, 0.08}, {0.80, 0.15, 0.12}},
    {"round", "Round spectacles", CosmeticSlot::Glasses, CosmeticStyle::RoundGlasses,
     {0.10, 0.10, 0.12}, {0.72, 0.88, 0.96}},
    {"shades", "Black shades", CosmeticSlot::Glasses, CosmeticStyle::Shades,
     {0.04, 0.05, 0.07}, {0.28, 0.68, 0.82}},
    {"aviators", "Sky aviators", CosmeticSlot::Glasses, CosmeticStyle::Aviators,
     {0.78, 0.52, 0.10}, {0.26, 0.68, 0.86}},
};

const std::vector<SkinProfile> BUILT_IN_PROFILES = {
    {"classic", "Classic goose", {"none", "none"}, true},
    {"scholar", "The Scholar", {"top-hat", "round"}, true},
    {"party", "Party Bird", {"party-hat", "shades"}, true},
    {"pilot", "The Pilot", {"blue-cap", "aviators"}, true},
    {"royal", "Royal Goose", {"crown", "round"}, true},
    {"incognito", "Incognito", {"red-cap", "shades"}, true},
};

std::string Trim(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                       [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return "";
    return std::string(first, last);
}

bool IsValidProfileId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '-' || c == '_';
    });
}

bool ItemMatchesSlot(const std::string& id, CosmeticSlot slot) {
    if (id == "none") return true;
    const CosmeticItem* item = Cosmetics_FindItem(id);
    return item && item->slot == slot;
}

bool WriteProfiles(const std::vector<SkinProfile>& profiles, std::string* errorOut) {
    const fs::path path = Cosmetics_ConfigPath();
    std::error_code directoryError;
    fs::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        if (errorOut) *errorOut = "could not create skin config directory: " + directoryError.message();
        return false;
    }

    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        if (errorOut) *errorOut = "could not write " + temporary.string();
        return false;
    }

    output << "# Desktop Goose custom skin profiles\n";
    for (const SkinProfile& profile : profiles) {
        output << "profile." << profile.id << ".hat=" << profile.skin.hat << '\n';
        output << "profile." << profile.id << ".glasses=" << profile.skin.glasses << '\n';
    }
    output.close();
    if (!output) {
        if (errorOut) *errorOut = "could not finish writing " + temporary.string();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }

    if (std::rename(temporary.string().c_str(), path.string().c_str()) != 0) {
        if (errorOut) *errorOut = "could not replace " + path.string();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

void LoadProfiles() {
    g_customProfiles.clear();
    std::ifstream input(Cosmetics_ConfigPath());
    if (!input) return;

    std::map<std::string, GooseSkin> loaded;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));
        if (key.rfind("profile.", 0) != 0) continue;

        const size_t propertyDot = key.rfind('.');
        if (propertyDot <= 8) continue;
        const std::string id = key.substr(8, propertyDot - 8);
        if (!IsValidProfileId(id)) continue;
        const std::string property = key.substr(propertyDot + 1);
        if (property == "hat" && ItemMatchesSlot(value, CosmeticSlot::Hat)) {
            loaded[id].hat = value;
        } else if (property == "glasses" && ItemMatchesSlot(value, CosmeticSlot::Glasses)) {
            loaded[id].glasses = value;
        }
    }

    for (const auto& entry : loaded) {
        const bool reserved = std::any_of(
            BUILT_IN_PROFILES.begin(), BUILT_IN_PROFILES.end(),
            [&](const SkinProfile& profile) { return profile.id == entry.first; });
        if (!reserved) {
            g_customProfiles.push_back({entry.first, entry.first, entry.second, false});
        }
    }
}

void SetColor(cairo_t* cr, CosmeticColor color, double alpha = 1.0) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
}

void RoundedRectangle(cairo_t* cr, double x, double y, double width, double height,
                      double radius) {
    const double right = x + width;
    const double bottom = y + height;
    cairo_new_sub_path(cr);
    cairo_arc(cr, right - radius, y + radius, radius, -M_PI_2, 0);
    cairo_arc(cr, right - radius, bottom - radius, radius, 0, M_PI_2);
    cairo_arc(cr, x + radius, bottom - radius, radius, M_PI_2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

void FillAndStroke(cairo_t* cr, CosmeticColor fill, double lineWidth = 1.2) {
    SetColor(cr, fill);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.09);
    cairo_set_line_width(cr, lineWidth);
    cairo_stroke(cr);
}

void DrawBaseballCap(cairo_t* cr, const CosmeticItem& item, const CosmeticPose& pose) {
    cairo_move_to(cr, -8, 0);
    cairo_curve_to(cr, -7, -9, 7, -9, 8, 0);
    cairo_close_path(cr);
    FillAndStroke(cr, item.primary);

    const double billDirection = pose.forward.x < -0.1f ? -1.0 : 1.0;
    cairo_save(cr);
    cairo_translate(cr, billDirection * 7.0, 0.5);
    cairo_scale(cr, 1.0, 0.35);
    cairo_arc(cr, 0, 0, 7, 0, M_PI * 2);
    cairo_restore(cr);
    FillAndStroke(cr, item.accent, 1.0);

    cairo_set_source_rgb(cr, item.accent.r, item.accent.g, item.accent.b);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, -7);
    cairo_line_to(cr, 0, -1);
    cairo_stroke(cr);
}

void DrawTopHat(cairo_t* cr, const CosmeticItem& item) {
    RoundedRectangle(cr, -7, -14, 14, 13, 2);
    FillAndStroke(cr, item.primary);
    SetColor(cr, item.accent);
    cairo_rectangle(cr, -7, -4, 14, 3);
    cairo_fill(cr);
    RoundedRectangle(cr, -11, -2, 22, 4, 2);
    FillAndStroke(cr, item.primary);
}

void DrawPartyHat(cairo_t* cr, const CosmeticItem& item) {
    cairo_move_to(cr, -8, 0);
    cairo_line_to(cr, 0, -18);
    cairo_line_to(cr, 8, 0);
    cairo_close_path(cr);
    FillAndStroke(cr, item.primary);

    SetColor(cr, item.accent);
    cairo_set_line_width(cr, 2.2);
    cairo_move_to(cr, -5, -5);
    cairo_line_to(cr, 5, -9);
    cairo_stroke(cr);
    cairo_arc(cr, 0, -19, 2.5, 0, M_PI * 2);
    cairo_fill(cr);
}

void DrawCrown(cairo_t* cr, const CosmeticItem& item) {
    cairo_move_to(cr, -9, 1);
    cairo_line_to(cr, -9, -10);
    cairo_line_to(cr, -4, -5);
    cairo_line_to(cr, 0, -13);
    cairo_line_to(cr, 4, -5);
    cairo_line_to(cr, 9, -10);
    cairo_line_to(cr, 9, 1);
    cairo_close_path(cr);
    FillAndStroke(cr, item.primary);
    SetColor(cr, item.accent);
    for (double x : {-5.0, 0.0, 5.0}) {
        cairo_arc(cr, x, -1.5, 1.2, 0, M_PI * 2);
        cairo_fill(cr);
    }
}

void DrawHat(cairo_t* cr, const CosmeticItem& item, const CosmeticPose& pose) {
    cairo_save(cr);
    cairo_translate(cr, pose.head.x, pose.head.y - 9.0);
    if (item.style == CosmeticStyle::BaseballCap) DrawBaseballCap(cr, item, pose);
    else if (item.style == CosmeticStyle::TopHat) DrawTopHat(cr, item);
    else if (item.style == CosmeticStyle::PartyHat) DrawPartyHat(cr, item);
    else if (item.style == CosmeticStyle::Crown) DrawCrown(cr, item);
    cairo_restore(cr);
}

void DrawRoundLens(cairo_t* cr, double x, const CosmeticItem& item, bool fill) {
    cairo_arc(cr, x, 0, 4.2, 0, M_PI * 2);
    if (fill) {
        SetColor(cr, item.accent, 0.26);
        cairo_fill_preserve(cr);
    }
    SetColor(cr, item.primary);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
}

void DrawGlasses(cairo_t* cr, const CosmeticItem& item, const CosmeticPose& pose) {
    const Vector2 side = Vector2::Normalize(pose.side);
    const float eyeLift = 3.0f + 1.5f * pose.back;
    const Vector2 center = pose.head + Vector2{0, -eyeLift};
    const double angle = std::atan2(side.y, side.x);
    const bool singleLens = pose.back > 0.82f;
    const double separation = singleLens ? 0.0 : (5.0 - 2.2 * pose.back);

    cairo_save(cr);
    cairo_translate(cr, center.x, center.y);
    cairo_rotate(cr, angle);

    if (item.style == CosmeticStyle::RoundGlasses) {
        DrawRoundLens(cr, -separation, item, true);
        if (!singleLens) DrawRoundLens(cr, separation, item, true);
    } else if (item.style == CosmeticStyle::Shades) {
        const double lensWidth = singleLens ? 8.5 : 7.5;
        RoundedRectangle(cr, -separation - lensWidth / 2.0, -3.2, lensWidth, 6.4, 1.5);
        FillAndStroke(cr, item.primary, 1.0);
        if (!singleLens) {
            RoundedRectangle(cr, separation - lensWidth / 2.0, -3.2, lensWidth, 6.4, 1.5);
            FillAndStroke(cr, item.primary, 1.0);
        }
    } else if (item.style == CosmeticStyle::Aviators) {
        cairo_save(cr);
        cairo_translate(cr, -separation, 0);
        cairo_scale(cr, 1.0, 0.82);
        DrawRoundLens(cr, 0, item, true);
        cairo_restore(cr);
        if (!singleLens) {
            cairo_save(cr);
            cairo_translate(cr, separation, 0);
            cairo_scale(cr, 1.0, 0.82);
            DrawRoundLens(cr, 0, item, true);
            cairo_restore(cr);
        }
    }

    if (!singleLens) {
        SetColor(cr, item.primary);
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, -separation + 4.0, 0);
        cairo_line_to(cr, separation - 4.0, 0);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

} // namespace

void Cosmetics_Initialize() {
    if (g_initialized) return;
    g_initialized = true;
    LoadProfiles();
}

const std::vector<CosmeticItem>& Cosmetics_Items() {
    return BUILT_IN_ITEMS;
}

const std::vector<SkinProfile>& Cosmetics_BuiltInProfiles() {
    return BUILT_IN_PROFILES;
}

const std::vector<SkinProfile>& Cosmetics_CustomProfiles() {
    Cosmetics_Initialize();
    return g_customProfiles;
}

const CosmeticItem* Cosmetics_FindItem(const std::string& id) {
    const auto item = std::find_if(BUILT_IN_ITEMS.begin(), BUILT_IN_ITEMS.end(),
                                   [&](const CosmeticItem& candidate) {
                                       return candidate.id == id;
                                   });
    return item == BUILT_IN_ITEMS.end() ? nullptr : &*item;
}

const SkinProfile* Cosmetics_FindProfile(const std::string& id) {
    Cosmetics_Initialize();
    for (const SkinProfile& profile : BUILT_IN_PROFILES) {
        if (profile.id == id) return &profile;
    }
    for (const SkinProfile& profile : g_customProfiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

const char* Cosmetics_SlotName(CosmeticSlot slot) {
    return slot == CosmeticSlot::Hat ? "hat" : "glasses";
}

bool Cosmetics_ParseSlot(const std::string& text, CosmeticSlot* slotOut) {
    if (text == "hat") {
        if (slotOut) *slotOut = CosmeticSlot::Hat;
        return true;
    }
    if (text == "glasses") {
        if (slotOut) *slotOut = CosmeticSlot::Glasses;
        return true;
    }
    return false;
}

bool Cosmetics_SetItem(GooseSkin& skin, CosmeticSlot slot, const std::string& itemId,
                        std::string* errorOut) {
    if (!ItemMatchesSlot(itemId, slot)) {
        if (errorOut) {
            const CosmeticItem* item = Cosmetics_FindItem(itemId);
            *errorOut = item
                ? itemId + " belongs in the " + Cosmetics_SlotName(item->slot) + " slot"
                : "unknown " + std::string(Cosmetics_SlotName(slot)) + " item: " + itemId;
        }
        return false;
    }
    if (slot == CosmeticSlot::Hat) skin.hat = itemId;
    else skin.glasses = itemId;
    return true;
}

bool Cosmetics_ApplyProfile(GooseSkin& skin, const std::string& profileId,
                            std::string* errorOut) {
    const SkinProfile* profile = Cosmetics_FindProfile(profileId);
    if (!profile) {
        if (errorOut) *errorOut = "unknown skin look: " + profileId;
        return false;
    }
    skin = profile->skin;
    return true;
}

std::string Cosmetics_MatchingProfile(const GooseSkin& skin) {
    Cosmetics_Initialize();
    for (const SkinProfile& profile : BUILT_IN_PROFILES) {
        if (profile.skin.hat == skin.hat && profile.skin.glasses == skin.glasses) {
            return profile.id;
        }
    }
    for (const SkinProfile& profile : g_customProfiles) {
        if (profile.skin.hat == skin.hat && profile.skin.glasses == skin.glasses) {
            return profile.id;
        }
    }
    return "custom";
}

bool Cosmetics_SaveProfile(const std::string& profileId, const GooseSkin& skin,
                            std::string* errorOut) {
    Cosmetics_Initialize();
    if (!IsValidProfileId(profileId)) {
        if (errorOut) *errorOut = "look names use 1-32 lowercase letters, numbers, '-' or '_'";
        return false;
    }
    for (const SkinProfile& profile : BUILT_IN_PROFILES) {
        if (profile.id == profileId) {
            if (errorOut) *errorOut = "built-in look cannot be overwritten: " + profileId;
            return false;
        }
    }
    if (!ItemMatchesSlot(skin.hat, CosmeticSlot::Hat) ||
        !ItemMatchesSlot(skin.glasses, CosmeticSlot::Glasses)) {
        if (errorOut) *errorOut = "cannot save a look containing unknown items";
        return false;
    }

    std::vector<SkinProfile> updated = g_customProfiles;
    auto profile = std::find_if(updated.begin(), updated.end(),
                                [&](const SkinProfile& candidate) {
                                    return candidate.id == profileId;
                                });
    if (profile == updated.end()) {
        updated.push_back({profileId, profileId, skin, false});
    } else {
        profile->skin = skin;
    }
    std::sort(updated.begin(), updated.end(),
              [](const SkinProfile& a, const SkinProfile& b) { return a.id < b.id; });
    if (!WriteProfiles(updated, errorOut)) return false;
    g_customProfiles = std::move(updated);
    return true;
}

bool Cosmetics_DeleteProfile(const std::string& profileId, std::string* errorOut) {
    Cosmetics_Initialize();
    for (const SkinProfile& profile : BUILT_IN_PROFILES) {
        if (profile.id == profileId) {
            if (errorOut) *errorOut = "built-in look cannot be deleted: " + profileId;
            return false;
        }
    }

    std::vector<SkinProfile> updated = g_customProfiles;
    const auto newEnd = std::remove_if(updated.begin(), updated.end(),
                                       [&](const SkinProfile& profile) {
                                           return profile.id == profileId;
                                       });
    if (newEnd == updated.end()) {
        if (errorOut) *errorOut = "unknown saved look: " + profileId;
        return false;
    }
    updated.erase(newEnd, updated.end());
    if (!WriteProfiles(updated, errorOut)) return false;
    g_customProfiles = std::move(updated);
    return true;
}

std::string Cosmetics_ConfigPath() {
    return (fs::path(Config_GetPath()).parent_path() / "skins.ini").string();
}

void Cosmetics_Draw(cairo_t* cr, const GooseSkin& skin, const CosmeticPose& pose) {
    if (!cr) return;
    const CosmeticItem* hat = skin.hat == "none" ? nullptr : Cosmetics_FindItem(skin.hat);
    const CosmeticItem* glasses =
        skin.glasses == "none" ? nullptr : Cosmetics_FindItem(skin.glasses);
    if (hat && hat->slot == CosmeticSlot::Hat) DrawHat(cr, *hat, pose);
    if (glasses && glasses->slot == CosmeticSlot::Glasses) DrawGlasses(cr, *glasses, pose);
}
