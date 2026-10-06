// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Interfaces/SmoothSyncable.h"
#include "SmoothPawn.generated.h"

class UStaticMeshComponent;
class USmoothSyncComponent;
class USpringArmComponent;
class UCameraComponent;
class UInputMappingContext;
class UInputAction;
struct FInputActionValue;


/**
 * Demo player: a rolling ball driven by the CharacterMovementComponent (tuned for inertia).
 * The CMC predicts the owner; Smooth Sync makes the server trust the owner (no corrections when bumping props, with a
 * speed check) and shows the other players' balls with adaptive interpolation.
 * The ball mesh rolls from the movement on every machine (owner, server, remote copies) without replicating anything extra.
 */
UCLASS()
class SMOOTHNETWORKSYNCEXAMPLES_API ASmoothPawn : public ACharacter, public ISmoothSyncable
{
    GENERATED_BODY()

public:
    ASmoothPawn(const FObjectInitializer& ObjectInitializer);

    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

    virtual void Tick(float DeltaTime) override;

    /** Remote copies: Smooth Sync hands over the smoothed transform, so the ball rolls from smoothed positions. */
    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

protected:

    virtual void BeginPlay() override;

    /** Radius used to turn distance into rotation. 0 = computed from the ball mesh bounds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball", meta = (ClampMin = "0"))
    float BallRadius = 0.0f;

    /** Ball mesh. Rotated in world space by the rolling, independently of the camera yaw. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> StaticMesh = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<USmoothSyncComponent> SmoothSync = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    TObjectPtr<USpringArmComponent> SpringArm = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    TObjectPtr<UCameraComponent> Camera = nullptr;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputMappingContext> DefaultMappingContext = nullptr;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> MoveAction = nullptr;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> LookAction = nullptr;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> JumpAction = nullptr;

    void Input_Move(const FInputActionValue& Value);
    void Input_Look(const FInputActionValue& Value);

private:

    void RollBy(FVector InDisplacement);

    FQuat RollRotation = FQuat::Identity;
    FVector LastSmoothedLocation = FVector::ZeroVector;
    bool bHasSmoothedLocation = false;
    float EffectiveBallRadius = 50.0f;
};
