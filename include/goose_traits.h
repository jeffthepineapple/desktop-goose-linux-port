#pragma once

#include <cstdint>

// A goose personality sampled from a two-dimensional Perlin noise field.
// The seed uniquely identifies the sample; the three biases directly influence
// the existing wander-cycle behavior probabilities.
struct GooseTraits {
    std::uint64_t seed = 0;
    int attackMouseBias = 0; // 0..50
    int memeFetchBias = 0;   // 0..60
    int noteFetchBias = 0;   // 0..40
};

// Deterministic entry point for tests and reproducible generation.
GooseTraits GooseTraits_FromPerlin(std::uint64_t mapSeed,
                                   std::uint64_t sampleIndex);

// Runtime entry point. Uses one randomly seeded map per process and a unique
// sample index for every spawned goose.
GooseTraits GooseTraits_Next();
