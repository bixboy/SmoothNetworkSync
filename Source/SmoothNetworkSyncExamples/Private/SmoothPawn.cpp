// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothPawn.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputSubsystems.h"
#include "Components/InputComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/SmoothSyncCharacterMovementComponent.h"
#include "Components/SmoothSyncComponent.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"


ASmoothPawn::ASmoothPawn(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<USmoothSyncCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
    // bReplicateMovement must stay on: the CharacterMovementComponent only sends the owner's moves to the server with it.
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;

    if (GetMesh())
    {
        GetMesh()->SetCollisionProfileName(TEXT("NoCollision"));
        GetMesh()->SetHiddenInGame(true);
    }

    // Visual only: the capsule handles collision. Absolute rotation so the camera yaw doesn't spin the ball.
    StaticMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMesh"));
    StaticMesh->SetupAttachment(RootComponent);
    StaticMesh->bOwnerNoSee = false;
    StaticMesh->SetCollisionProfileName(TEXT("NoCollision"));
    StaticMesh->SetUsingAbsoluteRotation(true);

    // Inertia: slow to speed up, long glide when releasing input, drifts in turns.
    if (UCharacterMovementComponent* Movement = GetCharacterMovement())
    {
        Movement->MaxAcceleration = 900.0f;
        Movement->BrakingDecelerationWalking = 250.0f;
        Movement->GroundFriction = 1.0f;
        Movement->bUseSeparateBrakingFriction = true;
        Movement->BrakingFriction = 0.2f;
        Movement->AirControl = 0.35f;

        // The default push (750000, not scaled to mass) is tuned for heavy crates and launches light props across the map.
        // Scaled to mass, the push gives every prop the same acceleration (cm/s²) while in contact.
        Movement->bPushForceScaledToMass = true;
        Movement->PushForceFactor = 6000.0f;
        Movement->InitialPushForceFactor = 1000.0f;
    }

    // Client authority: the owner is never corrected (smooth even when bumping props), the server still pushes props
    // with its moves and rejects anything faster than MaxClientMoveSpeed. Other players' balls are interpolated.
    SmoothSync = CreateDefaultSubobject<USmoothSyncComponent>(TEXT("SmoothSync"));
    SmoothSync->bIsClientAuthoritative = true;
    SmoothSync->MaxClientMoveSpeed = 1500.0f;
    SmoothSync->PositionUpdateRate = 30.0f;
    SmoothSync->RotationUpdateRate = 30.0f;

    SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
    SpringArm->SetupAttachment(RootComponent);
    SpringArm->TargetArmLength = 500.0f;
    SpringArm->bUsePawnControlRotation = true;

    // Bumping a networked prop makes the server correct the pawn by a few units (client and server never collide
    // at exactly the same place). A fast camera lag hides these corrections without feeling delayed.
    SpringArm->bEnableCameraLag = true;
    SpringArm->CameraLagSpeed = 20.0f;

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
    Camera->bUsePawnControlRotation = false;
}

void ASmoothPawn::BeginPlay()
{
    Super::BeginPlay();

    EffectiveBallRadius = BallRadius;
    if (EffectiveBallRadius <= 0.0f && StaticMesh->GetStaticMesh())
    {
        const FVector Extent = StaticMesh->GetStaticMesh()->GetBounds().BoxExtent * StaticMesh->GetComponentScale();
        EffectiveBallRadius = Extent.GetMax();
    }
    EffectiveBallRadius = FMath::Max(EffectiveBallRadius, 1.0f);

    RollRotation = StaticMesh->GetComponentQuat();

    if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
        {
            if (DefaultMappingContext)
            {
                Subsystem->AddMappingContext(DefaultMappingContext, 0);
            }
        }
    }
}

void ASmoothPawn::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // Remote copies smoothed by Smooth Sync roll in ApplySmoothedTransform, from the smoothed positions.
    const bool bSmoothedBySmoothSync = GetLocalRole() == ROLE_SimulatedProxy && SmoothSync && !SmoothSync->bDisableSmoothing;
    if (!bSmoothedBySmoothSync)
        RollBy(GetVelocity() * DeltaTime);
}

void ASmoothPawn::ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity)
{
    SetActorTransform(InNewTransform, false, nullptr, ETeleportType::None);

    // Measured between smoothed positions: the actor location may hold a raw network update in between.
    const FVector Displacement = InNewTransform.GetLocation() - LastSmoothedLocation;
    const bool bHadSmoothedLocation = bHasSmoothedLocation;
    LastSmoothedLocation = InNewTransform.GetLocation();
    bHasSmoothedLocation = true;

    // Ignore snaps (teleports, corrections too large to smooth).
    if (bHadSmoothedLocation && Displacement.Size2D() < EffectiveBallRadius * 10.0f)
        RollBy(Displacement);
}

void ASmoothPawn::RollBy(FVector InDisplacement)
{
    InDisplacement.Z = 0.0;
    const double Distance = InDisplacement.Size();
    if (Distance < UE_KINDA_SMALL_NUMBER)
        return;

    const FVector Delta = InDisplacement;

    // Rolling without slipping: the ball turns around the horizontal axis perpendicular to its motion.
    const FVector Axis = FVector::CrossProduct(FVector::UpVector, Delta / Distance);
    RollRotation = FQuat(Axis, Distance / EffectiveBallRadius) * RollRotation;
    RollRotation.Normalize();

    StaticMesh->SetWorldRotation(RollRotation);
}

void ASmoothPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        if (MoveAction)
        {
            EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ASmoothPawn::Input_Move);
        }

        if (LookAction)
        {
            EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ASmoothPawn::Input_Look);
        }

        if (JumpAction)
        {
            EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
            EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
        }
    }
}

void ASmoothPawn::Input_Move(const FInputActionValue& Value)
{
    FVector2D MovementVector = Value.Get<FVector2D>();

    if (Controller != nullptr)
    {
        const FRotator Rotation = Controller->GetControlRotation();
        const FRotator YawRotation(0, Rotation.Yaw, 0);

        const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
        const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

        AddMovementInput(ForwardDirection, MovementVector.Y);
        AddMovementInput(RightDirection, MovementVector.X);
    }
}

void ASmoothPawn::Input_Look(const FInputActionValue& Value)
{
    FVector2D LookAxisVector = Value.Get<FVector2D>();

    if (Controller != nullptr)
    {
        AddControllerYawInput(LookAxisVector.X);
        AddControllerPitchInput(LookAxisVector.Y);
    }
}
