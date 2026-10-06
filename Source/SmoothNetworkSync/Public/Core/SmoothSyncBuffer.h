// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"


/**
 * Fixed-capacity ring buffer of network states (no allocation).
 * Index 0 is the newest state, Num() - 1 the oldest. StateType must expose a double ServerTimestamp.
 */
template<typename StateType, int32 Capacity>
class TSmoothSyncStateBuffer
{
public:

    int32 Num() const { return Count; }
    bool IsEmpty() const { return Count == 0; }

    const StateType& Get(int32 InIndexFromNewest) const
    {
        checkSlow(InIndexFromNewest >= 0 && InIndexFromNewest < Count);
        int32 Index = Head - InIndexFromNewest;
        if (Index < 0)
            Index += Capacity;

        return Items[Index];
    }

    /** For in-place conversions of the buffered states (e.g. to another coordinate space). */
    StateType& GetMutable(int32 InIndexFromNewest)
    {
        return const_cast<StateType&>(Get(InIndexFromNewest));
    }

    const StateType& Newest() const { return Get(0); }
    const StateType& Oldest() const { return Get(Count - 1); }

    void Push(const StateType& InState)
    {
        Head = (Head + 1) % Capacity;
        Items[Head] = InState;
        Count = FMath::Min(Count + 1, Capacity);
    }

    void Reset()
    {
        Head = -1;
        Count = 0;
    }

    /** Finds the two consecutive states surrounding InTime. Returns false if InTime is outside the buffered range. */
    bool FindSegment(double InTime, const StateType*& OutOlder, const StateType*& OutNewer) const
    {
        for (int32 i = 0; i < Count - 1; ++i)
        {
            const StateType& Newer = Get(i);
            const StateType& Older = Get(i + 1);

            if (InTime >= Older.ServerTimestamp && InTime <= Newer.ServerTimestamp)
            {
                OutOlder = &Older;
                OutNewer = &Newer;
                return true;
            }
        }
        return false;
    }

private:

    StateType Items[Capacity];
    int32 Head = -1;
    int32 Count = 0;
};


/**
 * Measures how snapshots arrive (age against the estimated server clock, and spacing between them)
 * to size the interpolation delay: just enough to always have a newer snapshot to interpolate toward.
 */
struct FSmoothSyncArrivalStats
{
    /** Gaps longer than this are "object was at rest", not network timing. */
    static constexpr double MaxMeasuredInterval = 1.0;

    void AddSample(double InAge, double InIntervalSincePrevious)
    {
        if (!bHasSamples)
        {
            MeanAge = InAge;
            AgeDeviation = 0.0;
            bHasSamples = true;
        }
        else
        {
            const double Diff = InAge - MeanAge;
            MeanAge += Diff * SmoothingFactor;
            AgeDeviation += (FMath::Abs(Diff) - AgeDeviation) * SmoothingFactor;
        }

        if (InIntervalSincePrevious > 0.0 && InIntervalSincePrevious < MaxMeasuredInterval)
        {
            MeanInterval = bHasInterval ? MeanInterval + (InIntervalSincePrevious - MeanInterval) * SmoothingFactor : InIntervalSincePrevious;
            bHasInterval = true;
        }
    }

    /** The next snapshot can be up to one interval away, plus its arrival jitter. */
    bool HasEstimate() const { return bHasInterval; }
    double GetRequiredDelay() const { return MeanInterval + MeanAge + 2.0 * AgeDeviation; }

    /** Segments longer than this are treated as "rest, then move" rather than one long slow motion. */
    double GetMaxSegmentDuration() const { return bHasInterval ? FMath::Max(0.25, MeanInterval * 3.0) : 0.25; }

    void Reset() { *this = FSmoothSyncArrivalStats(); }

private:

    static constexpr double SmoothingFactor = 0.1;

    double MeanAge = 0.0;
    double AgeDeviation = 0.0;
    double MeanInterval = 0.0;
    bool bHasSamples = false;
    bool bHasInterval = false;
};
