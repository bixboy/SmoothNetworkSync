#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SmoothSyncable.h"
#include "SmoothSyncTypes.h"
#include "SmoothScaleProp.generated.h"

class UNetworkInterpolatorComponent;
class UStaticMeshComponent;


UCLASS()
class SMOOTHNETWORKSYNC_API ASmoothScaleProp : public AActor, public ISmoothSyncable
{
    GENERATED_BODY()

public:

    ASmoothScaleProp();

    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

    virtual void Tick(float DeltaTime) override;

protected:

    virtual void BeginPlay() override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> MeshComponent = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UNetworkInterpolatorComponent> NetworkInterpolator = nullptr;

private:
    
    /** The base scale to pulsate from. */
    FVector BaseScale;

    /** Time tracker for the sine wave. */
    float RunningTime = 0.0f;
};
