#pragma once
#include <initializer_list>
#include <utility>

// Preset infrastructure (agent-wiki/plan-roadmap.md B9). The values below are PLACEHOLDERS
// -- curation by ear is a follow-up task, not done here. Don't treat these as final sound
// design; they exist to prove the mechanism (ComboBox -> apply -> deselect-on-edit).
struct Preset
{
    const char* name;
    std::initializer_list<std::pair<const char*, float>> values; // paramID -> plain value
};

inline const Preset kPresets[] = {
    { "Init",           {} },
    { "Infinite Pad",   { { "loss", 0.0f }, { "feed", 0.3f } } },
    { "Shimmer Freeze", { { "phaseNoiseAmt", 0.4f }, { "revMix", 0.35f } } },
    { "Cathedral",      { { "revMix", 0.5f }, { "revDecay", 0.85f }, { "revSize", 1.6f } } },
    { "Harmonic Drift", { { "harmonize", 0.05f }, { "harmonic", 1.0f } } },
    { "Dark Comb",      { { "shape", 2.0f }, { "shapeLevel", -0.6f }, { "revDamp", 0.7f } } },
    { "Metallic Room",  { { "revMix", 0.4f }, { "revMetal", 0.8f } } },
    { "Feedback Bloom", { { "revFeed", 0.5f }, { "revMix", 0.3f }, { "loss", 0.1f } } },
};
inline constexpr int kNumPresets = (int) (sizeof (kPresets) / sizeof (kPresets[0]));
