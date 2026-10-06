// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "Stats/Stats.h"

// Console: "stat SmoothSync"
DECLARE_STATS_GROUP(TEXT("SmoothSync"), STATGROUP_SmoothSync, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Tick (total)"), STAT_SmoothSync_Tick, STATGROUP_SmoothSync, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Send"), STAT_SmoothSync_Send, STATGROUP_SmoothSync, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Interpolate"), STAT_SmoothSync_Interpolate, STATGROUP_SmoothSync, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Owner Correction"), STAT_SmoothSync_OwnerCorrection, STATGROUP_SmoothSync, );

DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Components"), STAT_SmoothSync_Components, STATGROUP_SmoothSync, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Settled (idle)"), STAT_SmoothSync_Settled, STATGROUP_SmoothSync, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Dormant (server)"), STAT_SmoothSync_Dormant, STATGROUP_SmoothSync, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Snapshots Sent"), STAT_SmoothSync_SnapshotsSent, STATGROUP_SmoothSync, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Snapshots Received"), STAT_SmoothSync_SnapshotsReceived, STATGROUP_SmoothSync, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Snaps"), STAT_SmoothSync_Snaps, STATGROUP_SmoothSync, );

namespace SmoothSync
{
    /** Console "smoothsync.Debug 1". */
    bool IsGlobalDebugEnabled();
}
