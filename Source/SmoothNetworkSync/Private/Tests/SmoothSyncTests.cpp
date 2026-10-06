// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "Core/SmoothSyncTypes.h"
#include "Core/SmoothSyncBuffer.h"
#include "Serialization/BitReader.h"
#include "Serialization/BitWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    constexpr EAutomationTestFlags SmoothSyncTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

    template<typename StateType>
    StateType RoundTrip(StateType& InState)
    {
        FBitWriter Writer(0, true);
        bool bSuccess = false;
        InState.NetSerialize(Writer, nullptr, bSuccess);

        FBitReader Reader(Writer.GetData(), Writer.GetNumBits());
        StateType Out;
        Out.NetSerialize(Reader, nullptr, bSuccess);
        return Out;
    }
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmoothSyncTimestampTest, "SmoothSync.Timestamp.CompressRoundTrip", SmoothSyncTestFlags)
bool FSmoothSyncTimestampTest::RunTest(const FString& Parameters)
{
    const double Samples[] = { 0.0, 1.234, 65.535, 65.536, 3600.5, 86400.017 };
    for (const double Stamp : Samples)
    {
        // The receiver's clock is a bit ahead (normal) or behind (clock estimate error).
        for (const double ReferenceOffset : { 0.0, 0.25, -0.1, 20.0 })
        {
            const double Rebuilt = SmoothSync::DecompressTimestamp(SmoothSync::CompressTimestamp(Stamp), Stamp + ReferenceOffset);
            TestNearlyEqual(FString::Printf(TEXT("Stamp %.3f, offset %.2f"), Stamp, ReferenceOffset), Rebuilt, Stamp, 0.0011);
        }
    }
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmoothSyncPositionSerializeTest, "SmoothSync.Serialize.Position", SmoothSyncTestFlags)
bool FSmoothSyncPositionSerializeTest::RunTest(const FString& Parameters)
{
    FSmoothSyncState_Pos State;
    State.Position = FVector(12345.678, -987.654, 42.1);
    State.Velocity = FVector(600.0, 0.0, -980.0);
    State.Scale = FVector(2.0, 2.0, 0.5);
    State.bHasScale = true;
    State.TeleportId = 5;
    State.bRelativeToParent = true;
    State.ServerTimestamp = 12.345;
    State.Precision = ESmoothSyncPositionPrecision::Millimeter;

    const FSmoothSyncState_Pos Out = RoundTrip(State);
    TestTrue(TEXT("Position within 0.1"), Out.Position.Equals(State.Position, 0.1));
    TestTrue(TEXT("Velocity within 0.1"), Out.Velocity.Equals(State.Velocity, 0.1));
    TestTrue(TEXT("Scale within 0.01"), Out.Scale.Equals(State.Scale, 0.01));
    TestEqual(TEXT("TeleportId"), Out.TeleportId, State.TeleportId);
    TestTrue(TEXT("bRelativeToParent"), Out.bRelativeToParent);
    TestTrue(TEXT("bDecodedFromWire"), Out.bDecodedFromWire);
    TestEqual(TEXT("WireTimestamp"), Out.WireTimestamp, SmoothSync::CompressTimestamp(State.ServerTimestamp));

    // Resting object without scale: velocity and scale are not sent and come back as defaults.
    FSmoothSyncState_Pos Resting;
    Resting.Position = FVector(1.0, 2.0, 3.0);
    const FSmoothSyncState_Pos RestingOut = RoundTrip(Resting);
    TestTrue(TEXT("Resting velocity is zero"), RestingOut.Velocity.IsZero());
    TestTrue(TEXT("Unsent scale is one"), RestingOut.Scale.Equals(FVector::OneVector));
    TestFalse(TEXT("bHasScale"), RestingOut.bHasScale);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmoothSyncRotationSerializeTest, "SmoothSync.Serialize.Rotation", SmoothSyncTestFlags)
bool FSmoothSyncRotationSerializeTest::RunTest(const FString& Parameters)
{
    // Includes ~180 degree rotations, where dropping W (the old format) lost precision.
    const FRotator Samples[] = {
        FRotator::ZeroRotator, FRotator(10.f, 20.f, 30.f), FRotator(0.f, 180.f, 0.f), FRotator(89.f, -179.5f, 45.f), FRotator(-45.f, 179.9f, 180.f)
    };

    for (const FRotator& Sample : Samples)
    {
        FSmoothSyncState_Rot State;
        State.Rotation = Sample.Quaternion();
        State.TeleportId = 3;

        const FSmoothSyncState_Rot Out = RoundTrip(State);
        const float ErrorDegrees = FMath::RadiansToDegrees(Out.Rotation.AngularDistance(State.Rotation));
        TestTrue(FString::Printf(TEXT("%s error %.4f deg"), *Sample.ToString(), ErrorDegrees), ErrorDegrees < 0.01f);
        TestEqual(TEXT("TeleportId"), Out.TeleportId, State.TeleportId);
    }

    // Angular velocity (sent in degrees/s at 0.1 precision).
    FSmoothSyncState_Rot Spinning;
    Spinning.AngularVelocity = FVector(0.0, 0.0, 12.5);
    const FSmoothSyncState_Rot SpinningOut = RoundTrip(Spinning);
    TestTrue(TEXT("Angular velocity within 0.01 rad/s"), SpinningOut.AngularVelocity.Equals(Spinning.AngularVelocity, 0.01));
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmoothSyncResolveTimestampTest, "SmoothSync.Timestamp.ResolveIrisAndLegacy", SmoothSyncTestFlags)
bool FSmoothSyncResolveTimestampTest::RunTest(const FString& Parameters)
{
    // Iris replicates the struct properties as is: ServerTimestamp is used directly.
    FSmoothSyncState_Pos IrisState;
    IrisState.ServerTimestamp = 4321.123;
    TestEqual(TEXT("Iris path keeps ServerTimestamp"), IrisState.ResolveTimestamp(9999.0), 4321.123);

    // Legacy replication goes through NetSerialize: the compressed value is rebuilt against the receiver clock.
    FSmoothSyncState_Pos Sent;
    Sent.ServerTimestamp = 4321.123;
    const FSmoothSyncState_Pos Received = RoundTrip(Sent);
    TestNearlyEqual(TEXT("Legacy path rebuilds the timestamp"), Received.ResolveTimestamp(4321.3), 4321.123, 0.0011);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmoothSyncBufferTest, "SmoothSync.Buffer.RingAndSegment", SmoothSyncTestFlags)
bool FSmoothSyncBufferTest::RunTest(const FString& Parameters)
{
    TSmoothSyncStateBuffer<FSmoothSyncState_Rot, 4> Buffer;
    TestTrue(TEXT("Starts empty"), Buffer.IsEmpty());

    for (int32 i = 0; i < 6; ++i)
    {
        FSmoothSyncState_Rot State;
        State.ServerTimestamp = i;
        Buffer.Push(State);
    }

    TestEqual(TEXT("Capped at capacity"), Buffer.Num(), 4);
    TestEqual(TEXT("Newest"), Buffer.Newest().ServerTimestamp, 5.0);
    TestEqual(TEXT("Oldest"), Buffer.Oldest().ServerTimestamp, 2.0);

    const FSmoothSyncState_Rot* Older = nullptr;
    const FSmoothSyncState_Rot* Newer = nullptr;
    TestTrue(TEXT("Finds inner segment"), Buffer.FindSegment(3.5, Older, Newer));
    TestTrue(TEXT("Segment bounds"), Older && Newer && Older->ServerTimestamp == 3.0 && Newer->ServerTimestamp == 4.0);
    TestFalse(TEXT("Outside range"), Buffer.FindSegment(1.0, Older, Newer));

    Buffer.Reset();
    TestTrue(TEXT("Reset empties"), Buffer.IsEmpty());
    return true;
}

#endif
