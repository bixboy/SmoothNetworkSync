// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "Components/SmoothSyncCharacterMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"


bool USmoothSyncCharacterMovementComponent::IsInPhysicsContact() const
{
    if (PhysicsContactErrorTolerance <= 0.0f)
        return false;

    // Standing on a physics object counts as a contact too.
    const UPrimitiveComponent* Floor = CurrentFloor.HitResult.GetComponent();
    if (CurrentFloor.bBlockingHit && Floor && Floor->IsSimulatingPhysics())
        return true;

    const UWorld* World = GetWorld();
    return World && LastPhysicsContactTime >= 0.0 && World->GetTimeSeconds() - LastPhysicsContactTime <= PhysicsContactGraceTime;
}

FVector USmoothSyncCharacterMovementComponent::ConsumeCorrectionDelta()
{
    const FVector Delta = PendingCorrectionDelta;
    PendingCorrectionDelta = FVector::ZeroVector;
    return Delta;
}

void USmoothSyncCharacterMovementComponent::OnClientCorrectionReceived(FNetworkPredictionData_Client_Character& ClientData, float TimeStamp,
    FVector NewLocation, FVector NewVelocity, UPrimitiveComponent* NewBase, FName NewBaseBoneName, bool bHasBase, bool bBaseRelativePosition,
    uint8 ServerMovementMode, FVector ServerGravityDirection)
{
    // Called right before the correction snaps the character to the server's (past) position.
    // Several corrections can arrive before the replay: keep the location from before the first one.
    if (!bHasLocationBeforeCorrection && UpdatedComponent)
    {
        LocationBeforeCorrection = UpdatedComponent->GetComponentLocation();
        bHasLocationBeforeCorrection = true;
    }

    Super::OnClientCorrectionReceived(ClientData, TimeStamp, NewLocation, NewVelocity, NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition,
        ServerMovementMode, ServerGravityDirection);
}

bool USmoothSyncCharacterMovementComponent::ClientUpdatePositionAfterServerUpdate()
{
    // The replay brings the snapped character back to the present: after the replay minus before the snap is
    // exactly the jump the correction caused.
    const bool bReplayed = Super::ClientUpdatePositionAfterServerUpdate();
    if (bHasLocationBeforeCorrection && UpdatedComponent)
    {
        PendingCorrectionDelta += UpdatedComponent->GetComponentLocation() - LocationBeforeCorrection;
        bHasLocationBeforeCorrection = false;
    }

    return bReplayed;
}

void USmoothSyncCharacterMovementComponent::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
    Super::HandleImpact(Hit, TimeSlice, MoveDelta);

    const UPrimitiveComponent* HitComponent = Hit.GetComponent();
    if (HitComponent && HitComponent->IsSimulatingPhysics())
    {
        if (const UWorld* World = GetWorld())
            LastPhysicsContactTime = World->GetTimeSeconds();
    }
}

bool USmoothSyncCharacterMovementComponent::ServerExceedsAllowablePositionError(float ClientTimeStamp, float DeltaTime, const FVector& Accel,
    const FVector& ClientWorldLocation, const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode)
{
    // Movement mode mismatches and errors outside of contacts keep the stock (strict) behavior.
    if (!IsInPhysicsContact() || PackNetworkMovementMode() != ClientMovementMode)
    {
        return Super::ServerExceedsAllowablePositionError(ClientTimeStamp, DeltaTime, Accel, ClientWorldLocation, RelativeClientLocation,
            ClientMovementBase, ClientBaseBoneName, ClientMovementMode);
    }

    const FVector LocDiff = UpdatedComponent->GetComponentLocation() - ClientWorldLocation;
    if (LocDiff.SizeSquared() <= FMath::Square(PhysicsContactErrorTolerance))
        return false;

    bNetworkLargeClientCorrection |= LocDiff.SizeSquared() > FMath::Square(NetworkLargeClientCorrectionDistance);
    return true;
}

bool USmoothSyncCharacterMovementComponent::ServerShouldUseAuthoritativePosition(float ClientTimeStamp, float DeltaTime, const FVector& Accel,
    const FVector& ClientWorldLocation, const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode)
{
    // Within the tolerated contact error, take the client's position so both sides stay in agreement.
    return IsInPhysicsContact()
        || Super::ServerShouldUseAuthoritativePosition(ClientTimeStamp, DeltaTime, Accel, ClientWorldLocation, RelativeClientLocation,
            ClientMovementBase, ClientBaseBoneName, ClientMovementMode);
}
