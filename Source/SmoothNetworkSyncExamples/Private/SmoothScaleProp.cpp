// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothScaleProp.h"
#include "Components/SmoothSyncComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/StaticMesh.h"


ASmoothScaleProp::ASmoothScaleProp()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    SetReplicatingMovement(false);

    MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
    RootComponent = MeshComponent;
    
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube"));
    if (CubeMesh.Succeeded())
    {
        MeshComponent->SetStaticMesh(CubeMesh.Object);
    }
    
    NetworkInterpolator = CreateDefaultSubobject<USmoothSyncComponent>(TEXT("NetworkInterpolator"));
    NetworkInterpolator->bShowDebugPath = true;
    NetworkInterpolator->bSyncScale = true;
}

void ASmoothScaleProp::BeginPlay()
{
    Super::BeginPlay();
    
    BaseScale = GetActorScale3D();

    // Moving props shouldn't pull the player's camera in.
    MeshComponent->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

    // Only the server animates the prop; clients receive the scale from Smooth Sync.
    SetActorTickEnabled(HasAuthority());
}

void ASmoothScaleProp::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (HasAuthority())
    {
        RunningTime += DeltaTime;
        float ScaleMultiplier = 1.0f + FMath::Abs(FMath::Sin(RunningTime * 2.0f)) * 1.5f;
        
        SetActorScale3D(BaseScale * ScaleMultiplier);
    }
}

void ASmoothScaleProp::ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity)
{
    SetActorTransform(InNewTransform, false, nullptr, ETeleportType::None);
}
