// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "Core/SmoothSyncTypes.h"
#include "Components/PrimitiveComponent.h"
#include "UObject/CoreNet.h"


namespace
{
    template<typename QuantizedVectorType>
    bool SerializeQuantizedVector(FArchive& Ar, UPackageMap* Map, FVector& InOutValue)
    {
        QuantizedVectorType Quantized(InOutValue);
        bool bSuccess = true;
        Quantized.NetSerialize(Ar, Map, bSuccess);

        if (Ar.IsLoading())
            InOutValue = Quantized;

        return bSuccess;
    }

    bool SerializePosition(FArchive& Ar, UPackageMap* Map, FVector& InOutValue, ESmoothSyncPositionPrecision InPrecision)
    {
        switch (InPrecision)
        {
        case ESmoothSyncPositionPrecision::Centimeter:
            return SerializeQuantizedVector<FVector_NetQuantize>(Ar, Map, InOutValue);
        case ESmoothSyncPositionPrecision::TenthMillimeter:
            return SerializeQuantizedVector<FVector_NetQuantize100>(Ar, Map, InOutValue);
        default:
            return SerializeQuantizedVector<FVector_NetQuantize10>(Ar, Map, InOutValue);
        }
    }

    // Largest quaternion component dropped, the other three lie in [-1/sqrt(2), 1/sqrt(2)].
    constexpr float QuatComponentRange = UE_INV_SQRT_2;

    uint16 QuantizeQuatComponent(float InValue)
    {
        const float Normalized = (FMath::Clamp(InValue, -QuatComponentRange, QuatComponentRange) + QuatComponentRange) / (2.0f * QuatComponentRange);
        return static_cast<uint16>(FMath::RoundToInt(Normalized * 65535.0f));
    }

    float DequantizeQuatComponent(uint16 InValue)
    {
        return (InValue / 65535.0f) * (2.0f * QuatComponentRange) - QuatComponentRange;
    }
}


bool FSmoothSyncState_Pos::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
    bOutSuccess = true;

    // Header: precision (2 bits) | has scale (1) | has velocity (1) | relative (1) | teleport id (3)
    uint8 Header = 0;
    if (Ar.IsSaving())
    {
        const bool bHasVelocity = !Velocity.IsNearlyZero(0.05);
        Header = (static_cast<uint8>(Precision) & 0x3)
            | (bHasScale ? 0x4 : 0)
            | (bHasVelocity ? 0x8 : 0)
            | (bRelativeToParent ? 0x10 : 0)
            | ((TeleportId & SmoothSync::TeleportIdMask) << 5);
        WireTimestamp = SmoothSync::CompressTimestamp(ServerTimestamp);
    }

    Ar << Header;
    Ar << WireTimestamp;

    if (Ar.IsLoading())
    {
        Precision = static_cast<ESmoothSyncPositionPrecision>(Header & 0x3);
        bHasScale = (Header & 0x4) != 0;
        bRelativeToParent = (Header & 0x10) != 0;
        TeleportId = (Header >> 5) & SmoothSync::TeleportIdMask;
        bDecodedFromWire = true;
    }

    // Relative states carry the movement base they are relative to (null when relative to the attach parent).
    if (bRelativeToParent && Map)
    {
        UObject* BaseObject = MovementBase;
        bOutSuccess = Map->SerializeObject(Ar, UPrimitiveComponent::StaticClass(), BaseObject);
        if (Ar.IsLoading())
            MovementBase = Cast<UPrimitiveComponent>(BaseObject);
    }
    else if (Ar.IsLoading())
    {
        MovementBase = nullptr;
    }

    bOutSuccess &= SerializePosition(Ar, Map, Position, Precision);

    if (Header & 0x8)
        bOutSuccess &= SerializeQuantizedVector<FVector_NetQuantize10>(Ar, Map, Velocity);
    else if (Ar.IsLoading())
        Velocity = FVector::ZeroVector;

    if (bHasScale)
        bOutSuccess &= SerializeQuantizedVector<FVector_NetQuantize100>(Ar, Map, Scale);
    else if (Ar.IsLoading())
        Scale = FVector::OneVector;

    return true;
}


bool FSmoothSyncState_Rot::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
    bOutSuccess = true;

    // Header: largest component index (2 bits) | teleport id (3) | relative (1) | has angular velocity (1)
    uint8 Header = 0;
    uint16 Packed[3] = { 0, 0, 0 };

    if (Ar.IsSaving())
    {
        const FQuat Normalized = Rotation.GetNormalized();
        const float Components[4] = { static_cast<float>(Normalized.X), static_cast<float>(Normalized.Y), static_cast<float>(Normalized.Z), static_cast<float>(Normalized.W) };

        int32 LargestIndex = 0;
        for (int32 i = 1; i < 4; ++i)
        {
            if (FMath::Abs(Components[i]) > FMath::Abs(Components[LargestIndex]))
                LargestIndex = i;
        }

        // q and -q are the same rotation: flip so the dropped component is positive.
        const float Sign = Components[LargestIndex] < 0.0f ? -1.0f : 1.0f;
        int32 Out = 0;
        for (int32 i = 0; i < 4; ++i)
        {
            if (i != LargestIndex)
                Packed[Out++] = QuantizeQuatComponent(Components[i] * Sign);
        }

        const bool bHasAngularVelocity = !AngularVelocity.IsNearlyZero(0.001);
        Header = static_cast<uint8>(LargestIndex)
            | ((TeleportId & SmoothSync::TeleportIdMask) << 2)
            | (bRelativeToParent ? 0x20 : 0)
            | (bHasAngularVelocity ? 0x40 : 0);
        WireTimestamp = SmoothSync::CompressTimestamp(ServerTimestamp);
    }

    Ar << Header;
    Ar << Packed[0];
    Ar << Packed[1];
    Ar << Packed[2];
    Ar << WireTimestamp;

    // Angular velocity travels in degrees/s at 0.1 precision.
    if (Header & 0x40)
    {
        FVector Degrees = FMath::RadiansToDegrees(AngularVelocity);
        bOutSuccess = SerializeQuantizedVector<FVector_NetQuantize10>(Ar, Map, Degrees);
        if (Ar.IsLoading())
            AngularVelocity = FMath::DegreesToRadians(Degrees);
    }
    else if (Ar.IsLoading())
    {
        AngularVelocity = FVector::ZeroVector;
    }

    if (Ar.IsLoading())
    {
        const int32 LargestIndex = Header & 0x3;
        TeleportId = (Header >> 2) & SmoothSync::TeleportIdMask;
        bRelativeToParent = (Header & 0x20) != 0;
        bDecodedFromWire = true;

        float Components[4];
        float SumSquares = 0.0f;
        int32 In = 0;
        for (int32 i = 0; i < 4; ++i)
        {
            if (i == LargestIndex)
                continue;

            Components[i] = DequantizeQuatComponent(Packed[In++]);
            SumSquares += Components[i] * Components[i];
        }
        Components[LargestIndex] = FMath::Sqrt(FMath::Max(0.0f, 1.0f - SumSquares));

        Rotation = FQuat(Components[0], Components[1], Components[2], Components[3]);
        Rotation.Normalize();
    }

    return true;
}
