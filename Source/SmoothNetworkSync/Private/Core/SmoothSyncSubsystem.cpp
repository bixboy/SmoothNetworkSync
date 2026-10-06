// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "Core/SmoothSyncSubsystem.h"
#include "Components/SmoothSyncComponent.h"
#include "SmoothSyncStats.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

static TAutoConsoleVariable<int32> CVarSmoothSyncDebug(
    TEXT("smoothsync.Debug"),
    0,
    TEXT("Draw Smooth Sync debug info for every component, plus an on-screen summary.\n")
    TEXT("0: Off, 1: On"),
    ECVF_Cheat
);

bool SmoothSync::IsGlobalDebugEnabled()
{
    return CVarSmoothSyncDebug.GetValueOnGameThread() > 0;
}


bool USmoothSyncSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USmoothSyncSubsystem::Deinitialize()
{
    for (USmoothSyncComponent* Comp : ActiveComponents)
    {
        if (Comp)
            Comp->SubsystemIndex = INDEX_NONE;
    }

    ActiveComponents.Reset();
    Super::Deinitialize();
}


// ===== Registration =====

void USmoothSyncSubsystem::RegisterComponent(USmoothSyncComponent* InComponent)
{
    if (!IsValid(InComponent) || InComponent->SubsystemIndex != INDEX_NONE)
        return;

    InComponent->SubsystemIndex = ActiveComponents.Add(InComponent);
}

void USmoothSyncSubsystem::UnregisterComponent(USmoothSyncComponent* InComponent)
{
    if (!InComponent)
        return;

    const int32 Index = InComponent->SubsystemIndex;
    if (!ActiveComponents.IsValidIndex(Index) || ActiveComponents[Index] != InComponent)
        return;

    InComponent->SubsystemIndex = INDEX_NONE;

    if (bIsTicking)
    {
        ActiveComponents[Index] = nullptr;
        bNeedsCompaction = true;
        return;
    }

    ActiveComponents.RemoveAtSwap(Index);
    if (ActiveComponents.IsValidIndex(Index) && ActiveComponents[Index])
        ActiveComponents[Index]->SubsystemIndex = Index;
}

void USmoothSyncSubsystem::CompactComponents()
{
    int32 WriteIndex = 0;
    for (int32 ReadIndex = 0; ReadIndex < ActiveComponents.Num(); ++ReadIndex)
    {
        USmoothSyncComponent* Comp = ActiveComponents[ReadIndex];
        if (!Comp)
            continue;

        Comp->SubsystemIndex = WriteIndex;
        ActiveComponents[WriteIndex++] = Comp;
    }

    ActiveComponents.SetNum(WriteIndex);
    bNeedsCompaction = false;
}


// ===== Tick =====

void USmoothSyncSubsystem::BuildFrameContext(UWorld* InWorld, float InDeltaTime)
{
    FrameContext.World = InWorld;
    FrameContext.DeltaTime = InDeltaTime;
    FrameContext.bDrawDebug = SmoothSync::IsGlobalDebugEnabled();
    FrameContext.bIsServer = InWorld->GetNetMode() != NM_Client;

    const AGameStateBase* GameState = InWorld->GetGameState();
    FrameContext.ServerTime = GameState ? GameState->GetServerWorldTimeSeconds() : InWorld->GetTimeSeconds();

    FrameContext.ViewerLocations.Reset();
    FrameContext.LocalPawn = nullptr;
    FrameContext.RoundTripTime = 0.0;

    if (FrameContext.bIsServer)
    {
        for (FConstPlayerControllerIterator It = InWorld->GetPlayerControllerIterator(); It; ++It)
        {
            const APlayerController* PC = It->Get();
            if (!PC)
                continue;

            if (const APawn* Pawn = PC->GetPawn())
                FrameContext.ViewerLocations.Add(Pawn->GetActorLocation());
            else if (const AActor* ViewTarget = PC->GetViewTarget())
                FrameContext.ViewerLocations.Add(ViewTarget->GetActorLocation());
        }
        return;
    }

    if (const APlayerController* LocalPC = InWorld->GetFirstPlayerController())
    {
        FrameContext.LocalPawn = LocalPC->GetPawn();
        if (FrameContext.LocalPawn)
            FrameContext.LocalPawnLocation = FrameContext.LocalPawn->GetActorLocation();

        if (const APlayerState* PlayerState = LocalPC->PlayerState)
            FrameContext.RoundTripTime = PlayerState->GetPingInMilliseconds() / 1000.0;
    }
}

void USmoothSyncSubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (ActiveComponents.IsEmpty())
        return;

    UWorld* World = GetWorld();
    if (!IsValid(World) || World->GetNetMode() == NM_Standalone)
        return;

    SCOPE_CYCLE_COUNTER(STAT_SmoothSync_Tick);

    BuildFrameContext(World, DeltaTime);

    int32 NumSettled = 0;
    int32 NumDormant = 0;
    float TotalBytesPerSecond = 0.0f;

    // Components registered during the loop are processed next frame.
    bIsTicking = true;
    const int32 NumComponents = ActiveComponents.Num();
    for (int32 i = 0; i < NumComponents; ++i)
    {
        USmoothSyncComponent* Comp = ActiveComponents[i];
        if (!Comp)
            continue;

        if (!IsValid(Comp))
        {
            Comp->SubsystemIndex = INDEX_NONE;
            ActiveComponents[i] = nullptr;
            bNeedsCompaction = true;
            continue;
        }

        Comp->SmoothSyncTick(FrameContext);

        NumSettled += Comp->bIsSettled ? 1 : 0;
        NumDormant += Comp->bIsDormant ? 1 : 0;
        TotalBytesPerSecond += Comp->DebugBytesPerSecond;
    }
    bIsTicking = false;

    if (bNeedsCompaction)
        CompactComponents();

    SET_DWORD_STAT(STAT_SmoothSync_Components, ActiveComponents.Num());
    SET_DWORD_STAT(STAT_SmoothSync_Settled, NumSettled);
    SET_DWORD_STAT(STAT_SmoothSync_Dormant, NumDormant);

    if (FrameContext.bDrawDebug)
        DrawDebugSummary(NumSettled, NumDormant, TotalBytesPerSecond);
}

void USmoothSyncSubsystem::DrawDebugSummary(int32 InNumSettled, int32 InNumDormant, float InTotalBytesPerSecond) const
{
    if (!GEngine)
        return;

    const FString Summary = FString::Printf(
        TEXT("Smooth Sync [%s]  components: %d  idle: %d  dormant: %d  %s: %.1f KB/s  RTT: %.0f ms"),
        FrameContext.bIsServer ? TEXT("server") : TEXT("client"),
        ActiveComponents.Num(), InNumSettled, InNumDormant,
        FrameContext.bIsServer ? TEXT("sent") : TEXT("received"),
        InTotalBytesPerSecond / 1024.0f,
        FrameContext.RoundTripTime * 1000.0);

    // One line per world (server and clients share the screen in PIE).
    const uint64 Key = 0x5300000 + (FrameContext.bIsServer ? 0 : 1);
    GEngine->AddOnScreenDebugMessage(Key, 0.0f, FrameContext.bIsServer ? FColor::Yellow : FColor::Cyan, Summary);
}

TStatId USmoothSyncSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USmoothSyncSubsystem, STATGROUP_Tickables);
}
