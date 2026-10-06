// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interfaces/SmoothSyncable.h"
#include "SmoothPhysicsProp.generated.h"

class USmoothSyncComponent;
class UStaticMeshComponent;


UCLASS()
class SMOOTHNETWORKSYNCEXAMPLES_API ASmoothPhysicsProp : public AActor, public ISmoothSyncable
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
    TObjectPtr<USmoothSyncComponent> NetworkInterpolator = nullptr;

};
