// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "SmoothSyncCharacterMovementComponent.generated.h"


/**
 * CharacterMovementComponent for server-authoritative players that push physics objects.
 *
 * Client and server never collide with a physics object at exactly the same place, so the stock CMC corrects
 * the owning client on nearly every frame of contact (its tolerance is ~1.7 units), which shows as jitter.
 * While the character touches a simulating body, this component tolerates a bounded error and keeps the client's
 * position instead. Outside of contacts, corrections are as strict as the stock CMC.
 *
 * Use it in your Character's constructor:
 *   AMyCharacter(const FObjectInitializer& ObjectInitializer)
 *       : Super(ObjectInitializer.SetDefaultSubobjectClass<USmoothSyncCharacterMovementComponent>(ACharacter::CharacterMovementComponentName)) {}
 */
UCLASS(ClassGroup = (SmoothSync), meta = (BlueprintSpawnableComponent))
class SMOOTHNETWORKSYNC_API USmoothSyncCharacterMovementComponent : public UCharacterMovementComponent
{
    GENERATED_BODY()

public:

    /** Position error (units) the server accepts from the owning client while it touches a physics object. 0 disables. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (Networking)|Smooth Sync", meta = (ClampMin = "0", UIMax = "200"))
    float PhysicsContactErrorTolerance = 50.0f;

    /** How long (seconds) after the last physics contact the tolerance still applies. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (Networking)|Smooth Sync", meta = (ClampMin = "0", UIMax = "1"))
    float PhysicsContactGraceTime = 0.3f;

    /** True while the character touches (or just touched) a simulating body. */
    UFUNCTION(BlueprintPure, Category = "Character Movement (Networking)|Smooth Sync")
    bool IsInPhysicsContact() const;

    /**
     * Owning client: how far server corrections moved the character since the last call (exact, measured around the
     * correction replay). Used by Smooth Sync to slide the visuals instead of popping.
     */
    FVector ConsumeCorrectionDelta();

protected:

    virtual void HandleImpact(const FHitResult& Hit, float TimeSlice = 0.f, const FVector& MoveDelta = FVector::ZeroVector) override;

    virtual bool ClientUpdatePositionAfterServerUpdate() override;

    virtual void OnClientCorrectionReceived(FNetworkPredictionData_Client_Character& ClientData, float TimeStamp, FVector NewLocation, FVector NewVelocity,
        UPrimitiveComponent* NewBase, FName NewBaseBoneName, bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode, FVector ServerGravityDirection) override;

    virtual bool ServerExceedsAllowablePositionError(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& ClientWorldLocation,
        const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode) override;

    virtual bool ServerShouldUseAuthoritativePosition(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& ClientWorldLocation,
        const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode) override;

private:

    double LastPhysicsContactTime = -1.0;
    FVector PendingCorrectionDelta = FVector::ZeroVector;

    /** Where the client was before a received correction snapped it (the replay runs on the next tick). */
    FVector LocationBeforeCorrection = FVector::ZeroVector;
    bool bHasLocationBeforeCorrection = false;
};
