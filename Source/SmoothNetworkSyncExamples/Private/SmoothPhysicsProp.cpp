// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothPhysicsProp.h"
#include "Components/SmoothSyncComponent.h"
#include "Components/StaticMeshComponent.h"


ASmoothPhysicsProp::ASmoothPhysicsProp()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    
    SetReplicateMovement(false);

    MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
    RootComponent = MeshComponent;
    MeshComponent->SetMobility(EComponentMobility::Movable);
    MeshComponent->SetCollisionProfileName(TEXT("PhysicsActor"));

    // Chaos has no rolling resistance: without damping a ball rolls forever and never rests (nor goes dormant).
    MeshComponent->BodyInstance.LinearDamping = 0.5f;
    MeshComponent->BodyInstance.AngularDamping = 3.0f;

    // Physics preset: angular velocity for spins, shown in the player's present when close (so pushing lines up).
    // Dormancy: resting props cost nothing on the server until something moves them.
    NetworkInterpolator = CreateDefaultSubobject<USmoothSyncComponent>(TEXT("NetworkInterpolator"));
    NetworkInterpolator->ApplyPreset(ESmoothSyncPreset::PhysicsObject);
    NetworkInterpolator->bUseDormancyWhenAtRest = true;
}

void ASmoothPhysicsProp::BeginPlay()
{
    Super::BeginPlay();

    // The server simulates; clients stay kinematic and follow the replicated transform.
    // A simulating client copy would fight the interpolation every frame.
    MeshComponent->SetSimulatePhysics(HasAuthority());

    // Small moving props shouldn't pull the player's camera in.
    MeshComponent->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
}

void ASmoothPhysicsProp::ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity)
{
    SetActorLocationAndRotation(InNewTransform.GetLocation(), InNewTransform.GetRotation(), false, nullptr, ETeleportType::None);
}
