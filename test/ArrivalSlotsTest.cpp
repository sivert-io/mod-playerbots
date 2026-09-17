// Standalone test for src/Bot/ArrivalSlots.h (not part of the module build).
//   c++ -std=c++17 -I src/Bot test/ArrivalSlotsTest.cpp -o /tmp/arrival-slots-test && /tmp/arrival-slots-test

#include "ArrivalSlots.h"

#include <cstdio>
#include <string>

using namespace ArrivalSlots;

static int failures = 0;

static void Expect(bool ok, std::string const& name)
{
    if (!ok)
        ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", name.c_str());
}

static constexpr uint32_t T0 = 1789336800;  // a Monday 00:00 in the test clock (UTC, no offset)
static float LocalHour(uint32_t t) { return float((t - T0) % 86400) / 3600.0f; }

// The live AiPlayerbot.Arrivals.HourWeights
static HourPoints const Live = {{0, 3}, {3, 1}, {7, 2}, {10, 4}, {13, 6}, {16, 8}, {19, 10}, {21, 9}, {23, 5}};

static void Basics()
{
    std::mt19937 rng(42);
    Expect(DrawWindow(T0, T0 + 3600, 0, Live, LocalHour, rng).empty(), "zero count gives no slots");

    auto const same = DrawWindow(T0 + 50, T0 + 50, 3, Live, LocalHour, rng);
    Expect(same.size() == 3 && same[0] == T0 + 50 && same[2] == T0 + 50, "empty window puts every slot at its start");

    uint32_t const start = T0 + 5 * 3600, end = start + 36 * 3600;
    auto const times = DrawWindow(start, end, 800, Live, LocalHour, rng);
    bool inside = true, sorted = true;
    for (size_t i = 0; i < times.size(); ++i)
    {
        inside &= times[i] >= start && times[i] < end;
        sorted &= !i || times[i - 1] <= times[i];
    }
    Expect(times.size() == 800, "800 slots drawn");
    Expect(inside, "all slots inside [start, end)");
    Expect(sorted, "slots sorted");
}

static void Weighting()
{
    std::mt19937 rng(7);
    // Whole days so every hour is covered equally often
    auto const times = DrawWindow(T0, T0 + 7 * 86400, 20000, Live, LocalHour, rng);
    uint32_t evening = 0, night = 0;  // 18-22 (weight ~9-10) vs 2-6 (weight ~1-2)
    for (uint32_t t : times)
    {
        float const h = LocalHour(t);
        evening += h >= 18.0f && h < 22.0f;
        night += h >= 2.0f && h < 6.0f;
    }
    // Expected ratio is the curve's mass ratio (~5.8x)
    float const expected = HourMass(Live, 18, 22) / HourMass(Live, 2, 6);
    float const ratio = float(evening) / float(std::max<uint32_t>(1, night));
    Expect(ratio > expected * 0.85f && ratio < expected * 1.15f,
           "evening/night share follows HourWeights (" + std::to_string(ratio) + " vs " + std::to_string(expected) + ")");

    // Zero weight hours get nothing
    HourPoints const office = {{0, 0}, {8, 0}, {9, 10}, {17, 10}, {18, 0}};
    auto const work = DrawWindow(T0, T0 + 3 * 86400, 3000, office, LocalHour, rng);
    bool outside = false;
    for (uint32_t t : work)
        outside |= LocalHour(t) < 8.0f || LocalHour(t) > 18.0f;
    Expect(!outside, "no slots where the curve is 0");

    // No weight anywhere: uniform fallback, still inside the window
    HourPoints const zero = {{0, 0}, {12, 0}};
    auto const flat = DrawWindow(T0, T0 + 86400, 2400, zero, LocalHour, rng);
    uint32_t firstHalf = 0;
    for (uint32_t t : flat)
        firstHalf += t < T0 + 43200;
    Expect(flat.size() == 2400 && firstHalf > 1000 && firstHalf < 1400, "all-zero curve falls back to uniform");
}

static void Peaks()
{
    Expect(PeakPerHour({}) == 0, "no times, no peak");
    Expect(PeakPerHour({T0, T0 + 3599, T0 + 3600}) == 2, "an hour is [t, t + 3600)");
    Expect(PeakPerHour({T0 + 7200, T0, T0 + 10, T0 + 7300, T0 + 7400}) == 3, "peak found in unsorted input");

    // 800 over 36 h with the live curve: average 22/h, the evening peak roughly twice that
    std::mt19937 rng(3);
    uint32_t const start = T0 + 9 * 3600;  // 09:00
    auto const burst = DrawWindow(start, start + 36 * 3600, 800, Live, LocalHour, rng);
    uint32_t const peak = PeakPerHour(burst);
    std::printf("     800 over 36h from 09:00: busiest hour %u\n", peak);
    Expect(peak >= 30 && peak <= 60, "burst peak stays well under MaxPerHour 60");
}

static void Replanning()
{
    Replan const grow = PlanDelta(51, 70);
    Expect(grow.reschedule == 51 && grow.insert == 19 && grow.drop == 0, "higher budget moves all and adds the rest");
    Replan const shrink = PlanDelta(51, 20);
    Expect(shrink.reschedule == 20 && shrink.insert == 0 && shrink.drop == 31, "lower budget moves some and drops the rest");
    Replan const same = PlanDelta(51, 51);
    Expect(same.reschedule == 51 && !same.insert && !same.drop, "same budget only moves rows (idempotent count)");
    Replan const none = PlanDelta(0, 0);
    Expect(!none.reschedule && !none.insert && !none.drop, "nothing to do");
}

int main()
{
    Basics();
    Weighting();
    Peaks();
    Replanning();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
