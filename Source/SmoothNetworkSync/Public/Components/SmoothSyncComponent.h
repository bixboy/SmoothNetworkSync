// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/SmoothSyncTypes.h"
#include "Core/SmoothSyncBuffer.h"
#include "SmoothSyncComponent.generated.h"

struct FSmoothSyncFrameContext;
class UCharacterMovementComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSmoothSyncLagDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSmoothSyncSnapDelegate);


/**
 * Replicates the owner's transform and smooths it on remote machines. Replaces bReplicateMovement.
 *
 * Setup: add it to a replicated actor (bReplicates = true). Movement replication is disabled automatically.
 *  - Server-driven objects: clients interpolate between server snapshots.
 *  - Player pawns, server-authoritative (default): the owning client is softly corrected toward the server.
 *  - Player pawns, client-authoritative (bIsClientAuthoritative): the owning client sends its transform,
 *    the server interpolates it and relays it to the other clients.
 *
 * Call NotifyTeleported() on the server after teleporting the owner so clients snap instead of sliding.
 * Implement ISmoothSyncable on the owner to apply the smoothed transform yourself.
 *
 * Characters (owners with a CharacterMovementComponent): the CMC keeps predicting the owning client and running the
 * server, and Smooth Sync takes over the remote copies (other players see them with adaptive interpolation).
 * With bIsClientAuthoritative, the server accepts the owner's position (no corrections, even when bumping physics),
 * still simulates its moves so physics props get pushed, and falls back to strict corrections when MaxClientMoveSpeed is exceeded.
 */
UCLASS(ClassGroup = (SmoothSync), meta = (BlueprintSpawnableComponent, DisplayName = "Smooth Sync"))
class SMOOTHNETWORKSYNC_API USmoothSyncComponent : public UActorComponent
{
    GENERATED_BODY()

    friend class USmoothSyncSubsystem;

public:

    USmoothSyncComponent();

    static constexpr int32 MaxBufferCapacity = 16;


    // ===== Setup =====

    /** Picks sensible values for a use case. Editing a related setting afterwards switches back to Custom. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Smooth Sync")
    ESmoothSyncPreset Preset = ESmoothSyncPreset::Custom;

    /** The owning client sends its own transform to the server. For player-driven pawns with local physics. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync")
    bool bIsClientAuthoritative = false;

    /** Also replicate and interpolate the actor's 3D scale. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync")
    bool bSyncScale = false;

    /** Stops moving the owner on this machine (interpolation and corrections). Useful while local physics or a cinematic takes over. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync")
    bool bDisableSmoothing = false;


    // ===== Network =====

    /** Position snapshots sent per second. Capped by the actor's NetUpdateFrequency and the server tick rate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", meta = (ClampMin = "1", ClampMax = "120", UIMin = "1", UIMax = "60"))
    float PositionUpdateRate = 20.0f;

    /** Rotation snapshots sent per second. Capped by the actor's NetUpdateFrequency and the server tick rate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", meta = (ClampMin = "1", ClampMax = "120", UIMin = "1", UIMax = "60"))
    float RotationUpdateRate = 30.0f;

    /** Also send the angular velocity: fast spins stay correct at low rates and rotation can be extrapolated. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network")
    bool bSyncAngularVelocity = false;

    /**
     * Server only: once the object has been at rest for DormancyDelay, stop replicating the actor entirely (net dormancy)
     * until it moves again. Big server CPU saving with many props.
     * Only enable on actors whose other replicated properties don't change while they are at rest.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network")
    bool bUseDormancyWhenAtRest = false;

    /** Seconds at rest before going dormant. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "0", EditCondition = "bUseDormancyWhenAtRest"))
    float DormancyDelay = 1.0f;

    /**
     * Sync the transform relative to a moving parent: the component the owner is attached to (ships, vehicles), the base
     * set with SetSyncBase (cargo), or the moving object a Character stands on (platforms, elevators). Remote copies then stay glued to it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay)
    bool bSyncRelativeToParent = true;

    /** Position precision on the wire. Lower precision means less bandwidth. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay)
    ESmoothSyncPositionPrecision PositionPrecision = ESmoothSyncPositionPrecision::Millimeter;

    /** Minimum movement (units) before a new position is sent. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "0"))
    float PositionSyncTolerance = 0.1f;

    /** Minimum quaternion difference before a new rotation is sent. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "0"))
    float RotationSyncTolerance = 0.0001f;

    /** Minimum scale difference before a new scale is sent. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "0", EditCondition = "bSyncScale"))
    float ScaleSyncTolerance = 0.01f;

    /** Server only: lower the update rates when every player is far away. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay)
    bool bEnableDistanceLOD = true;

    /** Beyond this distance (units) from the closest player, rates are capped to LODMediumRate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "0", EditCondition = "bEnableDistanceLOD"))
    float LODMediumDistance = 5000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "1", EditCondition = "bEnableDistanceLOD"))
    float LODMediumRate = 10.0f;

    /** Beyond this distance (units) from the closest player, rates are capped to LODFarRate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "0", EditCondition = "bEnableDistanceLOD"))
    float LODFarDistance = 15000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Network", AdvancedDisplay, meta = (ClampMin = "1", EditCondition = "bEnableDistanceLOD"))
    float LODFarRate = 5.0f;


    // ===== Interpolation (remote copies) =====

    /**
     * How far in the past (seconds) remote copies are displayed. Higher is smoother but lags more behind the server.
     * With adaptive delay, this is the minimum: the delay grows automatically on bad connections.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Interpolation", meta = (ClampMin = "0", UIMax = "0.5"))
    float InterpolationDelay = 0.1f;

    /** Grow the delay automatically from the measured packet spacing and jitter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Interpolation")
    bool bAdaptiveInterpolationDelay = true;

    /** Upper bound of the adaptive delay (seconds). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Interpolation", AdvancedDisplay, meta = (ClampMin = "0", UIMax = "1", EditCondition = "bAdaptiveInterpolationDelay"))
    float MaxInterpolationDelay = 0.35f;

    /** How long (seconds) to keep moving a remote copy along its last velocity when packets stop. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Interpolation", meta = (ClampMin = "0", UIMax = "2"))
    float MaxExtrapolationTime = 0.5f;

    /** A jump larger than this (units) beyond the expected travel snaps instead of sliding. Prefer NotifyTeleported() for real teleports. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Interpolation", meta = (ClampMin = "0"))
    float TeleportDistanceThreshold = 1000.0f;

    /** When a snapshot contradicts what was displayed (extrapolation miss), blend the difference out over this time instead of popping. 0 disables. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Interpolation", AdvancedDisplay, meta = (ClampMin = "0", UIMax = "0.5"))
    float ErrorSmoothingTime = 0.1f;


    // ===== Near the local player =====

    /**
     * Remote copies are normally shown slightly in the past. Near the local player, show this object in the player's present
     * instead (extrapolated), so pushing or catching it lines up with what the player sees. For physics props players interact with.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Local Player")
    bool bDisplayInPresentNearLocalPlayer = false;

    /** Fully in the present below this distance (units) from the local pawn. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Local Player", meta = (ClampMin = "0", EditCondition = "bDisplayInPresentNearLocalPlayer"))
    float PresentTimeNearDistance = 400.0f;

    /** Back to the normal delayed display beyond this distance (units). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Local Player", meta = (ClampMin = "0", EditCondition = "bDisplayInPresentNearLocalPlayer"))
    float PresentTimeFarDistance = 1200.0f;


    // ===== Owning client correction (server-authoritative pawns) =====
    // Skipped for client-authoritative pawns.

    /**
     * Characters: when the server corrects the owning player, the collision snaps right away but the visuals
     * (meshes, camera boom: the root's children) slide back over OwnerCorrectionSmoothingTime instead of popping.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction")
    bool bSmoothOwnerCorrections = true;

    /** Time constant (seconds) of the visual correction slide. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0", UIMax = "0.5", EditCondition = "bSmoothOwnerCorrections"))
    float OwnerCorrectionSmoothingTime = 0.15f;

    /** Corrections larger than this (units) snap: they are teleports, not jitter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0", EditCondition = "bSmoothOwnerCorrections"))
    float MaxOwnerCorrectionSmoothingDistance = 200.0f;

    /**
     * Characters on a listen server: the host sees remote players moved in bursts, as their moves arrive. Their visuals
     * (the root's children) follow a smoothed position instead. Collision is unaffected.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Listen Server")
    bool bSmoothRemotePlayersOnListenServer = true;

    /** Time constant (seconds) of the listen server smoothing. Higher is smoother but trails a bit more. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Listen Server", AdvancedDisplay, meta = (ClampMin = "0", UIMax = "0.3", EditCondition = "bSmoothRemotePlayersOnListenServer"))
    float ListenServerSmoothingTime = 0.08f;

    /** Error (units) between the owning client and the server tolerated before correcting. Increase for fast vehicles. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", meta = (ClampMin = "0"))
    float PredictionErrorTolerance = 50.0f;

    /** Scale applied to vertical errors, so suspension or jumps are corrected softly. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0", ClampMax = "1"))
    float ZDampingFactor = 0.15f;

    /** Base stiffness of the spring pulling a physics body toward the server position. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float BaseSpringStiffness = 5.0f;

    /** Extra stiffness per unit of error beyond ErrorThresholdForScaling. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float SpringStiffnessScale = 0.2f;

    /** Error (units) above which the spring gets stiffer. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float ErrorThresholdForScaling = 100.0f;

    /** Maximum acceleration used to nudge a physics body toward the server position. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float MaxLinearNudgeAccel = 5000.0f;

    /** Angle error (degrees) above which the rotation snaps to the server rotation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0", ClampMax = "180"))
    float HardSnapAngleDegrees = 45.0f;

    /** Angle error (degrees) above which the rotation is softly corrected. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0", ClampMax = "180"))
    float SoftSnapAngleDegrees = 5.0f;

    /** Stiffness of the angular spring used on physics bodies. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float AngularSpringStiffness = 8.0f;

    /** Maximum angular acceleration (rad/s²) used on physics bodies. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float MaxAngularNudgeAccel = 15.0f;

    /** Position correction speed for non-physics owners. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float PositionInterpSpeed = 10.0f;

    /** Rotation correction speed for non-physics owners. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Owner Correction", AdvancedDisplay, meta = (ClampMin = "0"))
    float RotationInterpSpeed = 5.0f;


    // ===== Client authority =====

    /**
     * Server rejects client states moving faster than this (units/s) and corrects the client.
     * Characters are checked on horizontal speed (falling is allowed).
     * 0 disables the check: any owning client can then teleport its pawn (a warning is logged).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Client Authority", meta = (ClampMin = "0", EditCondition = "bIsClientAuthoritative"))
    float MaxClientMoveSpeed = 0.0f;


    // ===== Debug =====

    /** Draws buffered snapshots, the delay, bandwidth and state of this component (also: console "smoothsync.Debug 1"). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Debug")
    bool bShowDebugPath = false;


    // ===== Events =====

    /** Snapshots stopped arriving for longer than MaxExtrapolationTime while the object was moving. */
    UPROPERTY(BlueprintAssignable, Category = "Smooth Sync|Events")
    FSmoothSyncLagDelegate OnNetworkLagDetected;

    /** Snapshots arrive again after OnNetworkLagDetected. */
    UPROPERTY(BlueprintAssignable, Category = "Smooth Sync|Events")
    FSmoothSyncLagDelegate OnNetworkLagRecovered;

    /** The owner was snapped instead of smoothed (teleport, or a correction too large to smooth). */
    UPROPERTY(BlueprintAssignable, Category = "Smooth Sync|Events")
    FSmoothSyncSnapDelegate OnHardSnapTriggered;


    // ===== API =====

    /**
     * Call after teleporting the owner (respawn, portal...) so remote machines snap instead of sliding.
     * Server only, or owning client when client-authoritative.
     */
    UFUNCTION(BlueprintCallable, Category = "Smooth Sync")
    void NotifyTeleported();

    /**
     * Server only: syncs the owner relative to InBase without attaching it (a crate in a truck bed, a barrel on a ship
     * deck), so remote copies stay glued to the base while it moves. Typically set when the object enters the cargo
     * area and cleared (null) when it leaves. InBase must belong to a replicated actor. Needs Sync Relative To Parent.
     */
    UFUNCTION(BlueprintCallable, Category = "Smooth Sync")
    void SetSyncBase(UPrimitiveComponent* InBase);

    /** The base set with SetSyncBase, if any. */
    UFUNCTION(BlueprintPure, Category = "Smooth Sync")
    UPrimitiveComponent* GetSyncBase() const { return SyncBase.Get(); }

    /** Clears the received snapshots on this machine. The next snapshot is applied directly. */
    UFUNCTION(BlueprintCallable, Category = "Smooth Sync")
    void ResetInterpolation();

    /** Applies a preset's values (Custom does nothing). */
    UFUNCTION(BlueprintCallable, Category = "Smooth Sync")
    void ApplyPreset(ESmoothSyncPreset InPreset);

    /** True while extrapolation is capped because snapshots stopped arriving. */
    UFUNCTION(BlueprintPure, Category = "Smooth Sync")
    bool IsLagging() const { return bIsLagging; }

    /** Current interpolation delay (seconds), including the adaptive part. */
    UFUNCTION(BlueprintPure, Category = "Smooth Sync")
    float GetCurrentInterpolationDelay() const { return CurrentInterpolationDelay; }

    /** Owning client only: distance (units) between the local pawn and the server's position. */
    UFUNCTION(BlueprintPure, Category = "Smooth Sync")
    float GetPredictionError() const { return PredictionError; }

    /** Server only: the actor is currently net dormant because it rests. */
    UFUNCTION(BlueprintPure, Category = "Smooth Sync")
    bool IsDormantAtRest() const { return bIsDormant; }


    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif

protected:

    virtual void BeginPlay() override;

    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

    /** Result of evaluating the position buffer at a time. Pure data: events are fired by the caller. */
    struct FPositionSample
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        FVector Scale = FVector::OneVector;
        bool bSnapped = false;
        double SnapTimestamp = 0.0;
    };


    // --- Tick (called by USmoothSyncSubsystem) ---

    void SmoothSyncTick(const FSmoothSyncFrameContext& InContext);

    /** Samples the owner and sends snapshots: replication on the server, RPC on a client-authoritative owner. */
    void TickSending(const FSmoothSyncFrameContext& InContext, bool bInIsServer);

    /** Displays the owner from the buffered snapshots: remote copies on clients, client-authoritative pawns on the server. */
    void TickInterpolation(const FSmoothSyncFrameContext& InContext);

    /** Softly corrects a server-authoritative owning client toward the server. */
    void TickAutonomousProxy(const FSmoothSyncFrameContext& InContext);


    // --- Characters (CMC owners) ---

    void TickCharacter(const FSmoothSyncFrameContext& InContext);

    /** On remote copies: CMC smoothing off while Smooth Sync drives them, restored otherwise. */
    void SetCharacterMovementSmoothing(bool bInUseCharacterMovementSmoothing);

    /** Server: lets the CMC trust the owner's position (client-authoritative), unless trust was revoked. */
    void ApplyCharacterTrust();

    /** Server: speed check for client-authoritative characters; revokes trust for a second on violation. */
    void CheckCharacterClientMoves(const FSmoothSyncFrameContext& InContext);

    /** Owning client: turns server corrections into a short visual slide (see bSmoothOwnerCorrections). */
    void TickOwnerCorrectionSmoothing(const FSmoothSyncFrameContext& InContext);

    /** Listen server: smooths the visuals of characters driven by remote players (see bSmoothRemotePlayersOnListenServer). */
    void TickListenServerSmoothing(const FSmoothSyncFrameContext& InContext);

    bool IsRemotePlayerOnListenServer(const FSmoothSyncFrameContext& InContext) const;

    /** Moves the root's children by the visual offset (in root space); zero removes it. */
    void ApplyVisualOffset(const FVector& InWorldOffset);

    /** Removes any visual offset (on role or mode changes). */
    void ClearVisualOffset();

    bool IsOwnerCharacter() const;

    /**
     * Server, character driven by a remote player: the server time at which its last processed move was made.
     * Moves arrive in bursts; stamping snapshots with the sampling time instead makes remote copies stutter.
     */
    bool GetRemoteMoveTime(const FSmoothSyncFrameContext& InContext, double& OutMoveTime);

    bool ShouldSkipOwner() const { return bIsClientAuthoritative || IsOwnerCharacter(); }


    // --- Evaluation ---

    FPositionSample SamplePosition(double InTargetTime, double InMaxExtrapolation) const;

    FQuat SampleRotation(double InTargetTime, double InMaxExtrapolation) const;

    void UpdateInterpolationDelay(float InDeltaTime);

    void UpdatePresentTimeWeight(const FSmoothSyncFrameContext& InContext);

    /** Absorbs the visual jump caused by a new snapshot into the decaying error offsets. */
    void AbsorbSnapshotError(const FVector& InPositionBefore, const FQuat& InRotationBefore);


    // --- Helpers ---

    /** The moving parent snapshots are relative to: the attach parent, the SetSyncBase base, or a Character's moving base. Null for world space. */
    const USceneComponent* GetSyncParent(FName& OutSocket) const;

    /** Sender: the movement base the snapshots are relative to, null when world space or relative to the attach parent. */
    UPrimitiveComponent* GetSyncMovementBase() const;

    /** The owner's transform in the space snapshots are expressed in (world, or relative to its sync parent). */
    FTransform GetOwnerSyncSpaceTransform(bool& bOutRelative) const;

    /**
     * A snapshot in another space arrived (stepped on / off a moving parent, or onto another one): restarts the buffers
     * with it and hides the visual jump in the decaying error offsets.
     */
    void RestartInNewSpace(const FSmoothSyncState_Pos& InState);

    /** Starts a new teleport id: receivers snap to the next snapshots instead of interpolating. */
    void MarkTeleported();

    /** Server: client-authoritative owner teleported by the server. Forces the owner there and ignores its older states. */
    void ForceClientToCurrentTransform();

    /** Server: replicates the current transform right away (when the owner's own states are not driving it). */
    void RelayCurrentTransform();

    /** Converts a transform in sync space to world space. */
    FTransform SyncSpaceToWorld(const FTransform& InTransform, bool bInRelative) const;

    void ApplyTransformToOwner(const FTransform& InSyncSpaceTransform, bool bInRelative, const FVector& InVelocity, const FVector& InAngularVelocity, bool bInTeleport);

    float GetLODRate(float InBaseRate, const FSmoothSyncFrameContext& InContext) const;

    double GetEstimatedServerTime() const;

    bool IsDrivenByRemoteClient(const FSmoothSyncFrameContext& InContext) const;

    bool CanUseDormancy() const;

    void SetDormant(bool bInDormant);

    void UpdateOwnerReplicationCondition();

    void ValidateOwnerSetup();

    void SetLagging(bool bInLagging);

    void NotifySnap(double InStateTimestamp);

    bool IsDebugDrawEnabled(const FSmoothSyncFrameContext* InContext = nullptr) const;

    void RecordDebugBandwidth(int32 InBits);

    void DrawDebugInterpolation(const FSmoothSyncFrameContext& InContext, double InTargetTime, const FVector& InWorldLocation, bool bInRelative) const;

    void DrawDebugAutonomous(const FSmoothSyncFrameContext& InContext, const FVector& InServerLocation, const FVector& InLocalLocation) const;

    void DrawDebugServer(const FSmoothSyncFrameContext& InContext) const;


    // --- Network ---

    UFUNCTION()
    void OnRep_SyncPos();

    UFUNCTION()
    void OnRep_SyncRot();

    /** Pushes a received position snapshot into the buffer (client replication, or client-authoritative RPC on the server). */
    bool ReceivePositionState(const FSmoothSyncState_Pos& InState, double InServerNow);

    bool ReceiveRotationState(const FSmoothSyncState_Rot& InState, double InServerNow);

    UFUNCTION(Server, Unreliable)
    void Server_ReceiveClientState(const FSmoothSyncState_Pos& InPos, const FSmoothSyncState_Rot& InRot);

    /** Sent to a client-authoritative owner whose state was rejected by the server. */
    UFUNCTION(Client, Reliable)
    void Client_ForceTransform(FVector_NetQuantize100 InPosition, FQuat InRotation);

    UPROPERTY(ReplicatedUsing = OnRep_SyncPos)
    FSmoothSyncState_Pos SyncPos;

    UPROPERTY(ReplicatedUsing = OnRep_SyncRot)
    FSmoothSyncState_Rot SyncRot;


    // --- Receiving side ---

    TSmoothSyncStateBuffer<FSmoothSyncState_Pos, MaxBufferCapacity> PosBuffer;
    TSmoothSyncStateBuffer<FSmoothSyncState_Rot, MaxBufferCapacity> RotBuffer;

    FSmoothSyncArrivalStats PosArrival;
    FSmoothSyncArrivalStats RotArrival;

    float CurrentInterpolationDelay = 0.1f;
    float PresentTimeWeight = 0.0f;
    float PredictionError = 0.0f;
    double LastSnapTimestamp = -1.0;

    /** Target time used for the last displayed frame (to measure snapshot-induced jumps). Negative when unknown. */
    double LastTargetTime = -1.0;
    double LastMaxExtrapolation = 0.0;

    /** Decaying offsets that hide extrapolation misses. */
    FVector PositionErrorOffset = FVector::ZeroVector;
    FQuat RotationErrorOffset = FQuat::Identity;

    bool bIsLagging = false;

    /** Remote copy reached its final resting snapshot: nothing to do until the next snapshot. */
    bool bIsSettled = false;

    /** A teleport was received: the next applied transform is a snap. */
    bool bPendingTeleport = false;


    // --- Sending side ---

    FSmoothSyncState_Pos LastSentPos;
    FSmoothSyncState_Rot LastSentRot;

    FVector LastSamplePosition = FVector::ZeroVector;
    FQuat LastSampleRotation = FQuat::Identity;
    double LastSamplePosTime = -1.0;
    double LastSampleRotTime = -1.0;

    float PosTimeSinceLastSync = 0.0f;
    float RotTimeSinceLastSync = 0.0f;

    uint8 LocalTeleportId = 0;
    bool bForceSendPos = true;
    bool bForceSendRot = true;

    /** Server: last time a snapshot was sent; drives dormancy. */
    double LastChangeTime = 0.0;
    bool bIsDormant = false;

    /** Server: after a server teleport, states from the owner are ignored until it acknowledges (sends this teleport id). */
    bool bAwaitingClientTeleport = false;
    uint8 ExpectedClientTeleportId = 0;

    /** Sender: the sync parent of the last sample (a different parent makes receivers snap). */
    TWeakObjectPtr<const USceneComponent> LastSyncParent;

    /** Server: the base set with SetSyncBase. */
    TWeakObjectPtr<UPrimitiveComponent> SyncBase;

    /** Receiver: capture time of the first snapshot in the current space. Older rotations belong to the previous one. */
    double SpaceStartTimestamp = -1.0;

    /** Server: last teleport id received from a client-authoritative owner, and when its last state arrived. */
    uint8 LastClientTeleportId = 0;
    double LastClientStateTime = -1.0;
    FVector LastClientPosition = FVector::ZeroVector;

    /** Server: whether SyncPos/SyncRot currently skip the owning connection. */
    bool bSkipOwnerApplied = false;


    // --- Cached owner info ---

    bool bOwnerImplementsSyncable = false;

    /** Set when the owner is a Character. */
    TWeakObjectPtr<UCharacterMovementComponent> CharacterMovement;
    uint8 SavedNetworkSmoothingMode = 0;
    bool bCharacterSmoothingOverridden = false;
    bool bCharacterTrustApplied = false;
    bool bCharacterTrustRevoked = false;
    double CharacterTrustRevokedUntil = 0.0;
    FVector LastCheckedCharacterLocation = FVector::ZeroVector;
    double LastCharacterCheckTime = -1.0;

    /** Server: client move clock to server clock offset, and the last move times seen / sampled. */
    double ClientMoveTimeOffset = 0.0;
    double LastClientMoveTimeStamp = -1.0;
    double LastSampledMoveTime = -1.0;
    bool bHasClientMoveTimeOffset = false;

    /** Owning client: visual offset hiding the last server corrections, and what is currently applied to the visuals. */
    FVector VisualOffset = FVector::ZeroVector;
    FVector AppliedVisualLocalOffset = FVector::ZeroVector;
    FVector LastOwnerLocation = FVector::ZeroVector;
    bool bHasLastOwnerLocation = false;

    /** Listen server: smoothed location shown for a remote player last frame. */
    FVector LastDisplayedLocation = FVector::ZeroVector;
    bool bHasLastDisplayedLocation = false;

    /** World velocity of the last transform applied to the owner (fed to the CMC for animation). */
    FVector LastAppliedVelocity = FVector::ZeroVector;

    int32 SubsystemIndex = INDEX_NONE;


    // --- Debug ---

    float DebugBytesPerSecond = 0.0f;
    int32 DebugBitsThisWindow = 0;
    double DebugWindowStart = 0.0;
    int32 DebugSnapCount = 0;
};
