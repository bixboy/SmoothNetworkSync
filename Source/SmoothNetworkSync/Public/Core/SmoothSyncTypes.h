// Copyright (c) Bixboy, 2026. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "SmoothSyncTypes.generated.h"

class UPrimitiveComponent;


/** Ready-made settings for common use cases. Selecting one overwrites the related settings of the component. */
UENUM(BlueprintType)
enum class ESmoothSyncPreset : uint8
{
    Custom          UMETA(ToolTip = "Keep the values you set manually."),
    Prop            UMETA(ToolTip = "Objects moved by gameplay code on the server (doors, platforms, pickups, AI)."),
    PhysicsObject   UMETA(DisplayName = "Physics Object", ToolTip = "Objects simulated by physics on the server (crates, debris, balls)."),
    Vehicle         UMETA(ToolTip = "Fast player-driven pawns. Usually combined with client authority."),
};

/** How precisely positions are sent over the network. Lower precision means less bandwidth. */
UENUM(BlueprintType)
enum class ESmoothSyncPositionPrecision : uint8
{
    Centimeter      UMETA(DisplayName = "1 cm", ToolTip = "1 unit precision. Cheapest, fine for most props."),
    Millimeter      UMETA(DisplayName = "1 mm", ToolTip = "0.1 unit precision. Good default."),
    TenthMillimeter UMETA(DisplayName = "0.1 mm", ToolTip = "0.01 unit precision. Highest bandwidth."),
};


namespace SmoothSync
{
    /** Timestamps travel as milliseconds modulo 2^16 (a ~65s window) and are rebuilt against the receiver's clock. */
    inline uint16 CompressTimestamp(double InSeconds)
    {
        return static_cast<uint16>(static_cast<int64>(FMath::RoundToDouble(InSeconds * 1000.0)) & 0xFFFF);
    }

    /** Rebuilds the full timestamp closest to InReferenceSeconds (the receiver's estimate of the server time). */
    inline double DecompressTimestamp(uint16 InWire, double InReferenceSeconds)
    {
        const int64 ReferenceMs = static_cast<int64>(FMath::RoundToDouble(InReferenceSeconds * 1000.0));
        int64 Ms = (ReferenceMs & ~static_cast<int64>(0xFFFF)) | InWire;

        if (Ms - ReferenceMs > 0x8000)
            Ms -= 0x10000;
        else if (ReferenceMs - Ms > 0x8000)
            Ms += 0x10000;

        return Ms / 1000.0;
    }

    /** Teleport ids wrap at 8 (3 bits on the wire). */
    constexpr uint8 TeleportIdMask = 0x7;
}


/** A position snapshot sent by the authority. */
USTRUCT(BlueprintType)
struct SMOOTHNETWORKSYNC_API FSmoothSyncState_Pos
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "The authoritative position (world space, or relative to the attach parent when bRelativeToParent)."))
    FVector Position = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "The velocity at capture time, in the same space as Position. Zero when the object came to rest."))
    FVector Velocity = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "The world scale. Only sent when bSyncScale is enabled."))
    FVector Scale = FVector::OneVector;

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "Server time (seconds) at which this state was captured."))
    double ServerTimestamp = 0.0;

    /** Incremented on every teleport. A change tells receivers to snap instead of interpolating. */
    UPROPERTY()
    uint8 TeleportId = 0;

    /** Position and velocity are relative to the owner's attach parent, or to MovementBase when set. */
    UPROPERTY()
    bool bRelativeToParent = false;

    /** The moving object a Character stands on (platform, elevator), when the state is relative to it. */
    UPROPERTY()
    TObjectPtr<UPrimitiveComponent> MovementBase = nullptr;

    // --- Wire settings, filled by the sender (not compared for replication) ---

    bool bHasScale = false;
    ESmoothSyncPositionPrecision Precision = ESmoothSyncPositionPrecision::Millimeter;

    /** Compressed timestamp. Only valid when bDecodedFromWire (legacy replication); Iris sends ServerTimestamp as is. */
    uint16 WireTimestamp = 0;
    bool bDecodedFromWire = false;

    /** The capture time on the server clock, rebuilt from the compressed wire value when needed. */
    double ResolveTimestamp(double InReceiverServerTime) const
    {
        return bDecodedFromWire ? SmoothSync::DecompressTimestamp(WireTimestamp, InReceiverServerTime) : ServerTimestamp;
    }

    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

template<>
struct TStructOpsTypeTraits<FSmoothSyncState_Pos> : public TStructOpsTypeTraitsBase2<FSmoothSyncState_Pos>
{
    enum { WithNetSerializer = true };
};


/** A rotation snapshot sent by the authority. */
USTRUCT(BlueprintType)
struct SMOOTHNETWORKSYNC_API FSmoothSyncState_Rot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "The authoritative rotation (world space, or relative to the attach parent when bRelativeToParent)."))
    FQuat Rotation = FQuat::Identity;

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "Angular velocity (radians/s, axis * speed) in the same space as Rotation. Only sent when bSyncAngularVelocity is enabled."))
    FVector AngularVelocity = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Smooth Sync", meta = (ToolTip = "Server time (seconds) at which this state was captured."))
    double ServerTimestamp = 0.0;

    /** Incremented on every teleport. A change tells receivers to snap instead of interpolating. */
    UPROPERTY()
    uint8 TeleportId = 0;

    /** Rotation is relative to the owner's attach parent. */
    UPROPERTY()
    bool bRelativeToParent = false;

    /** Compressed timestamp. Only valid when bDecodedFromWire (legacy replication); Iris sends ServerTimestamp as is. */
    uint16 WireTimestamp = 0;
    bool bDecodedFromWire = false;

    double ResolveTimestamp(double InReceiverServerTime) const
    {
        return bDecodedFromWire ? SmoothSync::DecompressTimestamp(WireTimestamp, InReceiverServerTime) : ServerTimestamp;
    }

    /** Sent with "smallest three" compression: 2 bits of index + 3 x 16 bits. */
    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

template<>
struct TStructOpsTypeTraits<FSmoothSyncState_Rot> : public TStructOpsTypeTraitsBase2<FSmoothSyncState_Rot>
{
    enum { WithNetSerializer = true };
};
