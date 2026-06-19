// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SmoothSyncable.generated.h"


/**
 * @brief Blueprintable Interface for any Actor or Component that should receive smooth network interpolation.
 */
UINTERFACE(MinimalAPI, Blueprintable)
class USmoothSyncable : public UInterface
{
    GENERATED_BODY()
};


/**
 * @brief Interface class containing the execution methods for Smooth Sync.
 */
class SMOOTHNETWORKSYNC_API ISmoothSyncable
{
    GENERATED_BODY()

public:

    /**
     * @brief Called by the SmoothSyncSubsystem every frame to apply the newly calculated interpolated transform.
     * @param InNewTransform The newly computed FTransform.
     * @param InNewVelocity The newly computed interpolated or extrapolated FVector velocity.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Smooth Sync")
    void ApplySmoothedTransform(const FTransform& InNewTransform, const FVector& InNewVelocity);
};
