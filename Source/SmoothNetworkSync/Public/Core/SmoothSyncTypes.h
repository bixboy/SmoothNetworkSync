#pragma once
#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "SmoothSyncTypes.generated.h"



USTRUCT(BlueprintType)
struct FSmoothSyncState_Pos
{
    GENERATED_BODY()

public:

    /** The actual world position */
    UPROPERTY(BlueprintReadWrite, Category = "Smooth Sync", meta = (ToolTip = "The authoritative world position received from the server."))
    FVector Position = FVector::ZeroVector;

    /** The linear velocity */
    UPROPERTY(BlueprintReadWrite, Category = "Smooth Sync", meta = (ToolTip = "The current velocity at the time of the update. Used for Hermite Spline interpolation and Dead Reckoning Extrapolation."))
    FVector Velocity = FVector::ZeroVector;

    /** The 3D scale (optional) */
    UPROPERTY(BlueprintReadWrite, Category = "Smooth Sync", meta = (ToolTip = "The world scale. Synchronized only if bSyncScale is true."))
    FVector Scale = FVector::OneVector;

    /** Server timestamp */
    UPROPERTY(BlueprintReadWrite, Category = "Smooth Sync", meta = (ToolTip = "The exact server time at which this state was captured."))
    float ServerTimestamp = 0.0f;


    bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
    {
        bOutSuccess = true;
        bool bLocalSuccess = true;

        FVector_NetQuantize100 NetPos(Position);
        NetPos.NetSerialize(Ar, Map, bLocalSuccess);
        bOutSuccess &= bLocalSuccess;

        if (Ar.IsLoading())
            Position = NetPos;



        FVector_NetQuantize100 NetScale(Scale);
        NetScale.NetSerialize(Ar, Map, bLocalSuccess);
        bOutSuccess &= bLocalSuccess;

        if (Ar.IsLoading())
            Scale = NetScale;

        Ar << ServerTimestamp;
        return bOutSuccess;
    }
};

template<>
struct TStructOpsTypeTraits<FSmoothSyncState_Pos> : public TStructOpsTypeTraitsBase2<FSmoothSyncState_Pos>
{
    enum { WithNetSerializer = true, };
};


USTRUCT(BlueprintType)
struct FSmoothSyncState_Rot
{
    GENERATED_BODY()

public:

    /** The actual world rotation */
    UPROPERTY(BlueprintReadWrite, Category = "Smooth Sync", meta = (ToolTip = "The authoritative world rotation received from the server."))
    FQuat Rotation = FQuat::Identity;

    /** The server timestamp at which this state was recorded */
    UPROPERTY(BlueprintReadWrite, Category = "Smooth Sync", meta = (ToolTip = "The exact server time at which this state was captured."))
    float ServerTimestamp = 0.0f;


    bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
    {
        bOutSuccess = true;
        if (Ar.IsSaving())
        {
            Rotation.Normalize();
            if (Rotation.W < 0.0f)
            {
                Rotation.X = -Rotation.X;
                Rotation.Y = -Rotation.Y;
                Rotation.Z = -Rotation.Z;
                Rotation.W = -Rotation.W;
            }

            uint16 X16 = (uint16)((Rotation.X + 1.0f) * 32767.5f);
            uint16 Y16 = (uint16)((Rotation.Y + 1.0f) * 32767.5f);
            uint16 Z16 = (uint16)((Rotation.Z + 1.0f) * 32767.5f);

            Ar << X16; Ar << Y16; Ar << Z16;
        }
        else
        {
            uint16 X16 = 0, Y16 = 0, Z16 = 0;
            Ar << X16; Ar << Y16; Ar << Z16;

            Rotation.X = (X16 / 32767.5f) - 1.0f;
            Rotation.Y = (Y16 / 32767.5f) - 1.0f;
            Rotation.Z = (Z16 / 32767.5f) - 1.0f;

            float Wsq = 1.0f - (Rotation.X * Rotation.X + Rotation.Y * Rotation.Y + Rotation.Z * Rotation.Z);
            Rotation.W = Wsq > 0.0f ? FMath::Sqrt(Wsq) : 0.0f;
        }
        
        Ar << ServerTimestamp;
        return bOutSuccess;
    }
};

template<>
struct TStructOpsTypeTraits<FSmoothSyncState_Rot> : public TStructOpsTypeTraitsBase2<FSmoothSyncState_Rot>
{
    enum
    {
        WithNetSerializer = true,
    };
};
