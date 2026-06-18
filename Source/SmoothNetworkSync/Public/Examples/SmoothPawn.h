#pragma once
#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "SmoothSyncable.h"
#include "SmoothSyncTypes.h"
#include "SmoothPawn.generated.h"

class UNetworkInterpolatorComponent;


UCLASS()
class SMOOTHNETWORKSYNC_API ASmoothPawn : public ADefaultPawn, public ISmoothSyncable
{
    GENERATED_BODY()

public:
    ASmoothPawn();

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

protected:

    virtual void BeginPlay() override;

    virtual void Tick(float DeltaTime) override;
    

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UNetworkInterpolatorComponent> NetworkInterpolator = nullptr;

};
