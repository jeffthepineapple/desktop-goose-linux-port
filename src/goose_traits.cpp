#include "goose_traits.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <random>

namespace {

constexpr std::uint64_t GOLDEN_STEP = 0x9e3779b97f4a7c15ULL;
constexpr std::size_t ATTACK_VALUES = 51;
constexpr std::size_t MEME_VALUES = 61;
constexpr std::size_t NOTE_VALUES = 41;
constexpr std::size_t PROFILE_COUNT =
    ATTACK_VALUES * MEME_VALUES * NOTE_VALUES;

std::uint64_t SplitMix64(std::uint64_t value) {
    value += GOLDEN_STEP;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

class PerlinMap {
public:
    explicit PerlinMap(std::uint64_t seed) {
        std::array<std::uint8_t, 256> base{};
        for (std::size_t i = 0; i < base.size(); ++i) {
            base[i] = static_cast<std::uint8_t>(i);
        }

        std::uint64_t state = seed;
        for (std::size_t remaining = base.size(); remaining > 1; --remaining) {
            state = SplitMix64(state);
            const std::size_t other = static_cast<std::size_t>(state % remaining);
            std::swap(base[remaining - 1], base[other]);
        }
        for (std::size_t i = 0; i < permutation_.size(); ++i) {
            permutation_[i] = base[i & 255U];
        }
    }

    double Sample(double x, double y) const {
        const int floorX = static_cast<int>(std::floor(x));
        const int floorY = static_cast<int>(std::floor(y));
        const int xi = floorX & 255;
        const int yi = floorY & 255;
        const double xf = x - std::floor(x);
        const double yf = y - std::floor(y);
        const double u = Fade(xf);
        const double v = Fade(yf);

        const int aa = permutation_[permutation_[xi] + yi];
        const int ab = permutation_[permutation_[xi] + yi + 1];
        const int ba = permutation_[permutation_[xi + 1] + yi];
        const int bb = permutation_[permutation_[xi + 1] + yi + 1];

        const double lower = Lerp(Gradient(aa, xf, yf),
                                  Gradient(ba, xf - 1.0, yf), u);
        const double upper = Lerp(Gradient(ab, xf, yf - 1.0),
                                  Gradient(bb, xf - 1.0, yf - 1.0), u);
        return std::clamp(Lerp(lower, upper, v), -1.0, 1.0);
    }

private:
    std::array<std::uint8_t, 512> permutation_{};

    static double Fade(double value) {
        return value * value * value *
               (value * (value * 6.0 - 15.0) + 10.0);
    }

    static double Lerp(double from, double to, double amount) {
        return from + amount * (to - from);
    }

    static double Gradient(int hash, double x, double y) {
        switch (hash & 7) {
            case 0: return x + y;
            case 1: return -x + y;
            case 2: return x - y;
            case 3: return -x - y;
            case 4: return x;
            case 5: return -x;
            case 6: return y;
            default: return -y;
        }
    }
};

int BiasFromNoise(double noise, int maximum) {
    const double unit = std::clamp((noise + 1.0) * 0.5, 0.0, 1.0);
    return static_cast<int>(std::lround(unit * maximum));
}

GooseTraits SampleTraits(const PerlinMap& map, std::uint64_t mapSeed,
                         std::uint64_t sampleIndex) {
    // Irrational steps scatter consecutive geese over the same continuous map
    // without collapsing them onto a visible integer grid.
    const double originX = static_cast<double>((mapSeed >> 8) & 0xffffU) / 257.0;
    const double originY = static_cast<double>((mapSeed >> 32) & 0xffffU) / 263.0;
    const double x = originX + static_cast<double>(sampleIndex) * 0.7548776662466927;
    const double y = originY + static_cast<double>(sampleIndex) * 0.5698402909980532;

    GooseTraits traits;
    traits.seed = SplitMix64(mapSeed + sampleIndex * GOLDEN_STEP);
    traits.attackMouseBias = BiasFromNoise(map.Sample(x, y), 50);
    traits.memeFetchBias = BiasFromNoise(map.Sample(x + 37.17, y + 11.53), 60);
    traits.noteFetchBias = BiasFromNoise(map.Sample(x - 19.41, y + 53.77), 40);
    return traits;
}

std::uint64_t RandomMapSeed() {
    std::random_device random;
    const std::uint64_t entropy =
        (static_cast<std::uint64_t>(random()) << 32) ^ random();
    const std::uint64_t clock = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return SplitMix64(entropy ^ clock);
}

std::size_t ProfileIndex(const GooseTraits& traits) {
    return (static_cast<std::size_t>(traits.attackMouseBias) * MEME_VALUES +
            static_cast<std::size_t>(traits.memeFetchBias)) * NOTE_VALUES +
           static_cast<std::size_t>(traits.noteFetchBias);
}

void ApplyProfileIndex(std::size_t profile, GooseTraits* traits) {
    traits->noteFetchBias = static_cast<int>(profile % NOTE_VALUES);
    profile /= NOTE_VALUES;
    traits->memeFetchBias = static_cast<int>(profile % MEME_VALUES);
    traits->attackMouseBias = static_cast<int>(profile / MEME_VALUES);
}

} // namespace

GooseTraits GooseTraits_FromPerlin(std::uint64_t mapSeed,
                                   std::uint64_t sampleIndex) {
    const PerlinMap map(mapSeed);
    return SampleTraits(map, mapSeed, sampleIndex);
}

GooseTraits GooseTraits_Next() {
    static const std::uint64_t mapSeed = RandomMapSeed();
    static const PerlinMap map(mapSeed);
    static std::uint64_t nextSample = 0;
    static std::bitset<PROFILE_COUNT> usedProfiles;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);
    GooseTraits traits = SampleTraits(map, mapSeed, nextSample++);
    const std::size_t perlinProfile = ProfileIndex(traits);
    for (std::size_t offset = 0; offset < PROFILE_COUNT; ++offset) {
        const std::size_t candidate = (perlinProfile + offset) % PROFILE_COUNT;
        if (usedProfiles.test(candidate)) continue;
        usedProfiles.set(candidate);
        if (candidate != perlinProfile) ApplyProfileIndex(candidate, &traits);
        return traits;
    }

    // Every possible integer profile is in use. The seed remains unique even
    // though no new attack/meme/note combination can be represented.
    return traits;
}
