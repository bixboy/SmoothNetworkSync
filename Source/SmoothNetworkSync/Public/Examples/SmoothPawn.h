// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "SmoothSyncable.h"
#include "SmoothSyncTypes.h"
#include "SmoothPawn.generated.h"

class UNetworkInterpolatorComponent;
class USpringArmComponent;
class UCameraComponent;
class UInputMappingContext;
class UInputAction;
struct FInputActionValue;


UCLASS()
class SMOOTHNETWORKSYNC_API ASmoothPawn : public ADefaultPawn, public ISmoothSyncable
{
    GENERATED_BODY()

public:
    ASmoothPawn();

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;

    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:

    virtual void BeginPlay() override;
    

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UNetworkInterpolatorComponent> NetworkInterpolator = nullptr;

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

    void Input_Move(const FInputActionValue& Value);
    void Input_Look(const FInputActionValue& Value);

};
