// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "Components/SmoothSyncComponent.h"
#include "Components/SmoothSyncCharacterMovementComponent.h"
#include "Core/SmoothSyncSubsystem.h"
#include "Interfaces/SmoothSyncable.h"
#include "SmoothSyncLog.h"
#include "SmoothSyncStats.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Misc/EngineVersionComparison.h"
#include "Net/UnrealNetwork.h"
#include "Net/Core/PushModel/PushModel.h"
#include "Serialization/BitWriter.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#define LOCTEXT_NAMESPACE "SmoothSyncComponent"


namespace
{
    /** Logs a setup problem once per actor class, so a level full of the same prop does not flood the log. */
    void WarnOncePerClass(const AActor* InOwner, const TCHAR* InIssueKey, const FString& InMessage)
    {
        static TSet<FString> WarnedKeys;
        const FString Key = FString::Printf(TEXT("%s|%s"), *GetNameSafe(InOwner ? InOwner->GetClass() : nullptr), InIssueKey);

        bool bAlreadyWarned = false;
        WarnedKeys.Add(Key, &bAlreadyWarned);

        if (!bAlreadyWarned)
            UE_LOG(LogSmoothSync, Warning, TEXT("%s"), *InMessage);
    }

    float GetOwnerNetUpdateFrequency(const AActor* InOwner)
    {
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        return InOwner->NetUpdateFrequency;
#else
        return InOwner->GetNetUpdateFrequency();
#endif
    }

    /** Rotates InRotation by an angular velocity (rad/s, same space) over InSeconds (can be negative). */
    FQuat IntegrateRotation(const FQuat& InRotation, const FVector& InAngularVelocity, double InSeconds)
    {
        const double Speed = InAngularVelocity.Size();
        if (Speed < UE_SMALL_NUMBER || InSeconds == 0.0)
            return InRotation;

        FQuat Result = FQuat(InAngularVelocity / Speed, Speed * InSeconds) * InRotation;
        Result.Normalize();
        return Result;
    }

    /** Angular velocity turning InFrom into InTo in InSeconds, along the shortest arc. */
    FVector AngularVelocityBetween(const FQuat& InFrom, const FQuat& InTo, double InSeconds)
    {
        if (InSeconds <= UE_SMALL_NUMBER)
            return FVector::ZeroVector;

        FQuat Delta = InTo * InFrom.Inverse();
        if (Delta.W < 0.0)
            Delta = Delta * -1.0;

        FVector Axis;
        double Angle;
        Delta.ToAxisAndAngle(Axis, Angle);
        return Angle < UE_SMALL_NUMBER ? FVector::ZeroVector : Axis * (Angle / InSeconds);
    }

    template<typename StateType>
    int32 MeasureSerializedBits(const StateType& InState)
    {
        StateType Copy = InState;
        FBitWriter Writer(0, true);
        bool bSuccess = false;
        Copy.NetSerialize(Writer, nullptr, bSuccess);
        return static_cast<int32>(Writer.GetNumBits());
    }
}


USmoothSyncComponent::USmoothSyncComponent()
{
    // Ticked in batch by USmoothSyncSubsystem.
    PrimaryComponentTick.bCanEverTick = false;
    bWantsInitializeComponent = false;
    SetIsReplicatedByDefault(true);
}


// ===== Replication setup =====

void USmoothSyncComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    // Push model: properties are only compared when we mark them dirty (requires Net.IsPushModelEnabled=1).
    // Dynamic condition: skips the owner when it is client-authoritative (it already knows its own transform).
    FDoRepLifetimeParams Params;
    Params.bIsPushBased = true;
    Params.Condition = COND_Dynamic;

    DOREPLIFETIME_WITH_PARAMS_FAST(USmoothSyncComponent, SyncPos, Params);
    DOREPLIFETIME_WITH_PARAMS_FAST(USmoothSyncComponent, SyncRot, Params);
}

void USmoothSyncComponent::GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const
{
    Super::GetReplicatedCustomConditionState(OutActiveState);

    // The owner doesn't need its own transform back: it drives it (client authority) or its CMC predicts it (characters).
    const ELifetimeCondition Condition = ShouldSkipOwner() ? COND_SkipOwner : COND_None;
    DOREPDYNAMICCONDITION_INITCONDITION_FAST(USmoothSyncComponent, SyncPos, Condition);
    DOREPDYNAMICCONDITION_INITCONDITION_FAST(USmoothSyncComponent, SyncRot, Condition);
}

void USmoothSyncComponent::UpdateOwnerReplicationCondition()
{
    const bool bSkipOwner = ShouldSkipOwner();
    if (bSkipOwnerApplied == bSkipOwner)
        return;

    bSkipOwnerApplied = bSkipOwner;
    const ELifetimeCondition Condition = bSkipOwner ? COND_SkipOwner : COND_None;
    DOREPDYNAMICCONDITION_SETCONDITION_FAST(USmoothSyncComponent, SyncPos, Condition);
    DOREPDYNAMICCONDITION_SETCONDITION_FAST(USmoothSyncComponent, SyncRot, Condition);
}


// ===== Lifetime =====

void USmoothSyncComponent::BeginPlay()
{
    Super::BeginPlay();

    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
        return;

    ValidateOwnerSetup();

    CurrentInterpolationDelay = InterpolationDelay;
    bSkipOwnerApplied = ShouldSkipOwner();

    bool bRelative = false;
    const FTransform SyncSpace = GetOwnerSyncSpaceTransform(bRelative);
    LastSentPos.Position = SyncSpace.GetLocation();
    LastSentRot.Rotation = SyncSpace.GetRotation();
    FName Socket;
    LastSyncParent = GetSyncParent(Socket);

    if (const UWorld* World = GetWorld())
        LastChangeTime = World->GetTimeSeconds();

    if (UWorld* World = GetWorld())
    {
        if (USmoothSyncSubsystem* SyncSubsystem = World->GetSubsystem<USmoothSyncSubsystem>())
            SyncSubsystem->RegisterComponent(this);
    }
}

void USmoothSyncComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        if (USmoothSyncSubsystem* SyncSubsystem = World->GetSubsystem<USmoothSyncSubsystem>())
            SyncSubsystem->UnregisterComponent(this);
    }

    Super::EndPlay(EndPlayReason);
}

void USmoothSyncComponent::ValidateOwnerSetup()
{
    AActor* Owner = GetOwner();

    bOwnerImplementsSyncable = Owner->Implements<USmoothSyncable>();
    CharacterMovement = Owner->FindComponentByClass<UCharacterMovementComponent>();

    if (!Owner->GetIsReplicated())
    {
        WarnOncePerClass(Owner, TEXT("NotReplicated"), FString::Printf(
            TEXT("%s: owner does not replicate (bReplicates is false). Smooth Sync will do nothing."), *Owner->GetName()));
    }

    if (CharacterMovement.IsValid())
    {
        // The CMC only sends the owning client's moves to the server while bReplicateMovement is on:
        // turning it off freezes the character on the server.
        if (!Owner->IsReplicatingMovement() && Owner->HasAuthority())
        {
            Owner->SetReplicateMovement(true);
            UE_LOG(LogSmoothSync, Log, TEXT("%s: re-enabled bReplicateMovement, the CharacterMovementComponent needs it to send moves."), *Owner->GetName());
        }
    }
    else if (Owner->IsReplicatingMovement() && Owner->HasAuthority())
    {
        // Both systems would fight over the transform.
        Owner->SetReplicateMovement(false);
        UE_LOG(LogSmoothSync, Log, TEXT("%s: disabled bReplicateMovement, Smooth Sync replicates the transform instead."), *Owner->GetName());
    }

    const USceneComponent* Root = Owner->GetRootComponent();
    if (Root && Root->Mobility != EComponentMobility::Movable)
    {
        WarnOncePerClass(Owner, TEXT("NotMovable"), FString::Printf(
            TEXT("%s: root component is not Movable. Smooth Sync cannot move it."), *Owner->GetName()));
    }

    const float NetUpdateFrequency = GetOwnerNetUpdateFrequency(Owner);
    if (NetUpdateFrequency < FMath::Max(PositionUpdateRate, RotationUpdateRate))
    {
        WarnOncePerClass(Owner, TEXT("NetUpdateFrequency"), FString::Printf(
            TEXT("%s: NetUpdateFrequency (%.0f) is lower than the Smooth Sync update rates (%.0f / %.0f). Extra snapshots will be dropped."),
            *Owner->GetName(), NetUpdateFrequency, PositionUpdateRate, RotationUpdateRate));
    }
}


// ===== Public API =====

void USmoothSyncComponent::NotifyTeleported()
{
    const AActor* Owner = GetOwner();
    if (!IsValid(Owner))
        return;

    const bool bCanSend = Owner->HasAuthority() || (bIsClientAuthoritative && Owner->GetLocalRole() == ROLE_AutonomousProxy);
    if (!bCanSend)
    {
        UE_LOG(LogSmoothSync, Warning, TEXT("%s: NotifyTeleported must be called on the server (or on the owning client when client-authoritative)."), *Owner->GetName());
        return;
    }

    MarkTeleported();

    // A client-authoritative owner would otherwise keep sending where it was, and the server would accept it.
    if (Owner->HasAuthority() && bIsClientAuthoritative)
    {
        const APawn* Pawn = Cast<APawn>(Owner);
        const bool bRemotelyControlled = Pawn ? (Pawn->IsPlayerControlled() && !Pawn->IsLocallyControlled()) : LastClientStateTime >= 0.0;
        if (!bRemotelyControlled)
            return;

        if (CharacterMovement.IsValid())
        {
            // Characters: stop trusting the owner for a moment, the CMC then corrects it to the new location.
            bCharacterTrustRevoked = true;
            CharacterTrustRevokedUntil = GetWorld()->GetTimeSeconds() + 1.0;
            ApplyCharacterTrust();
        }
        else
        {
            ForceClientToCurrentTransform();
            RelayCurrentTransform();
        }
    }
}

void USmoothSyncComponent::SetSyncBase(UPrimitiveComponent* InBase)
{
    const AActor* Owner = GetOwner();
    if (!IsValid(Owner) || !Owner->HasAuthority())
    {
        UE_LOG(LogSmoothSync, Warning, TEXT("%s: SetSyncBase must be called on the server."), *GetNameSafe(Owner));
        return;
    }

    if (InBase && (InBase->GetOwner() == Owner || !InBase->IsSupportedForNetworking()))
    {
        UE_LOG(LogSmoothSync, Warning, TEXT("%s: SetSyncBase ignored, %s must be a component of another, replicated actor."),
            *Owner->GetName(), *GetNameSafe(InBase));
        return;
    }

    if (InBase && !bSyncRelativeToParent)
    {
        UE_LOG(LogSmoothSync, Warning, TEXT("%s: SetSyncBase has no effect while Sync Relative To Parent is off."), *Owner->GetName());
    }

    // TickSending sees the new sync parent and tells receivers (they convert what they buffered, no pop).
    SyncBase = InBase;
}

void USmoothSyncComponent::MarkTeleported()
{
    LocalTeleportId = (LocalTeleportId + 1) & SmoothSync::TeleportIdMask;
    bForceSendPos = true;
    bForceSendRot = true;
    LastSamplePosTime = -1.0;
    LastSampleRotTime = -1.0;

    // A teleport is not a speed violation, nor a correction to smooth.
    LastCharacterCheckTime = -1.0;
    bHasLastOwnerLocation = false;
    bHasLastDisplayedLocation = false;
}

void USmoothSyncComponent::ResetInterpolation()
{
    PosBuffer.Reset();
    RotBuffer.Reset();
    PosArrival.Reset();
    RotArrival.Reset();
    PositionErrorOffset = FVector::ZeroVector;
    RotationErrorOffset = FQuat::Identity;
    LastTargetTime = -1.0;
    SpaceStartTimestamp = -1.0;
    bIsSettled = false;
    SetLagging(false);
}

void USmoothSyncComponent::ApplyPreset(ESmoothSyncPreset InPreset)
{
    Preset = InPreset;

    switch (InPreset)
    {
    case ESmoothSyncPreset::Prop:
        PositionUpdateRate = 20.0f;
        RotationUpdateRate = 20.0f;
        InterpolationDelay = 0.1f;
        MaxExtrapolationTime = 0.25f;
        TeleportDistanceThreshold = 1000.0f;
        PositionPrecision = ESmoothSyncPositionPrecision::Millimeter;
        bSyncAngularVelocity = false;
        bDisplayInPresentNearLocalPlayer = false;
        break;

    case ESmoothSyncPreset::PhysicsObject:
        PositionUpdateRate = 30.0f;
        RotationUpdateRate = 30.0f;
        InterpolationDelay = 0.1f;
        MaxExtrapolationTime = 0.3f;
        TeleportDistanceThreshold = 1000.0f;
        PositionPrecision = ESmoothSyncPositionPrecision::Millimeter;
        bSyncAngularVelocity = true;
        bDisplayInPresentNearLocalPlayer = true;
        break;

    case ESmoothSyncPreset::Vehicle:
        PositionUpdateRate = 30.0f;
        RotationUpdateRate = 30.0f;
        InterpolationDelay = 0.08f;
        MaxExtrapolationTime = 0.5f;
        TeleportDistanceThreshold = 2000.0f;
        PredictionErrorTolerance = 50.0f;
        PositionPrecision = ESmoothSyncPositionPrecision::Millimeter;
        bSyncAngularVelocity = true;
        bDisplayInPresentNearLocalPlayer = false;
        MaxClientMoveSpeed = 6000.0f;
        break;

    case ESmoothSyncPreset::Custom:
    default:
        break;
    }
}


// ===== Editor =====

#if WITH_EDITOR
void USmoothSyncComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropertyName = PropertyChangedEvent.GetMemberPropertyName();
    if (PropertyName == GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, Preset))
    {
        ApplyPreset(Preset);
        return;
    }

    static const TSet<FName> PresetProperties = {
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, PositionUpdateRate),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, RotationUpdateRate),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, InterpolationDelay),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, MaxExtrapolationTime),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, TeleportDistanceThreshold),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, PredictionErrorTolerance),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, PositionPrecision),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, bSyncAngularVelocity),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, bDisplayInPresentNearLocalPlayer),
        GET_MEMBER_NAME_CHECKED(USmoothSyncComponent, MaxClientMoveSpeed),
    };

    if (Preset != ESmoothSyncPreset::Custom && PresetProperties.Contains(PropertyName))
        Preset = ESmoothSyncPreset::Custom;
}

EDataValidationResult USmoothSyncComponent::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result = Super::IsDataValid(Context);

    if (!bAdaptiveInterpolationDelay && InterpolationDelay < 1.0f / FMath::Max(PositionUpdateRate, 1.0f))
    {
        Context.AddWarning(FText::Format(
            LOCTEXT("DelayTooShort", "{0}: InterpolationDelay ({1}s) is shorter than the position update interval ({2}s). Remote copies will extrapolate most of the time."),
            FText::FromString(GetName()), FText::AsNumber(InterpolationDelay), FText::AsNumber(1.0f / FMath::Max(PositionUpdateRate, 1.0f))));
    }

    if (bAdaptiveInterpolationDelay && MaxInterpolationDelay < InterpolationDelay)
    {
        Context.AddWarning(FText::Format(
            LOCTEXT("MaxDelayTooShort", "{0}: MaxInterpolationDelay is lower than InterpolationDelay. The adaptive delay cannot grow."),
            FText::FromString(GetName())));
    }

    if (bIsClientAuthoritative && MaxClientMoveSpeed <= 0.0f)
    {
        Context.AddWarning(FText::Format(
            LOCTEXT("NoSpeedCheck", "{0}: client-authoritative without MaxClientMoveSpeed. Any owning client can teleport its pawn."),
            FText::FromString(GetName())));
    }

    if (bDisplayInPresentNearLocalPlayer && PresentTimeFarDistance <= PresentTimeNearDistance)
    {
        Context.AddWarning(FText::Format(
            LOCTEXT("PresentDistances", "{0}: PresentTimeFarDistance must be greater than PresentTimeNearDistance."),
            FText::FromString(GetName())));
    }

    if (const AActor* Owner = GetOwner())
    {
        if (!Owner->GetIsReplicated())
        {
            Context.AddError(FText::Format(
                LOCTEXT("OwnerNotReplicated", "{0}: the owning actor does not replicate (bReplicates is false). Smooth Sync will do nothing."),
                FText::FromString(GetName())));
            Result = EDataValidationResult::Invalid;
        }
    }

    return Result;
}
#endif


// ===== Tick dispatch =====

void USmoothSyncComponent::SmoothSyncTick(const FSmoothSyncFrameContext& InContext)
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner) || !Owner->GetIsReplicated())
        return;

    if (CharacterMovement.IsValid())
    {
        TickCharacter(InContext);
        return;
    }

    switch (Owner->GetLocalRole())
    {
    case ROLE_Authority:
        if (IsDrivenByRemoteClient(InContext))
            TickInterpolation(InContext);
        else
            TickSending(InContext, true);

#if ENABLE_DRAW_DEBUG
        if (IsDebugDrawEnabled(&InContext))
            DrawDebugServer(InContext);
#endif
        break;

    case ROLE_AutonomousProxy:
        if (bIsClientAuthoritative)
            TickSending(InContext, false);
        else
            TickAutonomousProxy(InContext);
        break;

    case ROLE_SimulatedProxy:
        TickInterpolation(InContext);
        break;

    default:
        break;
    }
}

// ===== Characters =====

bool USmoothSyncComponent::IsOwnerCharacter() const
{
    if (CharacterMovement.IsValid())
        return true;

    // Called by replication setup, possibly before BeginPlay cached it.
    const AActor* Owner = GetOwner();
    return Owner && Owner->FindComponentByClass<UCharacterMovementComponent>() != nullptr;
}

bool USmoothSyncComponent::GetRemoteMoveTime(const FSmoothSyncFrameContext& InContext, double& OutMoveTime)
{
    const UCharacterMovementComponent* Movement = CharacterMovement.Get();
    const APawn* Pawn = Cast<APawn>(GetOwner());
    if (!Movement || !Pawn || !Pawn->IsPlayerControlled() || Pawn->IsLocallyControlled() || !Movement->HasPredictionData_Server())
        return false;

    const double ClientTimeStamp = Movement->GetPredictionData_Server_Character()->CurrentClientTimeStamp;
    if (ClientTimeStamp <= 0.0)
        return false;

    // The least delayed move gives the true clock offset: take lower offsets at once, follow higher ones slowly (drift).
    // Clients reset their move clock every few minutes: a backward jump restarts the estimate.
    const double RawOffset = InContext.ServerTime - ClientTimeStamp;
    if (!bHasClientMoveTimeOffset || ClientTimeStamp < LastClientMoveTimeStamp - 1.0)
    {
        ClientMoveTimeOffset = RawOffset;
        bHasClientMoveTimeOffset = true;
        LastSampledMoveTime = -1.0;
    }
    else if (RawOffset < ClientMoveTimeOffset)
    {
        ClientMoveTimeOffset = RawOffset;
    }
    else
    {
        ClientMoveTimeOffset += (RawOffset - ClientMoveTimeOffset) * 0.002;
    }

    LastClientMoveTimeStamp = ClientTimeStamp;
    OutMoveTime = ClientTimeStamp + ClientMoveTimeOffset;
    return true;
}

void USmoothSyncComponent::TickCharacter(const FSmoothSyncFrameContext& InContext)
{
    switch (GetOwner()->GetLocalRole())
    {
    case ROLE_Authority:
        // The CMC moves the character; Smooth Sync only samples it for the remote copies.
        TickSending(InContext, true);
        ApplyCharacterTrust();
        CheckCharacterClientMoves(InContext);

#if ENABLE_DRAW_DEBUG
        if (IsDebugDrawEnabled(&InContext))
            DrawDebugServer(InContext);
#endif

        if (IsRemotePlayerOnListenServer(InContext))
        {
            TickListenServerSmoothing(InContext);
            return;
        }
        break;

    case ROLE_SimulatedProxy:
        // Remote copies: Smooth Sync replaces the CMC's own smoothing (unless disabled on this copy).
        SetCharacterMovementSmoothing(bDisableSmoothing);
        if (!bDisableSmoothing)
        {
            TickInterpolation(InContext);

            // Animation blueprints read the CMC velocity: give them the smoothed one.
            if (UCharacterMovementComponent* Movement = CharacterMovement.Get())
                Movement->Velocity = LastAppliedVelocity;
        }
        break;

    case ROLE_AutonomousProxy:
        // The owning client is predicted by its CMC; only its corrections are smoothed.
        TickOwnerCorrectionSmoothing(InContext);
        return;

    default:
        break;
    }

    // No visual smoothing in this role (anymore): drop any leftover offset.
    ClearVisualOffset();
}

bool USmoothSyncComponent::IsRemotePlayerOnListenServer(const FSmoothSyncFrameContext& InContext) const
{
    const APawn* Pawn = Cast<APawn>(GetOwner());
    return InContext.World->GetNetMode() == NM_ListenServer && Pawn && Pawn->IsPlayerControlled() && !Pawn->IsLocallyControlled();
}

void USmoothSyncComponent::TickListenServerSmoothing(const FSmoothSyncFrameContext& InContext)
{
    const UCharacterMovementComponent* Movement = CharacterMovement.Get();
    const USceneComponent* Root = GetOwner()->GetRootComponent();
    const bool bSmooth = !bDisableSmoothing && bSmoothRemotePlayersOnListenServer && ListenServerSmoothingTime > 0.0f;

    // Smooth Sync takes over the CMC's own listen server smoothing (which only covers the skeletal mesh).
    SetCharacterMovementSmoothing(!bSmooth);
    if (!bSmooth || !Movement || !Root)
    {
        ClearVisualOffset();
        return;
    }

    // The remote player's moves are applied in bursts, as its packets arrive. Show a location that keeps moving
    // with the character's velocity and eases toward the real one.
    const FVector Location = Root->GetComponentLocation();
    const float DeltaTime = InContext.DeltaTime;
    FVector Displayed = Location;
    if (bHasLastDisplayedLocation && DeltaTime > 0.0f)
    {
        const FVector Predicted = LastDisplayedLocation + Movement->Velocity * DeltaTime;
        Displayed = FMath::Lerp(Predicted, Location, 1.0f - FMath::Exp(-DeltaTime / ListenServerSmoothingTime));

        // On the ground the CMC velocity is horizontal: follow slopes and steps exactly.
        if (Movement->IsMovingOnGround())
            Displayed.Z = Location.Z;

        // Teleports and large corrections snap.
        if (FVector::DistSquared(Displayed, Location) > FMath::Square(MaxOwnerCorrectionSmoothingDistance))
            Displayed = Location;
    }

    LastDisplayedLocation = Displayed;
    bHasLastDisplayedLocation = true;
    VisualOffset = Displayed - Location;
    ApplyVisualOffset(VisualOffset);
}

void USmoothSyncComponent::ClearVisualOffset()
{
    if (!AppliedVisualLocalOffset.IsZero())
    {
        VisualOffset = FVector::ZeroVector;
        ApplyVisualOffset(FVector::ZeroVector);
    }

    bHasLastOwnerLocation = false;
    bHasLastDisplayedLocation = false;
}

void USmoothSyncComponent::TickOwnerCorrectionSmoothing(const FSmoothSyncFrameContext& InContext)
{
    const UCharacterMovementComponent* Movement = CharacterMovement.Get();
    const USceneComponent* Root = GetOwner()->GetRootComponent();
    if (!Movement || !Root)
        return;

    const FVector Location = Root->GetComponentLocation();
    const float DeltaTime = InContext.DeltaTime;

    if (!bSmoothOwnerCorrections || OwnerCorrectionSmoothingTime <= 0.0f)
    {
        VisualOffset = FVector::ZeroVector;
    }
    else
    {
        FVector Correction = FVector::ZeroVector;
        if (USmoothSyncCharacterMovementComponent* SmoothSyncMovement = Cast<USmoothSyncCharacterMovementComponent>(CharacterMovement.Get()))
        {
            // Exact: measured by the movement component around the correction replay.
            Correction = SmoothSyncMovement->ConsumeCorrectionDelta();
        }
        else if (bHasLastOwnerLocation && DeltaTime > 0.0f)
        {
            // Stock CMC: a jump its velocity doesn't explain. On the ground the velocity is kept horizontal while
            // slopes and steps move the character vertically, so only horizontal jumps count there.
            Correction = (Location - LastOwnerLocation) - Movement->Velocity * DeltaTime;
            if (Movement->IsMovingOnGround())
                Correction.Z = 0.0;
            if (Correction.SizeSquared() <= FMath::Square(FMath::Max(2.0, Movement->Velocity.Size() * DeltaTime * 0.35)))
                Correction = FVector::ZeroVector;
        }

        if (Correction.SizeSquared() >= FMath::Square(MaxOwnerCorrectionSmoothingDistance))
            VisualOffset = FVector::ZeroVector;
        else
            VisualOffset -= Correction;

        // Many small corrections in a row (pushing physics props) must not pile up into a large offset that would
        // then slide back fast and visibly.
        constexpr double MaxAccumulatedOffset = 60.0;
        VisualOffset = VisualOffset.GetClampedToMaxSize(MaxAccumulatedOffset);

        VisualOffset *= FMath::Exp(-DeltaTime / OwnerCorrectionSmoothingTime);
        if (VisualOffset.SizeSquared() < 0.01)
            VisualOffset = FVector::ZeroVector;
    }

    LastOwnerLocation = Location;
    bHasLastOwnerLocation = true;
    ApplyVisualOffset(VisualOffset);
}

void USmoothSyncComponent::ApplyVisualOffset(const FVector& InWorldOffset)
{
    const USceneComponent* Root = GetOwner()->GetRootComponent();
    if (!Root)
        return;

    // Applied as a delta, so the game can still move these components itself.
    const FVector LocalOffset = Root->GetComponentTransform().InverseTransformVector(InWorldOffset);
    const FVector Delta = LocalOffset - AppliedVisualLocalOffset;
    if (Delta.IsNearlyZero(0.001))
        return;

    for (USceneComponent* Child : Root->GetAttachChildren())
    {
        if (Child && !Child->IsUsingAbsoluteLocation())
            Child->AddRelativeLocation(Delta);
    }

    AppliedVisualLocalOffset = LocalOffset;
}

void USmoothSyncComponent::SetCharacterMovementSmoothing(bool bInUseCharacterMovementSmoothing)
{
    UCharacterMovementComponent* Movement = CharacterMovement.Get();
    if (!Movement || bCharacterSmoothingOverridden != bInUseCharacterMovementSmoothing)
        return;

    if (bInUseCharacterMovementSmoothing)
    {
        Movement->NetworkSmoothingMode = static_cast<ENetworkSmoothingMode>(SavedNetworkSmoothingMode);
        bCharacterSmoothingOverridden = false;
    }
    else
    {
        // Otherwise the CMC offsets the mesh on every replicated update, on top of our interpolation.
        SavedNetworkSmoothingMode = static_cast<uint8>(Movement->NetworkSmoothingMode);
        Movement->NetworkSmoothingMode = ENetworkSmoothingMode::Disabled;
        bCharacterSmoothingOverridden = true;
    }
}

void USmoothSyncComponent::ApplyCharacterTrust()
{
    UCharacterMovementComponent* Movement = CharacterMovement.Get();
    const bool bTrust = bIsClientAuthoritative && !bCharacterTrustRevoked;
    if (!Movement || bCharacterTrustApplied == bTrust)
        return;

    // CMC client authority: the server still simulates the owner's moves (physics props get pushed),
    // then takes the owner's position instead of correcting it.
    Movement->bIgnoreClientMovementErrorChecksAndCorrection = bTrust;
    Movement->bServerAcceptClientAuthoritativePosition = bTrust;
    bCharacterTrustApplied = bTrust;
}

void USmoothSyncComponent::CheckCharacterClientMoves(const FSmoothSyncFrameContext& InContext)
{
    const APawn* Pawn = Cast<APawn>(GetOwner());
    if (!bIsClientAuthoritative || !Pawn || !Pawn->IsPlayerControlled() || Pawn->IsLocallyControlled())
        return;

    const double Now = InContext.ServerTime;
    const FVector Location = Pawn->GetActorLocation();

    // Speed is measured against the time the client's moves span, not the server's clock: after packet loss, several
    // moves arrive at once and would otherwise look like a speed hack.
    const UCharacterMovementComponent* Movement = CharacterMovement.Get();
    const double MoveTime = Movement && Movement->HasPredictionData_Server() ? Movement->GetPredictionData_Server_Character()->CurrentClientTimeStamp : Now;

    if (bCharacterTrustRevoked && Now >= CharacterTrustRevokedUntil)
    {
        bCharacterTrustRevoked = false;
        ApplyCharacterTrust();
    }

    if (MaxClientMoveSpeed <= 0.0f)
    {
        WarnOncePerClass(Pawn, TEXT("NoSpeedCheck"), FString::Printf(
            TEXT("%s: client-authoritative without MaxClientMoveSpeed. Any owning client can teleport its pawn."), *Pawn->GetName()));
        return;
    }

    // Checked over windows of a quarter second. Clients reset their move clock every few minutes: restart on a backward jump.
    if (LastCharacterCheckTime < 0.0 || MoveTime < LastCharacterCheckTime)
    {
        LastCharacterCheckTime = MoveTime;
        LastCheckedCharacterLocation = Location;
        return;
    }

    const double Elapsed = MoveTime - LastCharacterCheckTime;
    if (Elapsed < 0.25)
        return;

    const double AllowedDistance = MaxClientMoveSpeed * Elapsed * 1.25 + PredictionErrorTolerance;
    if (!bCharacterTrustRevoked && FVector::Dist2D(Location, LastCheckedCharacterLocation) > AllowedDistance)
    {
        UE_LOG(LogSmoothSync, Warning, TEXT("%s: client moved faster than MaxClientMoveSpeed (%.0f). Server takes over for a second."),
            *Pawn->GetName(), MaxClientMoveSpeed);

        // Back to the last valid spot, and let the CMC correct the client from there.
        GetOwner()->SetActorLocation(LastCheckedCharacterLocation, false, nullptr, ETeleportType::TeleportPhysics);
        bCharacterTrustRevoked = true;
        CharacterTrustRevokedUntil = Now + 1.0;
        ApplyCharacterTrust();
    }

    LastCharacterCheckTime = MoveTime;
    LastCheckedCharacterLocation = GetOwner()->GetActorLocation();
}

bool USmoothSyncComponent::IsDrivenByRemoteClient(const FSmoothSyncFrameContext& InContext) const
{
    // A client-authoritative pawn is interpolated from its owner's states while they keep coming.
    return bIsClientAuthoritative && LastClientStateTime >= 0.0 && InContext.ServerTime - LastClientStateTime < 1.0;
}


// ===== Sync space =====

const USceneComponent* USmoothSyncComponent::GetSyncParent(FName& OutSocket) const
{
    OutSocket = NAME_None;

    const AActor* Owner = GetOwner();
    const USceneComponent* Root = Owner ? Owner->GetRootComponent() : nullptr;
    if (!bSyncRelativeToParent || !Root)
        return nullptr;

    if (const USceneComponent* AttachParent = Root->GetAttachParent())
    {
        OutSocket = Root->GetAttachSocketName();
        return AttachParent;
    }

    // Remote copies use the base carried by the snapshots: their own movement base may lag behind or differ.
    if (Owner->GetLocalRole() == ROLE_SimulatedProxy)
        return (!PosBuffer.IsEmpty() && PosBuffer.Newest().bRelativeToParent) ? PosBuffer.Newest().MovementBase.Get() : nullptr;

    // Chosen by the game (cargo): physics bodies are fine here, a vehicle usually is one.
    if (const UPrimitiveComponent* Base = SyncBase.Get())
        return Base;

    // A Character standing on a moving platform (elevator, deck) moves with it. Physics objects don't count
    // (bumping a rolling ball is not riding it), and clients must be able to resolve the base.
    if (const UCharacterMovementComponent* Movement = CharacterMovement.Get())
    {
        const UPrimitiveComponent* Base = Movement->GetMovementBase();
        if (Base && MovementBaseUtility::IsDynamicBase(Base) && !Base->IsSimulatingPhysics() && Base->IsSupportedForNetworking())
            return Base;
    }

    return nullptr;
}

UPrimitiveComponent* USmoothSyncComponent::GetSyncMovementBase() const
{
    // Attach parents replicate with the actor; only a movement base has to travel with the snapshot.
    FName Socket;
    const USceneComponent* Parent = GetSyncParent(Socket);
    const USceneComponent* Root = GetOwner() ? GetOwner()->GetRootComponent() : nullptr;
    return (Parent && Root && Parent != Root->GetAttachParent()) ? const_cast<UPrimitiveComponent*>(Cast<UPrimitiveComponent>(Parent)) : nullptr;
}

FTransform USmoothSyncComponent::GetOwnerSyncSpaceTransform(bool& bOutRelative) const
{
    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        bOutRelative = false;
        return FTransform::Identity;
    }

    FName Socket;
    const USceneComponent* Parent = GetSyncParent(Socket);
    bOutRelative = Parent != nullptr;
    if (!Parent)
        return Owner->GetActorTransform();

    const USceneComponent* Root = Owner->GetRootComponent();
    if (Parent == Root->GetAttachParent())
        return Root->GetRelativeTransform();

    return Owner->GetActorTransform().GetRelativeTransform(Parent->GetSocketTransform(Socket));
}

FTransform USmoothSyncComponent::SyncSpaceToWorld(const FTransform& InTransform, bool bInRelative) const
{
    FName Socket;
    const USceneComponent* Parent = bInRelative ? GetSyncParent(Socket) : nullptr;
    return Parent ? InTransform * Parent->GetSocketTransform(Socket) : InTransform;
}

void USmoothSyncComponent::RestartInNewSpace(const FSmoothSyncState_Pos& InState)
{
    // The buffered states can't be re-expressed in the new space: the parent kept moving (possibly fast) since they
    // were captured. Start over from this snapshot, and keep the owner where it is on screen.
    const FTransform Displayed = GetOwner()->GetActorTransform();
    PosBuffer.Reset();
    RotBuffer.Reset();
    PosBuffer.Push(InState);
    SpaceStartTimestamp = InState.ServerTimestamp;
    bIsSettled = false;

    // Rotation: with no rotation snapshot yet, the current one is kept (see SampleRotation). Position: the jump to the
    // new snapshot is blended out, expressed in the new space.
    RotationErrorOffset = FQuat::Identity;
    PositionErrorOffset = FVector::ZeroVector;
    if (LastTargetTime < 0.0 || ErrorSmoothingTime <= 0.0f)
        return;

    FName Socket;
    const USceneComponent* Parent = InState.bRelativeToParent ? GetSyncParent(Socket) : nullptr;
    if (InState.bRelativeToParent && !Parent)
        return;

    const FVector Target = SyncSpaceToWorld(FTransform(SamplePosition(LastTargetTime, LastMaxExtrapolation).Position), InState.bRelativeToParent).GetLocation();
    const FVector WorldOffset = Displayed.GetLocation() - Target;
    if (WorldOffset.SizeSquared() < FMath::Square(TeleportDistanceThreshold))
        PositionErrorOffset = Parent ? Parent->GetSocketTransform(Socket).InverseTransformVector(WorldOffset) : WorldOffset;
}


// ===== Sending (server, or client-authoritative owner) =====

float USmoothSyncComponent::GetLODRate(float InBaseRate, const FSmoothSyncFrameContext& InContext) const
{
    float Rate = FMath::Max(InBaseRate, 1.0f);
    if (!bEnableDistanceLOD || InContext.ViewerLocations.IsEmpty())
        return Rate;

    const FVector OwnerLocation = GetOwner()->GetActorLocation();
    double MinDistSq = TNumericLimits<double>::Max();
    for (const FVector& ViewerLocation : InContext.ViewerLocations)
        MinDistSq = FMath::Min(MinDistSq, FVector::DistSquared(ViewerLocation, OwnerLocation));

    if (MinDistSq > FMath::Square(LODFarDistance))
        Rate = FMath::Min(Rate, FMath::Max(LODFarRate, 1.0f));
    else if (MinDistSq > FMath::Square(LODMediumDistance))
        Rate = FMath::Min(Rate, FMath::Max(LODMediumRate, 1.0f));

    return Rate;
}

bool USmoothSyncComponent::CanUseDormancy() const
{
    const AActor* Owner = GetOwner();
    // Characters keep replicating through their CMC: never put them to sleep.
    if (!bUseDormancyWhenAtRest || bIsClientAuthoritative || !Owner->GetIsReplicated() || CharacterMovement.IsValid())
        return false;

    // Player pawns carry RPCs and gameplay state: never put them to sleep.
    if (const APawn* Pawn = Cast<APawn>(Owner))
    {
        if (Pawn->IsPlayerControlled())
            return false;
    }

    // Respect dormancy set up by the game; only manage awake (or initially dormant) actors.
    return bIsDormant || Owner->NetDormancy == DORM_Awake || Owner->NetDormancy == DORM_Initial;
}

void USmoothSyncComponent::SetDormant(bool bInDormant)
{
    if (bIsDormant == bInDormant)
        return;

    bIsDormant = bInDormant;
    GetOwner()->SetNetDormancy(bInDormant ? DORM_DormantAll : DORM_Awake);
}

void USmoothSyncComponent::TickSending(const FSmoothSyncFrameContext& InContext, bool bInIsServer)
{
    SCOPE_CYCLE_COUNTER(STAT_SmoothSync_Send);

    AActor* Owner = GetOwner();

    if (bInIsServer)
        UpdateOwnerReplicationCondition();

    const float PosInterval = 1.0f / (bInIsServer ? GetLODRate(PositionUpdateRate, InContext) : FMath::Max(PositionUpdateRate, 1.0f));
    const float RotInterval = 1.0f / (bInIsServer ? GetLODRate(RotationUpdateRate, InContext) : FMath::Max(RotationUpdateRate, 1.0f));

    PosTimeSinceLastSync += InContext.DeltaTime;
    RotTimeSinceLastSync += InContext.DeltaTime;

    // Nothing sent for a while and the last snapshots say "at rest": stop replicating the actor.
    auto TryGoDormant = [this, &InContext, bInIsServer]()
    {
        if (bInIsServer && !bIsDormant && CanUseDormancy() && LastSentPos.Velocity.IsZero() && LastSentRot.AngularVelocity.IsZero()
            && InContext.ServerTime - LastChangeTime > DormancyDelay)
        {
            SetDormant(true);
        }
    };

    const bool bPosDue = bForceSendPos || PosTimeSinceLastSync >= PosInterval;
    const bool bRotDue = bForceSendRot || RotTimeSinceLastSync >= RotInterval;
    if (!bPosDue && !bRotDue)
    {
        TryGoDormant();
        return;
    }

    // Characters driven by a remote player are stamped with the time their move was made, and only sampled when a
    // new move arrived: otherwise bursty move arrival shows up as stutter (and pauses as false stops) on remote copies.
    double Now = InContext.ServerTime;
    double RemoteMoveTime = 0.0;
    const bool bRemoteMoves = bInIsServer && GetRemoteMoveTime(InContext, RemoteMoveTime);
    if (bRemoteMoves)
    {
        if (RemoteMoveTime <= LastSampledMoveTime && !bForceSendPos && !bForceSendRot)
            return;

        LastSampledMoveTime = RemoteMoveTime;
        Now = RemoteMoveTime;
    }

    bool bRelative = false;
    const FTransform SyncSpace = GetOwnerSyncSpaceTransform(bRelative);

    // Changing parent: receivers see the new space in the snapshots and restart smoothly (see RestartInNewSpace), so
    // only restart the sampling. Attach parents aren't carried by the snapshots: from one straight to another, receivers snap.
    FName Socket;
    const USceneComponent* SyncParent = GetSyncParent(Socket);
    if (SyncParent != LastSyncParent.Get())
    {
        const bool bWasAttached = LastSyncParent.IsValid() && LastSentPos.bRelativeToParent && !LastSentPos.MovementBase;
        if (bWasAttached && SyncParent && SyncParent == Owner->GetRootComponent()->GetAttachParent())
            MarkTeleported();

        LastSyncParent = SyncParent;
        LastSamplePosTime = -1.0;
        LastSampleRotTime = -1.0;
        bForceSendPos = true;
        bForceSendRot = true;
    }

    bool bPosChanged = false;
    bool bRotChanged = false;

    if (bForceSendPos || PosTimeSinceLastSync >= PosInterval)
    {
        PosTimeSinceLastSync = bForceSendPos ? 0.0f : FMath::Fmod(PosTimeSinceLastSync, PosInterval);

        const FVector Position = SyncSpace.GetLocation();

        // Kinematic and attached actors, and remote players (the server may keep the client's position but its own
        // simulated velocity): derive the velocity from the samples so it always agrees with the positions.
        FVector Velocity = (bRelative || bRemoteMoves) ? FVector::ZeroVector : Owner->GetVelocity();
        if (Velocity.IsNearlyZero() && LastSamplePosTime >= 0.0 && Now > LastSamplePosTime)
            Velocity = (Position - LastSamplePosition) / (Now - LastSamplePosTime);

        LastSamplePosition = Position;
        LastSamplePosTime = Now;

        const bool bMoved = !Position.Equals(LastSentPos.Position, PositionSyncTolerance);
        if (!bMoved)
            Velocity = FVector::ZeroVector;

        // One last snapshot with zero velocity, otherwise receivers keep extrapolating past the stop point.
        const bool bCameToRest = !bMoved && !LastSentPos.Velocity.IsZero();

        const FVector Scale = SyncSpace.GetScale3D();
        const bool bScaleChanged = bSyncScale && !Scale.Equals(LastSentPos.Scale, ScaleSyncTolerance);

        if (bForceSendPos || bMoved || bCameToRest || bScaleChanged)
        {
            LastSentPos.Position = Position;
            LastSentPos.Velocity = Velocity;
            LastSentPos.Scale = bSyncScale ? Scale : FVector::OneVector;
            LastSentPos.bHasScale = bSyncScale;
            LastSentPos.bRelativeToParent = bRelative;
            LastSentPos.MovementBase = GetSyncMovementBase();
            LastSentPos.Precision = PositionPrecision;
            LastSentPos.ServerTimestamp = Now;
            LastSentPos.TeleportId = LocalTeleportId;
            bPosChanged = true;
        }

        bForceSendPos = false;
    }

    if (bForceSendRot || RotTimeSinceLastSync >= RotInterval)
    {
        RotTimeSinceLastSync = bForceSendRot ? 0.0f : FMath::Fmod(RotTimeSinceLastSync, RotInterval);

        const FQuat Rotation = SyncSpace.GetRotation();

        FVector AngularVelocity = FVector::ZeroVector;
        if (bSyncAngularVelocity)
        {
            const UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
            if (!bRelative && RootPrim && RootPrim->IsSimulatingPhysics())
                AngularVelocity = RootPrim->GetPhysicsAngularVelocityInRadians();
            else if (LastSampleRotTime >= 0.0)
                AngularVelocity = AngularVelocityBetween(LastSampleRotation, Rotation, Now - LastSampleRotTime);
        }

        LastSampleRotation = Rotation;
        LastSampleRotTime = Now;

        const bool bRotated = !Rotation.Equals(LastSentRot.Rotation, RotationSyncTolerance);
        if (!bRotated)
            AngularVelocity = FVector::ZeroVector;

        const bool bStoppedSpinning = !bRotated && !LastSentRot.AngularVelocity.IsZero();

        if (bForceSendRot || bRotated || bStoppedSpinning)
        {
            LastSentRot.Rotation = Rotation;
            LastSentRot.AngularVelocity = AngularVelocity;
            LastSentRot.bRelativeToParent = bRelative;
            LastSentRot.ServerTimestamp = Now;
            LastSentRot.TeleportId = LocalTeleportId;
            bRotChanged = true;
        }

        bForceSendRot = false;
    }

    if (!bPosChanged && !bRotChanged)
    {
        TryGoDormant();
        return;
    }

    INC_DWORD_STAT_BY(STAT_SmoothSync_SnapshotsSent, (bPosChanged ? 1 : 0) + (bRotChanged ? 1 : 0));

    if (IsDebugDrawEnabled(&InContext))
        RecordDebugBandwidth((bPosChanged ? MeasureSerializedBits(LastSentPos) : 0) + (bRotChanged ? MeasureSerializedBits(LastSentRot) : 0));

    if (!bInIsServer)
    {
        Server_ReceiveClientState(LastSentPos, LastSentRot);
        return;
    }

    // Wake up before marking dirty so the change goes out on the next net update.
    LastChangeTime = Now;
    if (bIsDormant)
        SetDormant(false);

    if (bPosChanged)
    {
        SyncPos = LastSentPos;
        MARK_PROPERTY_DIRTY_FROM_NAME(USmoothSyncComponent, SyncPos, this);
    }

    if (bRotChanged)
    {
        SyncRot = LastSentRot;
        MARK_PROPERTY_DIRTY_FROM_NAME(USmoothSyncComponent, SyncRot, this);
    }
}

void USmoothSyncComponent::Server_ReceiveClientState_Implementation(const FSmoothSyncState_Pos& InPos, const FSmoothSyncState_Rot& InRot)
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
        return;

    // The RPC is callable by the owning client whatever the settings: never trust it unless the server allows it.
    if (!bIsClientAuthoritative)
    {
        WarnOncePerClass(Owner, TEXT("UnexpectedClientState"), FString::Printf(
            TEXT("%s: ignored a client transform because bIsClientAuthoritative is false on the server."), *Owner->GetName()));
        return;
    }

    if (InPos.Position.ContainsNaN() || InPos.Velocity.ContainsNaN() || InRot.Rotation.ContainsNaN() || InRot.AngularVelocity.ContainsNaN())
        return;

    if (MaxClientMoveSpeed <= 0.0f)
    {
        WarnOncePerClass(Owner, TEXT("NoSpeedCheck"), FString::Printf(
            TEXT("%s: client-authoritative without MaxClientMoveSpeed. Any owning client can teleport its pawn."), *Owner->GetName()));
    }

    // After the server moved the owner, states it sent before learning about it would pull it back: drop them.
    const uint8 ClientTeleportId = InPos.TeleportId & SmoothSync::TeleportIdMask;
    if (bAwaitingClientTeleport)
    {
        if (ClientTeleportId != ExpectedClientTeleportId)
            return;

        bAwaitingClientTeleport = false;
    }

    const double Now = GetWorld()->GetTimeSeconds();
    const bool bTeleport = ClientTeleportId != LastClientTeleportId;

    // Compare with the previous state received, not the displayed (delayed) one.
    if (!bTeleport && MaxClientMoveSpeed > 0.0f && LastClientStateTime >= 0.0)
    {
        const double Elapsed = FMath::Clamp(Now - LastClientStateTime, 1.0 / 60.0, 1.0);
        const double AllowedDistance = MaxClientMoveSpeed * Elapsed + PredictionErrorTolerance;

        if (FVector::DistSquared(InPos.Position, LastClientPosition) > FMath::Square(AllowedDistance))
        {
            UE_LOG(LogSmoothSync, Warning, TEXT("%s: rejected client state moving faster than MaxClientMoveSpeed (%.0f). Correcting the client."),
                *Owner->GetName(), MaxClientMoveSpeed);

            ForceClientToCurrentTransform();
            LastClientStateTime = Now;
            return;
        }
    }

    LastClientStateTime = Now;
    LastClientPosition = InPos.Position;

    if (bTeleport)
    {
        LastClientTeleportId = ClientTeleportId;
        LocalTeleportId = (LocalTeleportId + 1) & SmoothSync::TeleportIdMask;
    }

    // Interpolate the owner's states on the server, like a remote copy (see TickInterpolation).
    ReceivePositionState(InPos, Now);
    ReceiveRotationState(InRot, Now);

    // Relay the original states (capture time included) to the other clients.
    UpdateOwnerReplicationCondition();

    SyncPos = InPos;
    SyncPos.ServerTimestamp = InPos.ResolveTimestamp(Now);
    SyncPos.TeleportId = LocalTeleportId;
    SyncPos.bDecodedFromWire = false;
    MARK_PROPERTY_DIRTY_FROM_NAME(USmoothSyncComponent, SyncPos, this);

    SyncRot = InRot;
    SyncRot.ServerTimestamp = InRot.ResolveTimestamp(Now);
    SyncRot.TeleportId = LocalTeleportId;
    SyncRot.bDecodedFromWire = false;
    MARK_PROPERTY_DIRTY_FROM_NAME(USmoothSyncComponent, SyncRot, this);
}

void USmoothSyncComponent::Client_ForceTransform_Implementation(FVector_NetQuantize100 InPosition, FQuat InRotation)
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
        return;

    Owner->SetActorLocationAndRotation(InPosition, InRotation, false, nullptr, ETeleportType::TeleportPhysics);
    NotifyTeleported();
    OnHardSnapTriggered.Broadcast();
}

void USmoothSyncComponent::ForceClientToCurrentTransform()
{
    const AActor* Owner = GetOwner();

    // The owner answers by teleporting, which bumps its teleport id: until then, its states are outdated.
    bAwaitingClientTeleport = true;
    ExpectedClientTeleportId = (LastClientTeleportId + 1) & SmoothSync::TeleportIdMask;
    LastClientPosition = Owner->GetActorLocation();
    ResetInterpolation();

    Client_ForceTransform(Owner->GetActorLocation(), Owner->GetActorQuat());
}

void USmoothSyncComponent::RelayCurrentTransform()
{
    bool bRelative = false;
    const FTransform SyncSpace = GetOwnerSyncSpaceTransform(bRelative);
    const double Now = GetWorld()->GetTimeSeconds();

    SyncPos = FSmoothSyncState_Pos();
    SyncPos.Position = SyncSpace.GetLocation();
    SyncPos.Scale = bSyncScale ? SyncSpace.GetScale3D() : FVector::OneVector;
    SyncPos.bHasScale = bSyncScale;
    SyncPos.bRelativeToParent = bRelative;
    SyncPos.MovementBase = GetSyncMovementBase();
    SyncPos.Precision = PositionPrecision;
    SyncPos.ServerTimestamp = Now;
    SyncPos.TeleportId = LocalTeleportId;
    MARK_PROPERTY_DIRTY_FROM_NAME(USmoothSyncComponent, SyncPos, this);

    SyncRot = FSmoothSyncState_Rot();
    SyncRot.Rotation = SyncSpace.GetRotation();
    SyncRot.bRelativeToParent = bRelative;
    SyncRot.ServerTimestamp = Now;
    SyncRot.TeleportId = LocalTeleportId;
    MARK_PROPERTY_DIRTY_FROM_NAME(USmoothSyncComponent, SyncRot, this);
}


// ===== Receiving =====

double USmoothSyncComponent::GetEstimatedServerTime() const
{
    const UWorld* World = GetWorld();
    if (!World)
        return 0.0;

    const AGameStateBase* GameState = World->GetGameState();
    return GameState ? GameState->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
}

void USmoothSyncComponent::OnRep_SyncPos()
{
    ReceivePositionState(SyncPos, GetEstimatedServerTime());
}

void USmoothSyncComponent::OnRep_SyncRot()
{
    ReceiveRotationState(SyncRot, GetEstimatedServerTime());
}

bool USmoothSyncComponent::ReceivePositionState(const FSmoothSyncState_Pos& InState, double InServerNow)
{
    FSmoothSyncState_Pos State = InState;
    State.ServerTimestamp = InState.ResolveTimestamp(InServerNow);
    State.TeleportId &= SmoothSync::TeleportIdMask;

    double IntervalSincePrevious = -1.0;
    bool bRestarted = false;

    if (!PosBuffer.IsEmpty())
    {
        const FSmoothSyncState_Pos& Newest = PosBuffer.Newest();

        // Teleports snap; stepping on or off a moving parent (or onto another one) restarts without a visible jump.
        if (State.TeleportId != Newest.TeleportId)
        {
            PosBuffer.Reset();
            bPendingTeleport = true;
            bRestarted = true;
        }
        else if (State.ServerTimestamp > Newest.ServerTimestamp
            && (State.bRelativeToParent != Newest.bRelativeToParent || State.MovementBase != Newest.MovementBase))
        {
            PosArrival.AddSample(InServerNow - State.ServerTimestamp, State.ServerTimestamp - Newest.ServerTimestamp);
            RestartInNewSpace(State);
            return true;
        }
        else if (State.ServerTimestamp <= Newest.ServerTimestamp)
        {
            // Duplicate or out of order: drop. A large backward jump means the clock estimate was reset: restart.
            if (Newest.ServerTimestamp - State.ServerTimestamp < 1.0)
                return false;

            PosBuffer.Reset();
            bRestarted = true;
        }
        else
        {
            IntervalSincePrevious = State.ServerTimestamp - Newest.ServerTimestamp;
        }
    }

    // Measure how much this snapshot moves what is on screen, to blend it out instead of popping.
    const bool bMeasureError = !bRestarted && !PosBuffer.IsEmpty() && LastTargetTime >= 0.0 && ErrorSmoothingTime > 0.0f;
    const FVector Before = bMeasureError ? SamplePosition(LastTargetTime, LastMaxExtrapolation).Position : FVector::ZeroVector;

    PosArrival.AddSample(InServerNow - State.ServerTimestamp, IntervalSincePrevious);
    PosBuffer.Push(State);
    bIsSettled = false;

    if (bMeasureError)
    {
        PositionErrorOffset += Before - SamplePosition(LastTargetTime, LastMaxExtrapolation).Position;
        if (PositionErrorOffset.SizeSquared() > FMath::Square(TeleportDistanceThreshold))
            PositionErrorOffset = FVector::ZeroVector;
    }
    else if (bRestarted)
    {
        PositionErrorOffset = FVector::ZeroVector;
    }

    INC_DWORD_STAT(STAT_SmoothSync_SnapshotsReceived);
    if (IsDebugDrawEnabled())
        RecordDebugBandwidth(MeasureSerializedBits(State));

    return true;
}

bool USmoothSyncComponent::ReceiveRotationState(const FSmoothSyncState_Rot& InState, double InServerNow)
{
    FSmoothSyncState_Rot State = InState;
    State.ServerTimestamp = InState.ResolveTimestamp(InServerNow);
    State.TeleportId &= SmoothSync::TeleportIdMask;

    // The position snapshots set the space (see RestartInNewSpace): rotations from another space are outdated.
    if (!PosBuffer.IsEmpty() && (State.bRelativeToParent != PosBuffer.Newest().bRelativeToParent
        || (State.ServerTimestamp < SpaceStartTimestamp && SpaceStartTimestamp - State.ServerTimestamp < 1.0)))
    {
        return false;
    }

    double IntervalSincePrevious = -1.0;
    bool bRestarted = false;

    if (!RotBuffer.IsEmpty())
    {
        const FSmoothSyncState_Rot& Newest = RotBuffer.Newest();

        if (State.TeleportId != Newest.TeleportId)
        {
            RotBuffer.Reset();
            bPendingTeleport = true;
            bRestarted = true;
        }
        else if (State.ServerTimestamp <= Newest.ServerTimestamp)
        {
            if (Newest.ServerTimestamp - State.ServerTimestamp < 1.0)
                return false;

            RotBuffer.Reset();
            bRestarted = true;
        }
        else
        {
            IntervalSincePrevious = State.ServerTimestamp - Newest.ServerTimestamp;
        }
    }

    // Also measured into an empty buffer (after RestartInNewSpace): the owner then holds its current rotation.
    const bool bMeasureError = !bRestarted && LastTargetTime >= 0.0 && ErrorSmoothingTime > 0.0f;
    const FQuat Before = bMeasureError ? SampleRotation(LastTargetTime, LastMaxExtrapolation) : FQuat::Identity;

    RotArrival.AddSample(InServerNow - State.ServerTimestamp, IntervalSincePrevious);
    RotBuffer.Push(State);
    bIsSettled = false;

    if (bMeasureError)
    {
        RotationErrorOffset = RotationErrorOffset * Before * SampleRotation(LastTargetTime, LastMaxExtrapolation).Inverse();
        RotationErrorOffset.Normalize();
    }
    else if (bRestarted)
    {
        RotationErrorOffset = FQuat::Identity;
    }

    INC_DWORD_STAT(STAT_SmoothSync_SnapshotsReceived);
    if (IsDebugDrawEnabled())
        RecordDebugBandwidth(MeasureSerializedBits(State));

    return true;
}


// ===== Interpolation =====

void USmoothSyncComponent::UpdateInterpolationDelay(float InDeltaTime)
{
    float TargetDelay = InterpolationDelay;

    if (bAdaptiveInterpolationDelay)
    {
        double Required = 0.0;
        if (PosArrival.HasEstimate())
            Required = FMath::Max(Required, PosArrival.GetRequiredDelay());
        if (RotArrival.HasEstimate())
            Required = FMath::Max(Required, RotArrival.GetRequiredDelay());

        TargetDelay = FMath::Clamp(static_cast<float>(Required), InterpolationDelay, FMath::Max(InterpolationDelay, MaxInterpolationDelay));
    }

    // Grow fast to stop starving, shrink slowly to avoid visible time warping.
    const float Speed = TargetDelay > CurrentInterpolationDelay ? 2.0f : 0.5f;
    CurrentInterpolationDelay = FMath::FInterpTo(CurrentInterpolationDelay, TargetDelay, InDeltaTime, Speed);
}

void USmoothSyncComponent::UpdatePresentTimeWeight(const FSmoothSyncFrameContext& InContext)
{
    float TargetWeight = 0.0f;

    if (bDisplayInPresentNearLocalPlayer && !InContext.bIsServer && InContext.LocalPawn && InContext.LocalPawn != GetOwner())
    {
        const float NearDistance = FMath::Max(PresentTimeNearDistance, 0.0f);
        const float FarDistance = FMath::Max(PresentTimeFarDistance, NearDistance + 1.0f);
        const float Distance = FVector::Dist(GetOwner()->GetActorLocation(), InContext.LocalPawnLocation);
        TargetWeight = 1.0f - FMath::SmoothStep(NearDistance, FarDistance, Distance);
    }

    PresentTimeWeight = FMath::FInterpTo(PresentTimeWeight, TargetWeight, InContext.DeltaTime, 4.0f);
    if (PresentTimeWeight < 0.001f && TargetWeight == 0.0f)
        PresentTimeWeight = 0.0f;
}

void USmoothSyncComponent::TickInterpolation(const FSmoothSyncFrameContext& InContext)
{
    SCOPE_CYCLE_COUNTER(STAT_SmoothSync_Interpolate);

    if (bDisableSmoothing || (PosBuffer.IsEmpty() && RotBuffer.IsEmpty()))
        return;

    UpdateInterpolationDelay(InContext.DeltaTime);

    if (bIsSettled)
        return;

    UpdatePresentTimeWeight(InContext);

    // Normally shown in the past; near the local player, shifted forward to the player's present.
    const double BaseTargetTime = InContext.ServerTime - CurrentInterpolationDelay;
    const double PresentShift = PresentTimeWeight * (CurrentInterpolationDelay + InContext.RoundTripTime);
    const double TargetTime = BaseTargetTime + PresentShift;
    const double MaxExtrapolation = MaxExtrapolationTime + PresentShift;

    const FPositionSample Sample = SamplePosition(TargetTime, MaxExtrapolation);
    const FQuat SampledRotation = SampleRotation(TargetTime, MaxExtrapolation);
    LastTargetTime = TargetTime;
    LastMaxExtrapolation = MaxExtrapolation;

    if (!PosBuffer.IsEmpty())
    {
        const FSmoothSyncState_Pos& Newest = PosBuffer.Newest();
        SetLagging(!Newest.Velocity.IsZero() && BaseTargetTime - Newest.ServerTimestamp > MaxExtrapolationTime);
    }

    const bool bTeleport = Sample.bSnapped || bPendingTeleport;
    if (Sample.bSnapped)
        NotifySnap(Sample.SnapTimestamp);

    if (bPendingTeleport)
    {
        bPendingTeleport = false;
        ++DebugSnapCount;
        INC_DWORD_STAT(STAT_SmoothSync_Snaps);
        OnHardSnapTriggered.Broadcast();
    }

    // Blend out what new snapshots contradicted.
    if (bTeleport || ErrorSmoothingTime <= 0.0f)
    {
        PositionErrorOffset = FVector::ZeroVector;
        RotationErrorOffset = FQuat::Identity;
    }
    else
    {
        const float Keep = FMath::Exp(-InContext.DeltaTime / ErrorSmoothingTime);
        PositionErrorOffset *= Keep;
        RotationErrorOffset = FQuat::Slerp(FQuat::Identity, RotationErrorOffset, Keep);

        if (PositionErrorOffset.SizeSquared() < 0.0001)
            PositionErrorOffset = FVector::ZeroVector;
        if (RotationErrorOffset.AngularDistance(FQuat::Identity) < 0.0001)
            RotationErrorOffset = FQuat::Identity;
    }

    const bool bRelative = !PosBuffer.IsEmpty() ? PosBuffer.Newest().bRelativeToParent : RotBuffer.Newest().bRelativeToParent;

    FVector Scale = Sample.Scale;
    if (!bSyncScale)
    {
        bool bCurrentRelative = false;
        Scale = GetOwnerSyncSpaceTransform(bCurrentRelative).GetScale3D();
    }

    const FVector AngularVelocity = RotBuffer.IsEmpty() ? FVector::ZeroVector : RotBuffer.Newest().AngularVelocity;
    const FTransform SyncSpaceTransform(RotationErrorOffset * SampledRotation, Sample.Position + PositionErrorOffset, Scale);
    ApplyTransformToOwner(SyncSpaceTransform, bRelative, Sample.Velocity, AngularVelocity, bTeleport);

    // Stop working once the final resting snapshots are reached; the next snapshot wakes us up.
    // Except on a movement base (not attached): at rest on it still means moving with it.
    FName Socket;
    const USceneComponent* Parent = bRelative ? GetSyncParent(Socket) : nullptr;
    const bool bFollowsBase = Parent && Parent != GetOwner()->GetRootComponent()->GetAttachParent();
    const bool bPosDone = PosBuffer.IsEmpty() || (TargetTime >= PosBuffer.Newest().ServerTimestamp && PosBuffer.Newest().Velocity.IsZero());
    const bool bRotDone = RotBuffer.IsEmpty() || (TargetTime >= RotBuffer.Newest().ServerTimestamp && RotBuffer.Newest().AngularVelocity.IsZero());
    bIsSettled = !bFollowsBase && bPosDone && bRotDone && PositionErrorOffset.IsZero() && RotationErrorOffset.Equals(FQuat::Identity, 0.0);

#if ENABLE_DRAW_DEBUG
    if (IsDebugDrawEnabled(&InContext))
        DrawDebugInterpolation(InContext, TargetTime, SyncSpaceToWorld(SyncSpaceTransform, bRelative).GetLocation(), bRelative);
#endif
}

USmoothSyncComponent::FPositionSample USmoothSyncComponent::SamplePosition(double InTargetTime, double InMaxExtrapolation) const
{
    FPositionSample Sample;

    if (PosBuffer.IsEmpty())
    {
        bool bRelative = false;
        const FTransform Current = GetOwnerSyncSpaceTransform(bRelative);
        Sample.Position = Current.GetLocation();
        Sample.Scale = Current.GetScale3D();
        return Sample;
    }

    const FSmoothSyncState_Pos& Newest = PosBuffer.Newest();

    // --- Ahead of the newest snapshot: extrapolate along its velocity, capped ---
    if (PosBuffer.Num() == 1 || InTargetTime >= Newest.ServerTimestamp)
    {
        const double Ahead = FMath::Max(0.0, InTargetTime - Newest.ServerTimestamp);
        const double Extrapolation = FMath::Min(Ahead, InMaxExtrapolation);

        Sample.Position = Newest.Position + Newest.Velocity * Extrapolation;
        Sample.Velocity = Ahead > InMaxExtrapolation ? FVector::ZeroVector : Newest.Velocity;
        Sample.Scale = Newest.Scale;
        return Sample;
    }

    // --- Behind the oldest snapshot: hold it ---
    const FSmoothSyncState_Pos& Oldest = PosBuffer.Oldest();
    const FSmoothSyncState_Pos* S0 = &Oldest;
    const FSmoothSyncState_Pos* S1 = &Oldest;
    if (InTargetTime <= Oldest.ServerTimestamp || !PosBuffer.FindSegment(InTargetTime, S0, S1))
    {
        Sample.Position = Oldest.Position;
        Sample.Velocity = Oldest.Velocity;
        Sample.Scale = Oldest.Scale;
        return Sample;
    }

    // --- Between two snapshots: cubic Hermite using the sent velocities as tangents ---

    // After a long rest, the motion only started just before S1: don't spread it over the whole gap.
    double SegmentStart = S0->ServerTimestamp;
    const double MaxSegmentDuration = PosArrival.GetMaxSegmentDuration();
    if (S1->ServerTimestamp - SegmentStart > MaxSegmentDuration)
        SegmentStart = S1->ServerTimestamp - MaxSegmentDuration;

    if (InTargetTime <= SegmentStart)
    {
        Sample.Position = S0->Position;
        Sample.Scale = S0->Scale;
        return Sample;
    }

    const double SegmentDuration = S1->ServerTimestamp - SegmentStart;
    if (SegmentDuration <= UE_KINDA_SMALL_NUMBER)
    {
        Sample.Position = S1->Position;
        Sample.Velocity = S1->Velocity;
        Sample.Scale = S1->Scale;
        return Sample;
    }

    // Snap only if the jump is far beyond what the velocities explain (fast vehicles at low rates are fine).
    const double ExpectedTravel = FMath::Max(S0->Velocity.Size(), S1->Velocity.Size()) * SegmentDuration;
    const double SnapDistance = TeleportDistanceThreshold + 1.5 * ExpectedTravel;
    if (FVector::DistSquared(S0->Position, S1->Position) > FMath::Square(SnapDistance))
    {
        Sample.Position = S1->Position;
        Sample.Velocity = S1->Velocity;
        Sample.Scale = S1->Scale;
        Sample.bSnapped = true;
        Sample.SnapTimestamp = S1->ServerTimestamp;
        return Sample;
    }

    const float Alpha = static_cast<float>((InTargetTime - SegmentStart) / SegmentDuration);

    FVector Vel0 = S0->Velocity;
    FVector Vel1 = S1->Velocity;
    if (Vel0.IsNearlyZero() && Vel1.IsNearlyZero())
    {
        Vel0 = (S1->Position - S0->Position) / SegmentDuration;
        Vel1 = Vel0;
    }

    Sample.Position = FMath::CubicInterp(S0->Position, Vel0 * SegmentDuration, S1->Position, Vel1 * SegmentDuration, Alpha);
    Sample.Velocity = FMath::Lerp(Vel0, Vel1, Alpha);
    Sample.Scale = FMath::Lerp(S0->Scale, S1->Scale, Alpha);
    return Sample;
}

FQuat USmoothSyncComponent::SampleRotation(double InTargetTime, double InMaxExtrapolation) const
{
    if (RotBuffer.IsEmpty())
    {
        bool bRelative = false;
        return GetOwnerSyncSpaceTransform(bRelative).GetRotation();
    }

    const FSmoothSyncState_Rot& Newest = RotBuffer.Newest();
    if (RotBuffer.Num() == 1 || InTargetTime >= Newest.ServerTimestamp)
    {
        const double Extrapolation = FMath::Clamp(InTargetTime - Newest.ServerTimestamp, 0.0, InMaxExtrapolation);
        return IntegrateRotation(Newest.Rotation, Newest.AngularVelocity, Extrapolation);
    }

    const FSmoothSyncState_Rot* S0 = nullptr;
    const FSmoothSyncState_Rot* S1 = nullptr;
    if (InTargetTime <= RotBuffer.Oldest().ServerTimestamp || !RotBuffer.FindSegment(InTargetTime, S0, S1))
        return RotBuffer.Oldest().Rotation;

    double SegmentStart = S0->ServerTimestamp;
    const double MaxSegmentDuration = RotArrival.GetMaxSegmentDuration();
    if (S1->ServerTimestamp - SegmentStart > MaxSegmentDuration)
        SegmentStart = S1->ServerTimestamp - MaxSegmentDuration;

    const double SegmentDuration = S1->ServerTimestamp - SegmentStart;
    if (InTargetTime <= SegmentStart)
        return S0->Rotation;
    if (SegmentDuration <= UE_KINDA_SMALL_NUMBER)
        return S1->Rotation;

    const float Alpha = static_cast<float>((InTargetTime - SegmentStart) / SegmentDuration);

    // With angular velocities, predict forward from S0 and backward from S1, then blend:
    // spins of more than half a turn between snapshots stay correct (a plain slerp would take the short way).
    if (!S0->AngularVelocity.IsZero() || !S1->AngularVelocity.IsZero())
    {
        const FQuat FromStart = IntegrateRotation(S0->Rotation, S0->AngularVelocity, InTargetTime - SegmentStart);
        const FQuat FromEnd = IntegrateRotation(S1->Rotation, S1->AngularVelocity, InTargetTime - S1->ServerTimestamp);
        return FQuat::Slerp(FromStart, FromEnd, Alpha);
    }

    return FQuat::Slerp(S0->Rotation, S1->Rotation, Alpha);
}


// ===== Autonomous proxy: soft correction toward the server =====

void USmoothSyncComponent::TickAutonomousProxy(const FSmoothSyncFrameContext& InContext)
{
    SCOPE_CYCLE_COUNTER(STAT_SmoothSync_OwnerCorrection);

    if (bDisableSmoothing || PosBuffer.IsEmpty() || PosBuffer.Newest().bRelativeToParent)
        return;

    AActor* Owner = GetOwner();
    const float DeltaTime = InContext.DeltaTime;

    // Where the server probably is now: its last snapshot moved forward along its velocity (capped, never dropped).
    const FSmoothSyncState_Pos& LatestServerPos = PosBuffer.Newest();
    const double TimeSinceServerUpdate = FMath::Clamp(InContext.ServerTime - LatestServerPos.ServerTimestamp, 0.0, static_cast<double>(MaxExtrapolationTime));
    const FVector ServerPosition = LatestServerPos.Position + LatestServerPos.Velocity * TimeSinceServerUpdate;

    const FVector LocalPosition = Owner->GetActorLocation();
    const double ErrorSq = FVector::DistSquared(LocalPosition, ServerPosition);
    PredictionError = FMath::Sqrt(ErrorSq);

    UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
    const bool bIsSimulating = RootPrim && RootPrim->IsSimulatingPhysics();

    if (ErrorSq > FMath::Square(TeleportDistanceThreshold))
    {
        Owner->SetActorLocation(ServerPosition, false, nullptr, ETeleportType::TeleportPhysics);
        if (bIsSimulating)
        {
            RootPrim->SetPhysicsLinearVelocity(LatestServerPos.Velocity);
            RootPrim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
        }

        NotifySnap(LatestServerPos.ServerTimestamp);
    }
    else if (ErrorSq > FMath::Square(PredictionErrorTolerance))
    {
        if (bIsSimulating)
        {
            FVector Error = ServerPosition - LocalPosition;
            Error.Z *= ZDampingFactor;

            const float ErrorSize = Error.Size();
            float SpringStiffness = BaseSpringStiffness;
            if (ErrorSize > ErrorThresholdForScaling)
                SpringStiffness += (ErrorSize - ErrorThresholdForScaling) * SpringStiffnessScale;

            const FVector NudgeAccel = (Error * SpringStiffness).GetClampedToMaxSize(MaxLinearNudgeAccel);
            RootPrim->SetPhysicsLinearVelocity(RootPrim->GetPhysicsLinearVelocity() + NudgeAccel * DeltaTime);
        }
        else
        {
            const FVector CorrectedPosition = FMath::VInterpTo(LocalPosition, ServerPosition, DeltaTime, PositionInterpSpeed);
            Owner->SetActorLocation(CorrectedPosition, false, nullptr, ETeleportType::None);
        }
    }

    if (!RotBuffer.IsEmpty())
    {
        // Same time compensation as the position: the server rotation keeps turning while its snapshot travels.
        const FSmoothSyncState_Rot& LatestServerRot = RotBuffer.Newest();
        FVector AngularVelocity = LatestServerRot.AngularVelocity;
        if (AngularVelocity.IsZero() && RotBuffer.Num() >= 2)
        {
            const FSmoothSyncState_Rot& Previous = RotBuffer.Get(1);
            AngularVelocity = AngularVelocityBetween(Previous.Rotation, LatestServerRot.Rotation, LatestServerRot.ServerTimestamp - Previous.ServerTimestamp);
        }

        const double TimeSinceRotUpdate = FMath::Clamp(InContext.ServerTime - LatestServerRot.ServerTimestamp, 0.0, static_cast<double>(MaxExtrapolationTime));
        const FQuat ServerRotation = IntegrateRotation(LatestServerRot.Rotation, AngularVelocity, TimeSinceRotUpdate);

        const FQuat LocalRotation = Owner->GetActorQuat();
        const float AngularDistance = LocalRotation.AngularDistance(ServerRotation);

        if (AngularDistance > FMath::DegreesToRadians(HardSnapAngleDegrees))
        {
            Owner->SetActorRotation(ServerRotation, ETeleportType::TeleportPhysics);
            if (bIsSimulating)
                RootPrim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
        }
        else if (AngularDistance > FMath::DegreesToRadians(SoftSnapAngleDegrees))
        {
            if (bIsSimulating)
            {
                FVector Axis;
                float Angle;
                (ServerRotation * LocalRotation.Inverse()).ToAxisAndAngle(Axis, Angle);
                if (Angle > PI)
                    Angle -= 2.0f * PI;

                const FVector AngularAccel = (Axis * Angle * AngularSpringStiffness).GetClampedToMaxSize(MaxAngularNudgeAccel);
                RootPrim->SetPhysicsAngularVelocityInDegrees(RootPrim->GetPhysicsAngularVelocityInDegrees() + FMath::RadiansToDegrees(AngularAccel) * DeltaTime);
            }
            else
            {
                Owner->SetActorRotation(FQuat::Slerp(LocalRotation, ServerRotation, FMath::Clamp(DeltaTime * RotationInterpSpeed, 0.0f, 1.0f)), ETeleportType::None);
            }
        }
    }

#if ENABLE_DRAW_DEBUG
    if (IsDebugDrawEnabled(&InContext))
        DrawDebugAutonomous(InContext, ServerPosition, LocalPosition);
#endif
}


// ===== Shared helpers =====

void USmoothSyncComponent::ApplyTransformToOwner(const FTransform& InSyncSpaceTransform, bool bInRelative, const FVector& InVelocity, const FVector& InAngularVelocity, bool bInTeleport)
{
    AActor* Owner = GetOwner();
    USceneComponent* Root = Owner->GetRootComponent();
    FName Socket;
    const USceneComponent* Parent = GetSyncParent(Socket);

    // Relative snapshots need the parent (attachment or movement base), which replicates separately: wait for it.
    if (bInRelative && !Parent)
        return;

    const FQuat ParentRotation = bInRelative ? Parent->GetSocketQuaternion(Socket) : FQuat::Identity;
    const FVector WorldVelocity = ParentRotation.RotateVector(InVelocity);
    const FVector WorldAngularVelocity = ParentRotation.RotateVector(InAngularVelocity);
    LastAppliedVelocity = WorldVelocity;

    if (bOwnerImplementsSyncable)
    {
        ISmoothSyncable::Execute_ApplySmoothedTransform(Owner, SyncSpaceToWorld(InSyncSpaceTransform, bInRelative), WorldVelocity);
        return;
    }

    UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Root);
    const bool bIsSimulating = RootPrim && RootPrim->IsSimulatingPhysics();
    const ETeleportType TeleportType = (bInTeleport || bIsSimulating) ? ETeleportType::TeleportPhysics : ETeleportType::None;

    // Attached: placed in the parent's space. On a movement base (not attached): placed in world space.
    if (bInRelative && Parent == Root->GetAttachParent())
        Root->SetRelativeTransform(InSyncSpaceTransform, false, nullptr, TeleportType);
    else
        Owner->SetActorTransform(SyncSpaceToWorld(InSyncSpaceTransform, bInRelative), false, nullptr, TeleportType);

    if (bIsSimulating)
    {
        // Relative velocities exclude the parent's own motion.
        RootPrim->SetPhysicsLinearVelocity(bInRelative ? WorldVelocity + Parent->GetComponentVelocity() : WorldVelocity);
        if (bSyncAngularVelocity)
            RootPrim->SetPhysicsAngularVelocityInRadians(WorldAngularVelocity);
    }
}

void USmoothSyncComponent::SetLagging(bool bInLagging)
{
    if (bIsLagging == bInLagging)
        return;

    bIsLagging = bInLagging;
    if (bIsLagging)
        OnNetworkLagDetected.Broadcast();
    else
        OnNetworkLagRecovered.Broadcast();
}

void USmoothSyncComponent::NotifySnap(double InStateTimestamp)
{
    // Fire once per snapshot, not every frame spent on it.
    if (InStateTimestamp == LastSnapTimestamp)
        return;

    LastSnapTimestamp = InStateTimestamp;
    ++DebugSnapCount;
    INC_DWORD_STAT(STAT_SmoothSync_Snaps);
    OnHardSnapTriggered.Broadcast();
}


// ===== Debug =====

bool USmoothSyncComponent::IsDebugDrawEnabled(const FSmoothSyncFrameContext* InContext) const
{
#if ENABLE_DRAW_DEBUG
    return bShowDebugPath || (InContext ? InContext->bDrawDebug : SmoothSync::IsGlobalDebugEnabled());
#else
    return false;
#endif
}

void USmoothSyncComponent::RecordDebugBandwidth(int32 InBits)
{
    const UWorld* World = GetWorld();
    if (!World)
        return;

    const double Now = World->GetTimeSeconds();
    DebugBitsThisWindow += InBits;

    const double Elapsed = Now - DebugWindowStart;
    if (Elapsed >= 1.0)
    {
        DebugBytesPerSecond = static_cast<float>(DebugBitsThisWindow / 8.0 / Elapsed);
        DebugBitsThisWindow = 0;
        DebugWindowStart = Now;
    }
}

void USmoothSyncComponent::DrawDebugInterpolation(const FSmoothSyncFrameContext& InContext, double InTargetTime, const FVector& InWorldLocation, bool bInRelative) const
{
#if ENABLE_DRAW_DEBUG
    UWorld* World = InContext.World;
    if (PosBuffer.IsEmpty())
        return;

    auto ToWorld = [this, bInRelative](const FVector& InPosition)
    {
        return SyncSpaceToWorld(FTransform(InPosition), bInRelative).GetLocation();
    };

    for (int32 i = 0; i < PosBuffer.Num() - 1; ++i)
    {
        const FVector P0 = ToWorld(PosBuffer.Get(i).Position);
        const FVector P1 = ToWorld(PosBuffer.Get(i + 1).Position);
        DrawDebugLine(World, P0, P1, FColor::Emerald, false, -1.f, 0, 1.f);
        DrawDebugPoint(World, P0, 5.f, FColor::Green, false, -1.f);
    }

    const FSmoothSyncState_Pos& Latest = PosBuffer.Newest();
    FColor StatusColor = FColor::Cyan;
    FString Status = TEXT("Interpolating");

    if (bIsLagging)
    {
        StatusColor = FColor::Red;
        Status = TEXT("LAG (extrapolation capped)");
    }
    else if (PresentTimeWeight > 0.01f)
    {
        StatusColor = FColor::Magenta;
        Status = FString::Printf(TEXT("Present time %.0f%%"), PresentTimeWeight * 100.f);
    }
    else if (InTargetTime > Latest.ServerTimestamp && !Latest.Velocity.IsZero())
    {
        StatusColor = FColor::Orange;
        Status = TEXT("Extrapolating");
    }

    const FString Text = FString::Printf(TEXT("%s\nDelay %.0f ms  Buffer %d  Rx %.0f B/s  Snaps %d%s"),
        *Status, CurrentInterpolationDelay * 1000.f, PosBuffer.Num(), DebugBytesPerSecond, DebugSnapCount,
        bInRelative ? TEXT("  [relative]") : TEXT(""));

    DrawDebugBox(World, InWorldLocation, FVector(12.f), StatusColor, false, -1.f, 0, 2.f);
    DrawDebugLine(World, ToWorld(Latest.Position), InWorldLocation, StatusColor, false, -1.f, 0, 0.5f);
    DrawDebugString(World, InWorldLocation + FVector(0, 0, 100.f), Text, nullptr, StatusColor, 0.0f, true);
#endif
}

void USmoothSyncComponent::DrawDebugAutonomous(const FSmoothSyncFrameContext& InContext, const FVector& InServerLocation, const FVector& InLocalLocation) const
{
#if ENABLE_DRAW_DEBUG
    UWorld* World = InContext.World;

    const FColor StatusColor = PredictionError > TeleportDistanceThreshold ? FColor::Red
        : PredictionError > PredictionErrorTolerance ? FColor::Orange
        : FColor::Green;

    const FString StatusText = FString::Printf(TEXT("Owner Correction (Error: %.1f)"), PredictionError);

    DrawDebugBox(World, InServerLocation, FVector(12.f), StatusColor, false, -1.f, 0, 2.f);
    DrawDebugLine(World, InServerLocation, InLocalLocation, StatusColor, false, -1.f, 0, 0.5f);
    DrawDebugString(World, InLocalLocation + FVector(0, 0, 100.f), StatusText, nullptr, StatusColor, 0.0f, true);
#endif
}

void USmoothSyncComponent::DrawDebugServer(const FSmoothSyncFrameContext& InContext) const
{
#if ENABLE_DRAW_DEBUG
    // Server text sits lower than the client text so both stay readable in single-process PIE.
    const FString Text = FString::Printf(TEXT("Server: %s  Tx %.0f B/s%s"),
        bIsDormant ? TEXT("dormant") : TEXT("awake"), DebugBytesPerSecond,
        IsDrivenByRemoteClient(InContext) ? TEXT("  [client-driven]") : TEXT(""));

    DrawDebugString(InContext.World, GetOwner()->GetActorLocation() + FVector(0, 0, 60.f), Text, nullptr,
        bIsDormant ? FColor::Silver : FColor::Yellow, 0.0f, true);
#endif
}

#undef LOCTEXT_NAMESPACE
