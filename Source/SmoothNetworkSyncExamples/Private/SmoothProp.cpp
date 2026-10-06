// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothProp.h"
#include "Components/SmoothSyncComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"


ASmoothProp::ASmoothProp()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    
    SetReplicateMovement(false);

    MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
    RootComponent = MeshComponent;
    MeshComponent->SetMobility(EComponentMobility::Movable);

    NetworkInterpolator = CreateDefaultSubobject<USmoothSyncComponent>(TEXT("NetworkInterpolator"));
}

void ASmoothProp::BeginPlay()
{
    Super::BeginPlay();

    InitialLocation = GetActorLocation();

    // Moving props shouldn't pull the player's camera in.
    MeshComponent->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

    // Only the server animates the prop; clients receive the motion from Smooth Sync.
    SetActorTickEnabled(HasAuthority());
}

void ASmoothProp::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (HasAuthority())
    {
        const float Time = GetWorld()->GetTimeSeconds();
        FVector NewPos = InitialLocation + FVector(FMath::Sin(Time * 2.f) * 300.f, FMath::Cos(Time * 2.f) * 300.f, 0.f);
        SetActorLocation(NewPos);
    }
}

void ASmoothProp::ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity)
{
    SetActorTransform(InNewTransform, false, nullptr, ETeleportType::None);
}
