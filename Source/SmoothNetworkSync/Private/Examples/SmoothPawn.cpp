#include "SmoothPawn.h"
#include "NetworkInterpolatorComponent.h"
#include "Components/InputComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"

ASmoothPawn::ASmoothPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    SetReplicateMovement(false);

    NetworkInterpolator = CreateDefaultSubobject<UNetworkInterpolatorComponent>(TEXT("NetworkInterpolator"));
}

void ASmoothPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void ASmoothPawn::BeginPlay()
{
    Super::BeginPlay();
}

void ASmoothPawn::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (PC->IsInputKeyDown(EKeys::Z)) MoveForward(1.0f);
        if (PC->IsInputKeyDown(EKeys::S)) MoveForward(-1.0f);
        if (PC->IsInputKeyDown(EKeys::D)) MoveRight(1.0f);
        if (PC->IsInputKeyDown(EKeys::Q)) MoveRight(-1.0f);
    }
}

void ASmoothPawn::ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity)
{
    SetActorTransform(InNewTransform, false, nullptr, ETeleportType::None);
}
