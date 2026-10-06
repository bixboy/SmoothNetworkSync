// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SmoothSyncSubsystem.generated.h"

class APawn;
class USmoothSyncComponent;


/** Per-frame data shared by every Smooth Sync component, computed once by the subsystem. */
struct FSmoothSyncFrameContext
{
    UWorld* World = nullptr;
    float DeltaTime = 0.0f;

    /** Server time on the server, best estimate of it on clients. */
    double ServerTime = 0.0;

    bool bIsServer = false;

    /** Server only: locations of every player's pawn or view target, used for distance-based rate reduction. */
    TArray<FVector, TInlineAllocator<16>> ViewerLocations;

    /** Client only: the locally controlled pawn, used to display nearby objects in the player's present. */
    const APawn* LocalPawn = nullptr;
    FVector LocalPawnLocation = FVector::ZeroVector;

    /** Client only: round trip time to the server, in seconds. */
    double RoundTripTime = 0.0;

    bool bDrawDebug = false;
};


/**
 * Ticks every Smooth Sync component of the world in a single loop (no per-component tick function).
 * Created for game and PIE worlds only, idle in standalone.
 */
UCLASS()
class SMOOTHNETWORKSYNC_API USmoothSyncSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:

    virtual void Deinitialize() override;

    virtual void Tick(float DeltaTime) override;

    virtual TStatId GetStatId() const override;

    void RegisterComponent(USmoothSyncComponent* InComponent);

    void UnregisterComponent(USmoothSyncComponent* InComponent);

    int32 GetNumRegisteredComponents() const { return ActiveComponents.Num(); }

protected:

    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:

    void BuildFrameContext(UWorld* InWorld, float InDeltaTime);

    void CompactComponents();

    void DrawDebugSummary(int32 InNumSettled, int32 InNumDormant, float InTotalBytesPerSecond) const;

    UPROPERTY(Transient)
    TArray<TObjectPtr<USmoothSyncComponent>> ActiveComponents;

    FSmoothSyncFrameContext FrameContext;

    /** Components can be (un)registered from gameplay events fired during the loop. Removals are deferred. */
    bool bIsTicking = false;
    bool bNeedsCompaction = false;
};
