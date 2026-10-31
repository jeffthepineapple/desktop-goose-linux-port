#include "cosmetics.h"

#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <unistd.h>

namespace fs = std::filesystem;

static fs::path g_configPath;

std::string Config_GetPath() {
    return g_configPath.string();
}

static bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

int main() {
    const fs::path testDirectory =
        fs::temp_directory_path() / ("cppgoose-cosmetics-" + std::to_string(getpid()));
    std::error_code ignored;
    fs::remove_all(testDirectory, ignored);
    g_configPath = testDirectory / "config.ini";

    Cosmetics_Initialize();
    bool passed = true;
    passed &= Check(Cosmetics_Items().size() == 8, "expected eight built-in items");
    passed &= Check(Cosmetics_BuiltInProfiles().size() == 6,
                    "expected six built-in looks");

    std::unordered_set<std::string> itemIds;
    for (const CosmeticItem& item : Cosmetics_Items()) {
        passed &= Check(itemIds.insert(item.id).second, "duplicate item id: " + item.id);
    }

    GooseSkin skin;
    std::string error;
    passed &= Check(Cosmetics_ApplyProfile(skin, "party", &error), error);
    passed &= Check(skin.hat == "party-hat" && skin.glasses == "shades",
                    "party look did not equip both slots");
    passed &= Check(Cosmetics_MatchingProfile(skin) == "party",
                    "party look was not recognized");

    const GooseSkin unchanged = skin;
    passed &= Check(!Cosmetics_SetItem(skin, CosmeticSlot::Glasses, "red-cap", &error),
                    "hat was accepted in glasses slot");
    passed &= Check(skin.hat == unchanged.hat && skin.glasses == unchanged.glasses,
                    "failed equip mutated the skin");

    passed &= Check(Cosmetics_SetItem(skin, CosmeticSlot::Hat, "crown", &error), error);
    passed &= Check(Cosmetics_SetItem(skin, CosmeticSlot::Glasses, "aviators", &error), error);
    passed &= Check(Cosmetics_MatchingProfile(skin) == "custom",
                    "mixed look should initially be custom");

    passed &= Check(Cosmetics_SaveProfile("test-look", skin, &error), error);
    passed &= Check(fs::exists(testDirectory / "skins.ini"),
                    "saving a look did not create skins.ini");
    const SkinProfile* saved = Cosmetics_FindProfile("test-look");
    passed &= Check(saved && saved->skin.hat == "crown" && saved->skin.glasses == "aviators",
                    "saved look could not be loaded from the catalog");
    passed &= Check(Cosmetics_MatchingProfile(skin) == "test-look",
                    "saved look was not recognized");

    GooseSkin restored;
    passed &= Check(Cosmetics_ApplyProfile(restored, "test-look", &error), error);
    passed &= Check(restored.hat == skin.hat && restored.glasses == skin.glasses,
                    "saved look did not restore both slots");

    passed &= Check(!Cosmetics_SaveProfile("party", skin, &error),
                    "built-in look was overwritten");
    passed &= Check(!Cosmetics_SaveProfile("Bad Name", skin, &error),
                    "invalid look name was accepted");
    passed &= Check(Cosmetics_DeleteProfile("test-look", &error), error);
    passed &= Check(Cosmetics_FindProfile("test-look") == nullptr,
                    "deleted look remained in the catalog");

    // Recolored items from the web skin creator.
    {
        std::ofstream file(testDirectory / "skins.ini");
        file << "# generated\n"
                "item.neon-cap.label=Neon cap\n"
                "item.neon-cap.style=baseball-cap\n"
                "item.neon-cap.primary=#FF00AA\n"
                "item.neon-cap.accent=#00ffcc\n"
                "item.sunny.style=round\n"
                "item.sunny.primary=#102030\n"
                "item.sunny.accent=#ffee00\n"
                "item.crown.style=top-hat\n"
                "item.crown.primary=#000000\n"
                "item.crown.accent=#ffffff\n"
                "item.half.style=crown\n"
                "item.half.primary=#123456\n"
                "item.bad-color.style=crown\n"
                "item.bad-color.primary=red\n"
                "item.bad-color.accent=#ffffff\n"
                "profile.mine.hat=neon-cap\n"
                "profile.mine.glasses=sunny\n"
                "profile.swapped.hat=sunny\n"
                "profile.swapped.glasses=neon-cap\n";
    }
    Cosmetics_Reload();
    passed &= Check(Cosmetics_CustomItems().size() == 2,
                    "only complete, non-reserved items should load");
    const CosmeticItem* neon = Cosmetics_FindItem("neon-cap");
    passed &= Check(neon && neon->slot == CosmeticSlot::Hat &&
                    neon->style == CosmeticStyle::BaseballCap && neon->label == "Neon cap",
                    "custom hat did not load with its style and label");
    passed &= Check(neon && std::abs(neon->primary.r - 1.0) < 1e-9 &&
                    std::abs(neon->primary.g) < 1e-9 &&
                    std::abs(neon->primary.b - 170.0 / 255.0) < 1e-9,
                    "primary color was not parsed from hex");
    const CosmeticItem* sunny = Cosmetics_FindItem("sunny");
    passed &= Check(sunny && sunny->slot == CosmeticSlot::Glasses && sunny->label == "sunny",
                    "custom glasses should default their label to the id");
    const CosmeticItem* crown = Cosmetics_FindItem("crown");
    passed &= Check(crown && crown->style == CosmeticStyle::Crown,
                    "a custom item replaced a built-in item");
    passed &= Check(Cosmetics_FindProfile("mine") != nullptr,
                    "look using custom items did not load");
    const SkinProfile* swapped = Cosmetics_FindProfile("swapped");
    passed &= Check(!swapped || (swapped->skin.hat == "none" && swapped->skin.glasses == "none"),
                    "custom items were accepted in the wrong slot");

    GooseSkin mine;
    passed &= Check(Cosmetics_ApplyProfile(mine, "mine", &error), error);
    passed &= Check(mine.hat == "neon-cap" && mine.glasses == "sunny",
                    "custom look did not equip custom items");
    passed &= Check(Cosmetics_SaveProfile("other", mine, &error), error);
    Cosmetics_Reload();
    passed &= Check(Cosmetics_CustomItems().size() == 2 &&
                    Cosmetics_FindItem("neon-cap") != nullptr,
                    "saving a look dropped custom items from skins.ini");
    const CosmeticItem* reloaded = Cosmetics_FindItem("neon-cap");
    passed &= Check(reloaded && std::abs(reloaded->accent.g - 1.0) < 1e-9,
                    "custom item colors did not survive a save and reload");

    fs::remove_all(testDirectory, ignored);
    if (!passed) return 1;
    std::cout << "cosmetics contracts passed\n";
    return 0;
}
