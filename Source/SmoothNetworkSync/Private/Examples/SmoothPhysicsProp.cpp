// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothPhysicsProp.h"
#include "NetworkInterpolatorComponent.h"
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

    NetworkInterpolator = CreateDefaultSubobject<UNetworkInterpolatorComponent>(TEXT("NetworkInterpolator"));
}

void ASmoothPhysicsProp::BeginPlay()
{
    Super::BeginPlay();

    if (HasAuthority())
    {
        MeshComponent->SetSimulatePhysics(true);
    }
    else
    {
        MeshComponent->SetSimulatePhysics(false);
    }
}

void ASmoothPhysicsProp::ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity)
{
    SetActorLocationAndRotation(InNewTransform.GetLocation(), InNewTransform.GetRotation(), false, nullptr, ETeleportType::None);
}
