/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARRIVALSLOTS_H
#define PLAYERBOTS_ARRIVALSLOTS_H

// Arrival time math shared by the weekly plan and ".astro arrivals add". Plain C++ with no AzerothCore
// includes so test/ArrivalSlotsTest.cpp builds it standalone.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace ArrivalSlots
{
using HourPoints = std::vector<std::pair<float, float>>;  // (local hour, weight), sorted by hour

// HourWeights curve: linear between points, wrapping around midnight
inline float HourWeightAt(HourPoints const& points, float hour)
{
    if (points.empty())
        return 1.0f;
    std::pair<float, float> prev = points.back();
    prev.first -= 24.0f;
    std::pair<float, float> next = points.front();
    next.first += 24.0f;
    for (auto const& point : points)
    {
        if (point.first <= hour)
            prev = point;
        else
        {
            next = point;
            break;
        }
    }
    float weight = prev.second;
    if (next.first > prev.first)
        weight += (next.second - prev.second) * (hour - prev.first) / (next.first - prev.first);
    return std::max(0.0f, weight);
}

// Integral of the HourWeights curve over [from, to] hours (trapezoids of at most 3 minutes)
inline float HourMass(HourPoints const& points, float from, float to)
{
    if (to <= from)
        return 0.0f;
    uint32_t const steps = std::max<uint32_t>(1, uint32_t(std::ceil((to - from) / 0.05f)));
    float const width = (to - from) / steps;
    float mass = 0.0f;
    for (uint32_t i = 0; i < steps; ++i)
        mass += (HourWeightAt(points, from + i * width) + HourWeightAt(points, from + (i + 1) * width)) * 0.5f * width;
    return mass;
}

// `count` times in [start, end), sorted, each drawn with density proportional to the HourWeights curve at its
// local hour (`localHour(time)` gives the population clock's hour in [0, 24)). A window with no weight at all
// is filled uniformly.
template <class LocalHourFn, class Rng>
std::vector<uint32_t> DrawWindow(uint32_t start, uint32_t end, uint32_t count, HourPoints const& points,
                                 LocalHourFn localHour, Rng& rng)
{
    std::vector<uint32_t> times;
    times.reserve(count);
    if (!count)
        return times;
    if (end <= start + 1)
    {
        times.assign(count, start);
        return times;
    }

    float maxWeight = points.empty() ? 1.0f : 0.0f;
    for (auto const& point : points)
        maxWeight = std::max(maxWeight, point.second);

    std::uniform_real_distribution<double> unit(0.0, 1.0);
    double const span = double(end - start);
    auto uniformTime = [&]() { return std::min<uint32_t>(end - 1, start + uint32_t(unit(rng) * span)); };

    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t at = uniformTime();
        if (maxWeight > 0.0f)
        {
            bool accepted = false;
            for (uint32_t attempt = 0; attempt < 1000 && !accepted; ++attempt)
            {
                at = uniformTime();
                accepted = unit(rng) * maxWeight <= HourWeightAt(points, localHour(at));
            }
        }
        times.push_back(at);
    }
    std::sort(times.begin(), times.end());
    return times;
}

// Largest number of times inside any one hour [t, t + 3600)
inline uint32_t PeakPerHour(std::vector<uint32_t> times)
{
    std::sort(times.begin(), times.end());
    uint32_t peak = 0;
    size_t first = 0;
    for (size_t last = 0; last < times.size(); ++last)
    {
        while (times[last] >= times[first] + 3600)
            ++first;
        peak = std::max<uint32_t>(peak, uint32_t(last - first + 1));
    }
    return peak;
}

// Re-planning the rest of a week: pending planned rows are moved to the new times first, then rows are
// added or dropped to reach the target. Done rows are never touched.
struct Replan
{
    uint32_t reschedule = 0;
    uint32_t insert = 0;
    uint32_t drop = 0;
};

inline Replan PlanDelta(uint32_t pending, uint32_t target)
{
    Replan r;
    r.reschedule = std::min(pending, target);
    r.insert = target > pending ? target - pending : 0;
    r.drop = pending > target ? pending - target : 0;
    return r;
}
}  // namespace ArrivalSlots

#endif
