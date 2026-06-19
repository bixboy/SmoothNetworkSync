// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothProp.h"
#include "NetworkInterpolatorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"


ASmoothProp::ASmoothProp()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    
    SetReplicateMovement(false);

    MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
    RootComponent = MeshComponent;

    NetworkInterpolator = CreateDefaultSubobject<UNetworkInterpolatorComponent>(TEXT("NetworkInterpolator"));
}

void ASmoothProp::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void ASmoothProp::BeginPlay()
{
    Super::BeginPlay();
    
    InitialLocation = GetActorLocation();
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
