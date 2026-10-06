// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothNetworkSync.h"
#include "SmoothSyncLog.h"
#include "SmoothSyncStats.h"

DEFINE_LOG_CATEGORY(LogSmoothSync);

DEFINE_STAT(STAT_SmoothSync_Tick);
DEFINE_STAT(STAT_SmoothSync_Send);
DEFINE_STAT(STAT_SmoothSync_Interpolate);
DEFINE_STAT(STAT_SmoothSync_OwnerCorrection);
DEFINE_STAT(STAT_SmoothSync_Components);
DEFINE_STAT(STAT_SmoothSync_Settled);
DEFINE_STAT(STAT_SmoothSync_Dormant);
DEFINE_STAT(STAT_SmoothSync_SnapshotsSent);
DEFINE_STAT(STAT_SmoothSync_SnapshotsReceived);
DEFINE_STAT(STAT_SmoothSync_Snaps);


void FSmoothNetworkSyncModule::StartupModule()
{
}

void FSmoothNetworkSyncModule::ShutdownModule()
{
}


IMPLEMENT_MODULE(FSmoothNetworkSyncModule, SmoothNetworkSync)
