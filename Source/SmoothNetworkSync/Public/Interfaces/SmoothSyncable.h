// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SmoothSyncable.generated.h"


/**
 * Optional interface for actors owning a Smooth Sync component.
 * Implement it to apply the network transform yourself (e.g. move only a visual mesh, drive physics, feed animation).
 * Without it, the component calls SetActorTransform (and sets the physics velocity on simulating roots).
 */
UINTERFACE(MinimalAPI, Blueprintable)
class USmoothSyncable : public UInterface
{
    GENERATED_BODY()
};


class SMOOTHNETWORKSYNC_API ISmoothSyncable
{
    GENERATED_BODY()

public:

    /**
     * Called on the actor every time Smooth Sync wants to move it:
     * every frame on simulated proxies, and on the server when a client-authoritative owner sends its state.
     * Bind the component's OnHardSnapTriggered event to know when the move is a teleport.
     * @param InNewTransform The smoothed world transform (scale is the actor's current scale unless bSyncScale is on).
     * @param InNewVelocity The interpolated or extrapolated linear velocity.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Smooth Sync")
    void ApplySmoothedTransform(const FTransform& InNewTransform, const FVector& InNewVelocity);
};
