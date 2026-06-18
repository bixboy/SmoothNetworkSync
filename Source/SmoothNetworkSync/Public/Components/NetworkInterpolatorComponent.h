#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SmoothSyncTypes.h"
#include "NetworkInterpolatorComponent.generated.h"


DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSmoothSyncLagDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSmoothSyncSnapDelegate);


UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SMOOTHNETWORKSYNC_API UNetworkInterpolatorComponent : public UActorComponent
{
    GENERATED_BODY()

    friend class USmoothSyncSubsystem;

public:

    UNetworkInterpolatorComponent();

    static constexpr int32 MaxBufferCapacity = 16;

    void ServerUpdateAndSendState();

    void ClientSendStateToServer();


    // --- Dynamic Jitter Buffer & Smoothing ---
    
    /** 
     * The target interpolation delay in seconds. 
     * Higher means smoother movement but more visual lag behind the server. 
     * Auto-regulated based on packet buffer health.
     */
    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Configuration", meta = (ToolTip = "The target interpolation delay in seconds. Higher means smoother movement but more visual lag behind the server."))
    float InterpolationDelay = 0.1f;

    float TargetInterpolationDelay = 0.1f;

    /** 
     * If the distance between the newest packet and the target position exceeds this, 
     * the system teleports the object instead of interpolating through walls. 
     */
    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Configuration", meta = (ToolTip = "If the distance between two network states exceeds this threshold (in Unreal Units), the object snaps instantly instead of smoothly sliding."))
    float TeleportDistanceThreshold = 1000.0f;

    /** 
     * Maximum allowed extrapolation time into the future when packets are dropped.
     * Prevents objects from flying away forever when the connection dies.
     */
    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Configuration", meta = (ToolTip = "Maximum allowed extrapolation time (in seconds) into the future when packets drop. Prevents objects flying infinitely."))
    float MaxExtrapolationTime = 0.5f;


    // --- Configuration & Debug ---

    /** The network update rate per second for Position (e.g. 20Hz). Server-side only. */
    UPROPERTY(EditDefaultsOnly, Category = "Smooth Sync|Configuration", meta = (ToolTip = "The network update rate per second for Position. Only used by the Server or Autonomous Proxy."))
    float PositionUpdateRate = 20.0f;

    /** The network update rate per second for Rotation (e.g. 60Hz). Server-side only. */
    UPROPERTY(EditDefaultsOnly, Category = "Smooth Sync|Configuration", meta = (ToolTip = "The network update rate per second for Rotation. Slerp requires a higher frequency for smooth visuals."))
    float RotationUpdateRate = 60.0f;

    /** Minimum distance the actor must move before sending a position update over the network. */
    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Configuration", meta = (ToolTip = "Minimum distance (in Unreal Units) the actor must move before sending a position update over the network."))
    float PositionSyncTolerance = 0.1f;

    /** Minimum quaternion difference the actor must rotate before sending a rotation update. */
    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Configuration", meta = (ToolTip = "Minimum quaternion difference the actor must rotate before sending a rotation update over the network. A value of 0.0001f is recommended for smooth continuous rotations."))
    float RotationSyncTolerance = 0.0001f;

    /** Allowed prediction error distance before the client soft-corrects its position (Autonomous Proxy). */
    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Configuration", meta = (ToolTip = "Allowed prediction error distance before the client soft-corrects its position to match the server. Increase this for fast vehicles to allow slight divergence during turns without stuttering."))
    float PredictionErrorTolerance = 50.0f;

    /** Disables all smoothing and interpolation. Useful when local physics takes over. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Control", meta = (ToolTip = "Disables all smoothing and interpolation. Useful when local physics temporarily takes over."))
    bool bDisableSmoothing = false;

    /** If true, the scale of the object will also be synchronized and interpolated. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Control", meta = (ToolTip = "If true, the actor's 3D scale will also be replicated and interpolated. Defaults to false to save bandwidth."))
    bool bSyncScale = false;

    /** Enables visual debug lines in the world showing server position vs interpolated position. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Debug", meta = (ToolTip = "Enables visual debug lines in the world showing server position vs interpolated position."))
    bool bShowDebugPath = false;

    /** If true, the client owning this actor will forcefully send its state to the server. Set to false for Server-Authoritative vehicles. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smooth Sync|Control")
    bool bIsClientAuthoritative = false;


    // --- Autonomous Proxy Prediction ---

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Damping factor applied to Z-axis prediction errors."))
    float ZDampingFactor = 0.15f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Base stiffness for the spring used in linear prediction soft correction."))
    float BaseSpringStiffness = 5.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "How much the spring stiffness scales up as the prediction error grows."))
    float SpringStiffnessScale = 0.2f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Distance error threshold before scaling up the spring stiffness."))
    float ErrorThresholdForScaling = 100.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Maximum acceleration that can be applied to nudge the actor to the correct position."))
    float MaxLinearNudgeAccel = 5000.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Angle difference in degrees above which the rotation will hard snap to the server rotation."))
    float HardSnapAngleDegrees = 45.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Angle difference in degrees above which soft angular correction is applied."))
    float SoftSnapAngleDegrees = 5.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Stiffness of the angular spring for soft rotation correction."))
    float AngularSpringStiffness = 8.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Maximum angular acceleration that can be applied for soft rotation correction."))
    float MaxAngularNudgeAccel = 15.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Speed at which position interpolates during soft correction if not simulating physics."))
    float PositionInterpSpeed = 10.0f;

    UPROPERTY(EditAnywhere, Category = "Smooth Sync|Autonomous Prediction", meta = (ToolTip = "Speed at which rotation interpolates during soft correction if not simulating physics."))
    float RotationInterpSpeed = 5.0f;


    // --- Events ---

    /** Fired when the interpolation exceeds MaxExtrapolationTime (packet loss or ping spike). */
    UPROPERTY(BlueprintAssignable, Category = "Smooth Sync|Events")
    FSmoothSyncLagDelegate OnNetworkLagDetected;

    /** Fired when the distance error exceeds TeleportDistanceThreshold and a hard snap occurs. */
    UPROPERTY(BlueprintAssignable, Category = "Smooth Sync|Events")
    FSmoothSyncSnapDelegate OnHardSnapTriggered;


    // --- Batch Synchronization (Subsystem Managed) ---
    
    float PosTimeSinceLastSync = 0.0f;
    float RotTimeSinceLastSync = 0.0f;
    
    float GetPositionUpdateRate() const { return PositionUpdateRate; }
    float GetRotationUpdateRate() const { return RotationUpdateRate; }


    // --- State Management ---

    UFUNCTION(BlueprintCallable, Category = "Smooth Sync")
    void AddPositionState(const FSmoothSyncState_Pos& InNewState);

    UFUNCTION(BlueprintCallable, Category = "Smooth Sync")
    void AddRotationState(const FSmoothSyncState_Rot& InNewState);

    FORCEINLINE int32 GetPosStateCount() const { return PosElementCount; }
    FORCEINLINE int32 GetRotStateCount() const { return RotElementCount; }

    const FSmoothSyncState_Pos& GetPosState(int32 InChronologicalIndex) const;
    const FSmoothSyncState_Rot& GetRotState(int32 InChronologicalIndex) const;

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:

    virtual void BeginPlay() override;

    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

    // --- Internal Data ---

    FSmoothSyncState_Pos PosBuffer[MaxBufferCapacity];
    int32 PosHeadIndex = -1;
    int32 PosElementCount = 0;

    FSmoothSyncState_Rot RotBuffer[MaxBufferCapacity];
    int32 RotHeadIndex = -1;
    int32 RotElementCount = 0;

    bool bIsLagging = false;


    // --- Network Logic ---

    UFUNCTION()
    void OnRep_SyncPos();

    UFUNCTION()
    void OnRep_SyncRot();

    UFUNCTION(Server, Unreliable)
    void Server_ReceiveClientState(FVector InPos, FQuat InRot, FVector InVel);

    UPROPERTY(ReplicatedUsing = OnRep_SyncPos)
    FSmoothSyncState_Pos SyncPos;

    UPROPERTY(ReplicatedUsing = OnRep_SyncRot)
    FSmoothSyncState_Rot SyncRot;
};
