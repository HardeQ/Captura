#pragma once

#include <algorithm>
#include <iterator>

struct Rgb
{
    float r, g, b;
};

// One color theme. The first group drives the animated background shader, the
// second the menus and on-screen display.
struct Palette
{
    const wchar_t* name;
    Rgb bgLow;   // background, bottom of the screen
    Rgb bgHigh;  // background, top of the screen
    Rgb glow;    // horizon glow, fog and floor reflection
    Rgb light;   // light towers and particles
    Rgb accent;  // highlights, topographic lines, sliders
    Rgb text;
    Rgb dim;     // secondary text
    Rgb live;    // "live" / success indicators
    Rgb panel;   // translucent panels
};

inline constexpr Palette kPalettes[] = {
    {L"Orchid", {0.040f, 0.010f, 0.058f}, {0.150f, 0.036f, 0.215f}, {0.27f, 0.07f, 0.31f}, {0.92f, 0.42f, 1.00f},
     {1.00f, 0.56f, 0.93f}, {1.00f, 0.94f, 1.00f}, {0.80f, 0.64f, 0.90f}, {0.55f, 1.00f, 0.78f},
     {0.10f, 0.02f, 0.16f}},
    {L"Neon Rose", {0.050f, 0.008f, 0.030f}, {0.200f, 0.030f, 0.120f}, {0.34f, 0.06f, 0.22f}, {1.00f, 0.38f, 0.70f},
     {1.00f, 0.45f, 0.72f}, {1.00f, 0.94f, 0.97f}, {0.92f, 0.62f, 0.78f}, {0.55f, 1.00f, 0.78f},
     {0.14f, 0.02f, 0.09f}},
    {L"Ultraviolet", {0.020f, 0.012f, 0.060f}, {0.075f, 0.045f, 0.230f}, {0.14f, 0.09f, 0.36f}, {0.62f, 0.50f, 1.00f},
     {0.74f, 0.64f, 1.00f}, {0.95f, 0.94f, 1.00f}, {0.70f, 0.66f, 0.92f}, {0.55f, 1.00f, 0.78f},
     {0.05f, 0.03f, 0.17f}},
    {L"Midnight", {0.010f, 0.016f, 0.045f}, {0.035f, 0.065f, 0.160f}, {0.05f, 0.09f, 0.22f}, {0.30f, 0.55f, 1.00f},
     {0.40f, 0.70f, 1.00f}, {0.88f, 0.93f, 1.00f}, {0.55f, 0.66f, 0.86f}, {0.45f, 1.00f, 0.65f},
     {0.02f, 0.05f, 0.14f}},
    {L"Aurora", {0.006f, 0.030f, 0.035f}, {0.020f, 0.110f, 0.130f}, {0.05f, 0.26f, 0.26f}, {0.30f, 1.00f, 0.80f},
     {0.45f, 1.00f, 0.85f}, {0.92f, 1.00f, 0.98f}, {0.60f, 0.85f, 0.80f}, {0.95f, 1.00f, 0.50f},
     {0.01f, 0.09f, 0.10f}},
    {L"Ember", {0.050f, 0.012f, 0.008f}, {0.200f, 0.055f, 0.025f}, {0.34f, 0.12f, 0.04f}, {1.00f, 0.55f, 0.25f},
     {1.00f, 0.66f, 0.35f}, {1.00f, 0.96f, 0.92f}, {0.92f, 0.72f, 0.58f}, {0.55f, 1.00f, 0.70f},
     {0.15f, 0.04f, 0.02f}},
};

inline constexpr int kPaletteCount = static_cast<int>(std::size(kPalettes));

inline const Palette& GetPalette(int index)
{
    return kPalettes[std::clamp(index, 0, kPaletteCount - 1)];
}
