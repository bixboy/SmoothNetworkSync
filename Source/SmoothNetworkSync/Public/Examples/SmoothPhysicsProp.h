// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SmoothSyncable.h"
#include "SmoothPhysicsProp.generated.h"

class UNetworkInterpolatorComponent;
class UStaticMeshComponent;


UCLASS()
class SMOOTHNETWORKSYNC_API ASmoothPhysicsProp : public AActor, public ISmoothSyncable
{
    GENERATED_BODY()

public:

    ASmoothPhysicsProp();

    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

protected:

    virtual void BeginPlay() override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> MeshComponent = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UNetworkInterpolatorComponent> NetworkInterpolator = nullptr;

};
