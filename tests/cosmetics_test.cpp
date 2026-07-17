#include "cosmetics.h"

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

    fs::remove_all(testDirectory, ignored);
    if (!passed) return 1;
    std::cout << "cosmetics contracts passed\n";
    return 0;
}
