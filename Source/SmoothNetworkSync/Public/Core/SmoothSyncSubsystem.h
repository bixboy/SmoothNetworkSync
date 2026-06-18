#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SmoothSyncSubsystem.generated.h"

class UNetworkInterpolatorComponent;


UCLASS()
class SMOOTHNETWORKSYNC_API USmoothSyncSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    USmoothSyncSubsystem();

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    virtual void Deinitialize() override;

    virtual void Tick(float DeltaTime) override;

    virtual TStatId GetStatId() const override;

    
    /**
     * @brief Registers a component for the flat array optimization.
     * @param InComponent The component to register.
     */
    void RegisterComponent(UNetworkInterpolatorComponent* InComponent);

    /**
     * @brief Unregisters a component.
     * @param InComponent The component to unregister.
     */
    void UnregisterComponent(UNetworkInterpolatorComponent* InComponent);

protected:

    UPROPERTY()
    TArray<TObjectPtr<UNetworkInterpolatorComponent>> ActiveComponents;

private:

    /** 
     * @brief Internal helper to perform cubic hermite evaluation and fallback extrapolation.
     * @param InComponent The target component providing the buffer.
     * @param InTargetTime The target game time adjusted by InterpolationDelay.
     * @param OutTransform Output computed transform.
     * @param OutVelocity Output computed velocity.
     */
    void EvaluateDualState(UNetworkInterpolatorComponent* InComponent, float InTargetTime, FTransform& OutTransform, FVector& OutVelocity, FVector& OutScale) const;

    /** 
     * @brief Handles client-side prediction and soft correction for Autonomous Proxies.
     */
    void HandleAutonomousProxyPrediction(AActor* OwnerActor, UNetworkInterpolatorComponent* Comp, float DeltaTime, float CurrentLocalTime) const;
};
