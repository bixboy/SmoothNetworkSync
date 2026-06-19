// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SmoothSyncable.h"
#include "SmoothSyncTypes.h"
#include "SmoothProp.generated.h"

class UNetworkInterpolatorComponent;
class UStaticMeshComponent;


UCLASS()
class SMOOTHNETWORKSYNC_API ASmoothProp : public AActor, public ISmoothSyncable
{
    GENERATED_BODY()

public:

    ASmoothProp();

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

    virtual void Tick(float DeltaTime) override;

protected:

    virtual void BeginPlay() override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> MeshComponent = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UNetworkInterpolatorComponent> NetworkInterpolator = nullptr;

private:
    
    FVector InitialLocation;
};
