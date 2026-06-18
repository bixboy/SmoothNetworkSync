#include "NetworkInterpolatorComponent.h"
#include "SmoothSyncSubsystem.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UNetworkInterpolatorComponent::UNetworkInterpolatorComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bWantsInitializeComponent = false;
    SetIsReplicatedByDefault(true);
}

void UNetworkInterpolatorComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UNetworkInterpolatorComponent, SyncPos);
    DOREPLIFETIME(UNetworkInterpolatorComponent, SyncRot);
}

void UNetworkInterpolatorComponent::BeginPlay()
{
    Super::BeginPlay();

    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
        return;

    if (UWorld* World = GetWorld())
    {
        if (USmoothSyncSubsystem* SyncSubsystem = World->GetSubsystem<USmoothSyncSubsystem>())
            SyncSubsystem->RegisterComponent(this);
    }
}

void UNetworkInterpolatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        if (USmoothSyncSubsystem* SyncSubsystem = World->GetSubsystem<USmoothSyncSubsystem>())
            SyncSubsystem->UnregisterComponent(this);
    }

    Super::EndPlay(EndPlayReason);
}

void UNetworkInterpolatorComponent::AddPositionState(const FSmoothSyncState_Pos& InNewState)
{
    if (PosElementCount > 0)
    {
        const FSmoothSyncState_Pos& CurrentNewest = GetPosState(0);
        if (InNewState.ServerTimestamp <= CurrentNewest.ServerTimestamp)
            return;
    }

    PosHeadIndex = (PosHeadIndex + 1) % MaxBufferCapacity;
    PosBuffer[PosHeadIndex] = InNewState;
    if (PosElementCount < MaxBufferCapacity)
        PosElementCount++;
}

void UNetworkInterpolatorComponent::AddRotationState(const FSmoothSyncState_Rot& InNewState)
{
    if (RotElementCount > 0)
    {
        const FSmoothSyncState_Rot& CurrentNewest = GetRotState(0);

        if (InNewState.ServerTimestamp <= CurrentNewest.ServerTimestamp)
            return;
    }

    RotHeadIndex = (RotHeadIndex + 1) % MaxBufferCapacity;
    RotBuffer[RotHeadIndex] = InNewState;

    if (RotElementCount < MaxBufferCapacity)
        RotElementCount++;
}

const FSmoothSyncState_Pos& UNetworkInterpolatorComponent::GetPosState(int32 InChronologicalIndex) const
{
    checkSlow(InChronologicalIndex >= 0 && InChronologicalIndex < PosElementCount);
    int32 ActualIndex = PosHeadIndex - InChronologicalIndex;

    if (ActualIndex < 0)
        ActualIndex += MaxBufferCapacity;

    return PosBuffer[ActualIndex];
}

const FSmoothSyncState_Rot& UNetworkInterpolatorComponent::GetRotState(int32 InChronologicalIndex) const
{
    checkSlow(InChronologicalIndex >= 0 && InChronologicalIndex < RotElementCount);
    int32 ActualIndex = RotHeadIndex - InChronologicalIndex;

    if (ActualIndex < 0)
        ActualIndex += MaxBufferCapacity;

    return RotBuffer[ActualIndex];
}

void UNetworkInterpolatorComponent::ServerUpdateAndSendState()
{
}

void UNetworkInterpolatorComponent::ClientSendStateToServer()
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
        return;

    Server_ReceiveClientState(Owner->GetActorLocation(), Owner->GetActorQuat(), Owner->GetVelocity());
}

void UNetworkInterpolatorComponent::Server_ReceiveClientState_Implementation(FVector InPos, FQuat InRot, FVector InVel)
{
    if (AActor* Owner = GetOwner())
    {
        Owner->SetActorLocationAndRotation(InPos, InRot);
    }
}

void UNetworkInterpolatorComponent::OnRep_SyncPos()
{
    // --- Dynamic Velocity Calculation ---
    if (PosElementCount > 0)
    {
        const FSmoothSyncState_Pos& LastState = GetPosState(0);
        float DeltaTime = SyncPos.ServerTimestamp - LastState.ServerTimestamp;
        
        if (DeltaTime > 0.0001f)
        {
            SyncPos.Velocity = (SyncPos.Position - LastState.Position) / DeltaTime;
        }
        else
        {
            SyncPos.Velocity = LastState.Velocity;
        }
    }
    else
    {
        SyncPos.Velocity = FVector::ZeroVector;
    }

    AddPositionState(SyncPos);
}

void UNetworkInterpolatorComponent::OnRep_SyncRot()
{
    AddRotationState(SyncRot);
}
