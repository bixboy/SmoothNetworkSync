#include "SmoothScaleProp.h"
#include "NetworkInterpolatorComponent.h"
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
    
    // Setup the network component
    NetworkInterpolator = CreateDefaultSubobject<UNetworkInterpolatorComponent>(TEXT("NetworkInterpolator"));
    NetworkInterpolator->bShowDebugPath = true;
    NetworkInterpolator->bSyncScale = true;
}

void ASmoothScaleProp::BeginPlay()
{
    Super::BeginPlay();
    
    BaseScale = GetActorScale3D();
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
