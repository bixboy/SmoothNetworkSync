// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "SmoothSyncSubsystem.h"
#include "NetworkInterpolatorComponent.h"
#include "SmoothSyncable.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Math/UnrealMathUtility.h"
#include "DrawDebugHelpers.h"

static TAutoConsoleVariable<int32> CVarSmoothSyncDebug(
    TEXT("smoothsync.Debug"),
    0,
    TEXT("Enable global Smooth Sync debug visualization.\n")
    TEXT("0: Off, 1: On"),
    ECVF_Cheat
);

USmoothSyncSubsystem::USmoothSyncSubsystem()
{
}

void USmoothSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ActiveComponents.Reset();
}

void USmoothSyncSubsystem::Deinitialize()
{
    ActiveComponents.Reset();
    Super::Deinitialize();
}


// ===== Registration methods =====

void USmoothSyncSubsystem::RegisterComponent(UNetworkInterpolatorComponent* InComponent)
{
    if (IsValid(InComponent) && !ActiveComponents.Contains(InComponent))
    {
        ActiveComponents.Add(InComponent);
    }
}

void USmoothSyncSubsystem::UnregisterComponent(UNetworkInterpolatorComponent* InComponent)
{
    ActiveComponents.RemoveSingleSwap(InComponent);
}


// ===== Lifetime tick =====

void USmoothSyncSubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    UWorld* World = GetWorld();
    if (!IsValid(World))
        return;

    const float CurrentLocalTime = World->GetTimeSeconds();

    TArray<FVector> LocalPawnPositions;
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* PC = It->Get();
        if (PC && PC->GetPawn())
        {
            LocalPawnPositions.Add(PC->GetPawn()->GetActorLocation());
        }
    }

    // Loop
    for (int32 i = ActiveComponents.Num() - 1; i >= 0; --i)
    {
        UNetworkInterpolatorComponent* Comp = ActiveComponents[i];

        if (!IsValid(Comp))
        {
            ActiveComponents.RemoveAtSwap(i);
            continue;
        }

        AActor* OwnerActor = Comp->GetOwner();
        
        if (!IsValid(OwnerActor))
            continue;

        if (OwnerActor->HasAuthority() || OwnerActor->GetLocalRole() == ROLE_AutonomousProxy)
        {
            float CurrentPosUpdateRate = Comp->GetPositionUpdateRate();
            float CurrentRotUpdateRate = Comp->GetRotationUpdateRate();
            
            // Network LOD
            if (OwnerActor->HasAuthority())
            {
                float MinDistSq = MAX_flt;
                const FVector OwnerLoc = OwnerActor->GetActorLocation();
                for (const FVector& PawnLoc : LocalPawnPositions)
                {
                    float DistSq = FVector::DistSquared(PawnLoc, OwnerLoc);
                    if (DistSq < MinDistSq)
                    {
                        MinDistSq = DistSq;
                    }
                }

                if (MinDistSq != MAX_flt)
                {
                    if (MinDistSq > FMath::Square(15000.f))
                    {
                        CurrentPosUpdateRate = FMath::Min(CurrentPosUpdateRate, 5.0f);
                        CurrentRotUpdateRate = FMath::Min(CurrentRotUpdateRate, 5.0f);
                    }
                    else if (MinDistSq > FMath::Square(5000.f))
                    {
                        CurrentPosUpdateRate = FMath::Min(CurrentPosUpdateRate, 10.0f);
                        CurrentRotUpdateRate = FMath::Min(CurrentRotUpdateRate, 10.0f);
                    }
                }
            }

            Comp->PosTimeSinceLastSync += DeltaTime;
            Comp->RotTimeSinceLastSync += DeltaTime;
            
            const float PosSyncInterval = 1.0f / CurrentPosUpdateRate;
            const float RotSyncInterval = 1.0f / CurrentRotUpdateRate;
            
            bool bShouldSendPos = false;
            bool bShouldSendRot = false;

            FVector NewPos = OwnerActor->GetActorLocation();
            FVector NewVel = OwnerActor->GetVelocity();
            FVector NewScale = OwnerActor->GetActorScale3D();
            
            if (Comp->PosTimeSinceLastSync >= PosSyncInterval)
            {
                bool bScaleChanged = Comp->bSyncScale && !NewScale.Equals(Comp->SyncPos.Scale, Comp->ScaleSyncTolerance);
                
                if (!NewPos.Equals(Comp->SyncPos.Position, Comp->PositionSyncTolerance) || !NewVel.Equals(Comp->SyncPos.Velocity, Comp->PositionSyncTolerance) || bScaleChanged)
                {
                    Comp->SyncPos.Position = NewPos;
                    Comp->SyncPos.Velocity = NewVel;
                    
                    if (Comp->bSyncScale)
                        Comp->SyncPos.Scale = NewScale;

                    Comp->SyncPos.ServerTimestamp = World->GetTimeSeconds();
                    bShouldSendPos = true;
                }

                Comp->PosTimeSinceLastSync = FMath::Fmod(Comp->PosTimeSinceLastSync, PosSyncInterval);
            }

            FQuat NewRot = OwnerActor->GetActorQuat();
            if (Comp->RotTimeSinceLastSync >= RotSyncInterval)
            {
                if (!NewRot.Equals(Comp->SyncRot.Rotation, Comp->RotationSyncTolerance))
                {
                    Comp->SyncRot.Rotation = NewRot;
                    Comp->SyncRot.ServerTimestamp = World->GetTimeSeconds();
                    bShouldSendRot = true;
                }

                Comp->RotTimeSinceLastSync = FMath::Fmod(Comp->RotTimeSinceLastSync, RotSyncInterval);
            }

            if (!OwnerActor->HasAuthority())
            {
                if (Comp->bIsClientAuthoritative && (bShouldSendPos || bShouldSendRot))
                {
                    Comp->ClientSendStateToServer();
                }
            }
        }

        ENetRole Role = OwnerActor->GetLocalRole();
        
        if (Role != ROLE_SimulatedProxy && Role != ROLE_AutonomousProxy)
            continue;

        if ((Comp->GetPosStateCount() == 0 && Comp->GetRotStateCount() == 0))
            continue;

        float ServerTime = CurrentLocalTime;
        if (AGameStateBase* GS = World->GetGameState())
        {
            ServerTime = GS->GetServerWorldTimeSeconds();
        }

        // --- PRÉDICTION CLIENT (Soft Error Correction) ---

        if (Role == ROLE_AutonomousProxy)
        {
            HandleAutonomousProxyPrediction(OwnerActor, Comp, DeltaTime, ServerTime);
            continue;
        }

        // --- INTERPOLATION ---

        if (Comp->bDisableSmoothing)
            continue;

        // Dynamic Jitter Buffer
        int32 BufferedStates = FMath::Min(Comp->GetPosStateCount(), Comp->GetRotStateCount());
        if (BufferedStates <= 2)
        {
            Comp->TargetInterpolationDelay = 0.15f;
        }
        else if (BufferedStates >= 5)
        {
            Comp->TargetInterpolationDelay = 0.10f;
        }
        
        Comp->InterpolationDelay = FMath::FInterpTo(Comp->InterpolationDelay, Comp->TargetInterpolationDelay, DeltaTime, 1.0f);

        const float EstimatedServerTime = ServerTime;
        const float TargetTime = EstimatedServerTime - Comp->TargetInterpolationDelay;

        FTransform NewTransform;
        FVector NewVelocity;
        FVector NewScale = OwnerActor->GetActorScale3D();

        EvaluateDualState(Comp, TargetTime, NewTransform, NewVelocity, NewScale);

        if (Comp->bSyncScale)
        {
            NewTransform.SetScale3D(NewScale);
        }

        if (OwnerActor->Implements<USmoothSyncable>())
        {
            ISmoothSyncable::Execute_ApplySmoothedTransform(OwnerActor, NewTransform, NewVelocity);
        }
        else
        {
            OwnerActor->SetActorTransform(NewTransform, false, nullptr, ETeleportType::None);
        }

        // --- DEBUG DRAWER ---
        if (Comp->bShowDebugPath || CVarSmoothSyncDebug.GetValueOnAnyThread() > 0)
        {
            const FVector CurrentLoc = NewTransform.GetLocation();
            
            // Draw the Path
            const int32 PosCount = Comp->GetPosStateCount();
            if (PosCount > 1)
            {
                for (int32 j = 0; j < PosCount - 1; ++j)
                {
                    const FVector P0 = Comp->GetPosState(j).Position;
                    const FVector P1 = Comp->GetPosState(j + 1).Position;
                    DrawDebugLine(World, P0, P1, FColor::Emerald, false, -1.f, 0, 1.f);
                    DrawDebugPoint(World, P0, 5.f, FColor::Green, false, -1.f);
                }
            }

            // State Colors & Text
            const FSmoothSyncState_Pos& LatestServerPos = Comp->GetPosState(0);
            FColor StatusColor = FColor::Cyan;
            FString StatusText = FString::Printf(TEXT("Interpolating (Delay: %.2fs) [Buffer: %d]"), Comp->InterpolationDelay, PosCount);

            if (Comp->bIsLagging)
            {
                StatusColor = FColor::Red;
                StatusText = TEXT("EXTRAPOLATION CAPPED (LAG)");
            }
            else if (TargetTime > LatestServerPos.ServerTimestamp + 0.05f)
            {
                StatusColor = FColor::Orange;
                StatusText = TEXT("Extrapolating");
            }

            DrawDebugBox(World, CurrentLoc, FVector(12.f), StatusColor, false, -1.f, 0, 2.f);
            DrawDebugLine(World, LatestServerPos.Position, CurrentLoc, StatusColor, false, -1.f, 0, 0.5f);
            DrawDebugString(World, CurrentLoc + FVector(0, 0, 100.f), StatusText, nullptr, StatusColor, 0.0f, true);
        }
    }
}


// ===== Stat logics =====

TStatId USmoothSyncSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USmoothSyncSubsystem, STATGROUP_Tickables);
}

void USmoothSyncSubsystem::EvaluateDualState(UNetworkInterpolatorComponent* InComponent, float InTargetTime, FTransform& OutTransform, FVector& OutVelocity, FVector& OutScale) const
{
    FVector EvalPos = FVector::ZeroVector;
    FQuat EvalRot = FQuat::Identity;
    OutVelocity = FVector::ZeroVector;
    OutScale = FVector::OneVector;

    // --- POSITION EVALUATION ---
    int32 PosCount = InComponent->GetPosStateCount();
    if (PosCount == 0)
    {
        EvalPos = InComponent->GetOwner()->GetActorLocation();
        OutScale = InComponent->GetOwner()->GetActorScale3D();
    }
    else if (PosCount == 1)
    {
        const FSmoothSyncState_Pos& State = InComponent->GetPosState(0);
        EvalPos = State.Position;
        OutVelocity = State.Velocity;
        OutScale = State.Scale;
    }
    else
    {
        const FSmoothSyncState_Pos& NewestPos = InComponent->GetPosState(0);
        const FSmoothSyncState_Pos& OldestPos = InComponent->GetPosState(PosCount - 1);

        if (InTargetTime >= NewestPos.ServerTimestamp)
        {
            float ExtrapAlpha = InTargetTime - NewestPos.ServerTimestamp;
            if (ExtrapAlpha > InComponent->MaxExtrapolationTime)
            {
                ExtrapAlpha = InComponent->MaxExtrapolationTime;
                OutVelocity = FVector::ZeroVector;
                
                if (!InComponent->bIsLagging)
                {
                    InComponent->bIsLagging = true;
                    InComponent->OnNetworkLagDetected.Broadcast();
                }
            }
            else
            {
                OutVelocity = NewestPos.Velocity;
                InComponent->bIsLagging = false;
            }
            
            EvalPos = NewestPos.Position + (NewestPos.Velocity * ExtrapAlpha);
            OutScale = NewestPos.Scale;
        }
        else if (InTargetTime <= OldestPos.ServerTimestamp)
        {
            EvalPos = OldestPos.Position;
            OutVelocity = OldestPos.Velocity;
            OutScale = OldestPos.Scale;
            InComponent->bIsLagging = false;
        }
        else
        {
            FSmoothSyncState_Pos S0 = OldestPos;
            FSmoothSyncState_Pos S1 = NewestPos;

            for (int32 i = 0; i < PosCount - 1; ++i)
            {
                const FSmoothSyncState_Pos& Newer = InComponent->GetPosState(i);
                const FSmoothSyncState_Pos& Older = InComponent->GetPosState(i + 1);

                if (InTargetTime <= Newer.ServerTimestamp && InTargetTime >= Older.ServerTimestamp)
                {
                    S1 = Newer;
                    S0 = Older;
                    break;
                }
            }

            const float TimeDiff = S1.ServerTimestamp - S0.ServerTimestamp;
            if (FMath::IsNearlyZero(TimeDiff))
            {
                EvalPos = S1.Position;
                OutVelocity = S1.Velocity;
            }
            else if (FVector::DistSquared(S0.Position, S1.Position) > FMath::Square(InComponent->TeleportDistanceThreshold))
            {
                EvalPos = S1.Position;
                OutVelocity = S1.Velocity;
                OutScale = S1.Scale;
                InComponent->OnHardSnapTriggered.Broadcast();
            }
            else
            {
                const float Alpha = (InTargetTime - S0.ServerTimestamp) / TimeDiff;

                FVector Vel0 = S0.Velocity;
                FVector Vel1 = S1.Velocity;

                if (Vel0.IsNearlyZero() && Vel1.IsNearlyZero())
                {
                    Vel0 = (S1.Position - S0.Position) / TimeDiff;
                    Vel1 = Vel0;
                }

                const FVector Tangent0 = Vel0 * TimeDiff;
                const FVector Tangent1 = Vel1 * TimeDiff;

                EvalPos = FMath::CubicInterp(S0.Position, Tangent0, S1.Position, Tangent1, Alpha);
                OutVelocity = FMath::Lerp(Vel0, Vel1, Alpha);
                OutScale = FMath::Lerp(S0.Scale, S1.Scale, Alpha);
            }
        }
    }

    // --- ROTATION EVALUATION ---
    int32 RotCount = InComponent->GetRotStateCount();
    if (RotCount == 0)
    {
        EvalRot = InComponent->GetOwner()->GetActorQuat();
    }
    else if (RotCount == 1)
    {
        EvalRot = InComponent->GetRotState(0).Rotation;
    }
    else
    {
        const FSmoothSyncState_Rot& NewestRot = InComponent->GetRotState(0);
        const FSmoothSyncState_Rot& OldestRot = InComponent->GetRotState(RotCount - 1);

        if (InTargetTime >= NewestRot.ServerTimestamp)
        {
            EvalRot = NewestRot.Rotation;
        }
        else if (InTargetTime <= OldestRot.ServerTimestamp)
        {
            EvalRot = OldestRot.Rotation;
        }
        else
        {
            FSmoothSyncState_Rot S0 = OldestRot;
            FSmoothSyncState_Rot S1 = NewestRot;

            for (int32 i = 0; i < RotCount - 1; ++i)
            {
                const FSmoothSyncState_Rot& Newer = InComponent->GetRotState(i);
                const FSmoothSyncState_Rot& Older = InComponent->GetRotState(i + 1);

                if (InTargetTime <= Newer.ServerTimestamp && InTargetTime >= Older.ServerTimestamp)
                {
                    S1 = Newer;
                    S0 = Older;
                    break;
                }
            }

            const float TimeDiff = S1.ServerTimestamp - S0.ServerTimestamp;
            if (FMath::IsNearlyZero(TimeDiff))
            {
                EvalRot = S1.Rotation;
            }
            else
            {
                const float Alpha = (InTargetTime - S0.ServerTimestamp) / TimeDiff;
                EvalRot = FQuat::Slerp(S0.Rotation, S1.Rotation, Alpha);
            }
        }
    }

    OutTransform = FTransform(EvalRot, EvalPos);
}

void USmoothSyncSubsystem::HandleAutonomousProxyPrediction(AActor* OwnerActor, UNetworkInterpolatorComponent* Comp, float DeltaTime, float CurrentLocalTime) const
{
    const FSmoothSyncState_Pos& LatestServerPos = Comp->GetPosState(0);
    FVector LocalPos = OwnerActor->GetActorLocation();
    
    float TimeSinceServerUpdate = CurrentLocalTime - LatestServerPos.ServerTimestamp;
    FVector ExtrapServerPos = LatestServerPos.Position;

    if (TimeSinceServerUpdate > 0.f && TimeSinceServerUpdate < Comp->MaxExtrapolationTime)
    {
        ExtrapServerPos += LatestServerPos.Velocity * TimeSinceServerUpdate;
    }

    float ErrorSq = FVector::DistSquared(LocalPos, ExtrapServerPos);

    UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(OwnerActor->GetRootComponent());
    bool bIsSimulating = RootPrim && RootPrim->IsSimulatingPhysics();

    if (ErrorSq > FMath::Square(Comp->TeleportDistanceThreshold))
    {
        OwnerActor->SetActorLocation(ExtrapServerPos, false, nullptr, ETeleportType::TeleportPhysics);
        if (bIsSimulating)
        {
            RootPrim->SetPhysicsLinearVelocity(LatestServerPos.Velocity);
            RootPrim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
        }
        
        Comp->OnHardSnapTriggered.Broadcast();
    }
    else if (ErrorSq > FMath::Square(Comp->PredictionErrorTolerance))
    {
        if (bIsSimulating)
        {
            FVector Error = ExtrapServerPos - LocalPos;
            Error.Z *= Comp->ZDampingFactor;

            float ErrorMag = Error.Size();
            float SpringStiffness = Comp->BaseSpringStiffness;
            
            if (ErrorMag > Comp->ErrorThresholdForScaling)
            {
                SpringStiffness += (ErrorMag - Comp->ErrorThresholdForScaling) * Comp->SpringStiffnessScale;
            }

            FVector NudgeAccel = Error * SpringStiffness; 
            NudgeAccel = NudgeAccel.GetClampedToMaxSize(Comp->MaxLinearNudgeAccel);
            
            RootPrim->SetPhysicsLinearVelocity(RootPrim->GetPhysicsLinearVelocity() + (NudgeAccel * DeltaTime));
        }
        else
        {
            FVector CorrectedPos = FMath::VInterpTo(LocalPos, ExtrapServerPos, DeltaTime, Comp->PositionInterpSpeed);
            OwnerActor->SetActorLocation(CorrectedPos, false, nullptr, ETeleportType::None);
        }
    }

    if (Comp->GetRotStateCount() > 0)
    {
        FQuat LocalRot = OwnerActor->GetActorQuat();
        FQuat ServerRot = Comp->GetRotState(0).Rotation;
        float AngularDist = LocalRot.AngularDistance(ServerRot);
        
        if (AngularDist > FMath::DegreesToRadians(Comp->HardSnapAngleDegrees))
        {
            OwnerActor->SetActorRotation(ServerRot, ETeleportType::TeleportPhysics);

            if (bIsSimulating)
                RootPrim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);

        }
        else if (AngularDist > FMath::DegreesToRadians(Comp->SoftSnapAngleDegrees))
        {
            if (bIsSimulating)
            {
                FVector Axis;
                float Angle;
                (ServerRot * LocalRot.Inverse()).ToAxisAndAngle(Axis, Angle);
                
                if (Angle > PI)
                    Angle -= 2.0f * PI;
                
                FVector AngularAccel = Axis * Angle * Comp->AngularSpringStiffness; 
                AngularAccel = AngularAccel.GetClampedToMaxSize(Comp->MaxAngularNudgeAccel);
                
                FVector AngularVelChange = FMath::RadiansToDegrees(AngularAccel) * DeltaTime;
                RootPrim->SetPhysicsAngularVelocityInDegrees(RootPrim->GetPhysicsAngularVelocityInDegrees() + AngularVelChange);
            }
            else
            {
                FQuat CorrectedRot = FQuat::Slerp(LocalRot, ServerRot, DeltaTime * Comp->RotationInterpSpeed);
                OwnerActor->SetActorRotation(CorrectedRot, ETeleportType::None);
            }
        }
    }

    // --- DEBUG DRAWER FOR AUTONOMOUS PROXY ---
    if (Comp->bShowDebugPath || CVarSmoothSyncDebug.GetValueOnAnyThread() > 0)
    {
        UWorld* World = OwnerActor->GetWorld();
        if (IsValid(World))
        {
            FColor StatusColor = (ErrorSq > FMath::Square(Comp->TeleportDistanceThreshold)) ? FColor::Red : 
                                 (ErrorSq > FMath::Square(Comp->PredictionErrorTolerance)) ? FColor::Orange : FColor::Green;

            FString StatusText = FString::Printf(TEXT("Auto Proxy Prediction (Error: %.1f)"), FMath::Sqrt(ErrorSq));

            DrawDebugBox(World, ExtrapServerPos, FVector(12.f), StatusColor, false, -1.f, 0, 2.f);
            DrawDebugLine(World, ExtrapServerPos, LocalPos, StatusColor, false, -1.f, 0, 0.5f);
            DrawDebugString(World, LocalPos + FVector(0, 0, 100.f), StatusText, nullptr, StatusColor, 0.0f, true);
        }
    }
}
