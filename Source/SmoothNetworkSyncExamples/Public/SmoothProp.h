// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interfaces/SmoothSyncable.h"
#include "SmoothProp.generated.h"

class USmoothSyncComponent;
class UStaticMeshComponent;


UCLASS()
class SMOOTHNETWORKSYNCEXAMPLES_API ASmoothProp : public AActor, public ISmoothSyncable
{
    GENERATED_BODY()

public:

    ASmoothProp();

    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

    virtual void Tick(float DeltaTime) override;

protected:

    virtual void BeginPlay() override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> MeshComponent = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<USmoothSyncComponent> NetworkInterpolator = nullptr;

private:
    
    FVector InitialLocation;
};
