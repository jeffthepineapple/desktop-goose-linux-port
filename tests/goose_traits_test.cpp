#include "goose_traits.h"

#include <cassert>
#include <cstdint>
#include <set>

namespace {

bool Same(const GooseTraits& left, const GooseTraits& right) {
    return left.seed == right.seed &&
           left.attackMouseBias == right.attackMouseBias &&
           left.memeFetchBias == right.memeFetchBias &&
           left.noteFetchBias == right.noteFetchBias;
}

void TestDeterministicSampling() {
    const GooseTraits first = GooseTraits_FromPerlin(0x123456789abcdef0ULL, 42);
    const GooseTraits second = GooseTraits_FromPerlin(0x123456789abcdef0ULL, 42);
    assert(Same(first, second));
}

void TestRangesAndUniqueSeeds() {
    std::set<std::uint64_t> seeds;
    for (std::uint64_t index = 0; index < 1024; ++index) {
        const GooseTraits traits = GooseTraits_FromPerlin(0xc0ffeeULL, index);
        assert(traits.attackMouseBias >= 0 && traits.attackMouseBias <= 50);
        assert(traits.memeFetchBias >= 0 && traits.memeFetchBias <= 60);
        assert(traits.noteFetchBias >= 0 && traits.noteFetchBias <= 40);
        assert(seeds.insert(traits.seed).second);
    }
}

void TestNoiseCreatesVariedProfiles() {
    std::set<int> attack;
    std::set<int> memes;
    std::set<int> notes;
    for (std::uint64_t index = 0; index < 64; ++index) {
        const GooseTraits traits = GooseTraits_FromPerlin(0x5eedULL, index);
        attack.insert(traits.attackMouseBias);
        memes.insert(traits.memeFetchBias);
        notes.insert(traits.noteFetchBias);
    }
    assert(attack.size() >= 8);
    assert(memes.size() >= 8);
    assert(notes.size() >= 8);
}

void TestRuntimeProfilesAreUnique() {
    std::set<std::uint64_t> seeds;
    std::set<std::uint32_t> profiles;
    for (int index = 0; index < 256; ++index) {
        const GooseTraits traits = GooseTraits_Next();
        const std::uint32_t profile =
            (static_cast<std::uint32_t>(traits.attackMouseBias) * 61U +
             static_cast<std::uint32_t>(traits.memeFetchBias)) * 41U +
            static_cast<std::uint32_t>(traits.noteFetchBias);
        assert(seeds.insert(traits.seed).second);
        assert(profiles.insert(profile).second);
    }
}

} // namespace

int main() {
    TestDeterministicSampling();
    TestRangesAndUniqueSeeds();
    TestNoiseCreatesVariedProfiles();
    TestRuntimeProfilesAreUnique();
    return 0;
}
