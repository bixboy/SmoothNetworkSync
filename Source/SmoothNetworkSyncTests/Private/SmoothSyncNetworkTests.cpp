// Copyright (c) Bixboy, 2026. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "Components/SmoothSyncComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Player.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "SmoothPawn.h"
#include "SmoothPhysicsProp.h"
#include "SmoothProp.h"
#include "Engine/StaticMesh.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Multiplayer PIE regression tests on the example map: dedicated server + 1 client, with and without bad network.
 * Run from the Session Frontend or: UnrealEditor <project> -ExecCmds="Automation RunTests SmoothSync.Network; Quit"
 */
namespace SmoothSyncNetworkTest
{
    const TCHAR* MapPath = TEXT("/SmoothNetworkSync/Examples/Maps/MapTest_SmoothSync");

    struct FPIEWorlds
    {
        UWorld* Server = nullptr;
        UWorld* Client = nullptr;
        TArray<UWorld*, TInlineAllocator<2>> Clients;
    };

    FPIEWorlds FindPIEWorlds()
    {
        FPIEWorlds Worlds;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (Context.WorldType != EWorldType::PIE || !World)
                continue;

            if (World->GetNetMode() == NM_Client)
                Worlds.Clients.Add(World);
            else if (World->GetNetMode() == NM_DedicatedServer || World->GetNetMode() == NM_ListenServer)
                Worlds.Server = World;
        }
        Worlds.Client = Worlds.Clients.IsEmpty() ? nullptr : Worlds.Clients[0];
        return Worlds;
    }

    /** Measures how smooth a moving object looks: frame-to-frame acceleration and pops (jumps its speed can't explain). */
    struct FMotionSmoothness
    {
        void Add(const FVector& InLocation, float InDeltaTime, bool bInRecord, float InMaxSpeed)
        {
            if (bHasLocation && InDeltaTime > 0.0f)
            {
                const FVector Velocity = (InLocation - LastLocation) / InDeltaTime;
                if (bInRecord && bHasVelocity)
                {
                    Accelerations.Add((Velocity - LastVelocity).Size2D() / InDeltaTime);
                    Pops += FVector::Dist2D(InLocation, LastLocation) > InMaxSpeed * 1.5f * InDeltaTime + 10.0f ? 1 : 0;
                }
                LastVelocity = Velocity;
                bHasVelocity = true;
            }
            LastLocation = InLocation;
            bHasLocation = true;
        }

        float Mean() const
        {
            double Sum = 0.0;
            for (const float Value : Accelerations)
                Sum += Value;
            return Accelerations.Num() ? static_cast<float>(Sum / Accelerations.Num()) : 0.0f;
        }

        FString Describe() const
        {
            TArray<float> Sorted = Accelerations;
            Sorted.Sort();
            const float P95 = Sorted.Num() ? Sorted[FMath::Min(Sorted.Num() - 1, FMath::FloorToInt(Sorted.Num() * 0.95f))] : 0.0f;
            return FString::Printf(TEXT("mean accel %.0f, p95 accel %.0f u/s², pops %d"), Mean(), P95, Pops);
        }

        TArray<float> Accelerations;
        int32 Pops = 0;

    private:
        FVector LastLocation = FVector::ZeroVector;
        FVector LastVelocity = FVector::ZeroVector;
        bool bHasLocation = false;
        bool bHasVelocity = false;
    };

    /** The remote copy of another player's (or AI's) example pawn in a client world. */
    APawn* FindSimulatedPawn(UWorld* InClient)
    {
        for (TActorIterator<ASmoothPawn> It(InClient); It; ++It)
        {
            if (It->GetLocalRole() == ROLE_SimulatedProxy)
                return *It;
        }
        return nullptr;
    }

    /** Sharp direction changes every 0.6 s: the hardest case for remote smoothing. */
    FVector SquarePathDirection(float InTime)
    {
        static const FVector Directions[] = { FVector::ForwardVector, FVector::RightVector, FVector::BackwardVector, FVector::LeftVector };
        return Directions[FMath::FloorToInt(InTime / 0.6f) % 4];
    }

    /** Level-placed actors keep the same name in every PIE world. */
    template<typename ActorType>
    ActorType* FindNamed(UWorld* InWorld, FName InName)
    {
        for (TActorIterator<ActorType> It(InWorld); It; ++It)
        {
            if (It->GetFName() == InName)
                return *It;
        }
        return nullptr;
    }

    APawn* GetLocalPawn(UWorld* InClient)
    {
        const APlayerController* PC = InClient ? InClient->GetFirstPlayerController() : nullptr;
        return PC ? PC->GetPawn() : nullptr;
    }

    /** Spawned actors are not guaranteed to share names across worlds: find the player's pawn on the server by control. */
    APawn* GetServerPlayerPawn(UWorld* InServer)
    {
        for (TActorIterator<APawn> It(InServer); It; ++It)
        {
            if (It->IsPlayerControlled())
                return *It;
        }
        return nullptr;
    }

    float HorizontalSpeed(const APawn* InPawn)
    {
        return InPawn ? InPawn->GetVelocity().Size2D() : 0.0f;
    }
}


/**
 * Starts a multiplayer PIE session, optionally with network emulation.
 * Dedicated server + InNumPlayers clients, or a listen server whose host is one of the InNumPlayers players.
 */
class FSmoothSyncStartNetworkPIE : public IAutomationLatentCommand
{
public:
    FSmoothSyncStartNetworkPIE(bool bInLagged, int32 InNumPlayers = 1, bool bInListenServer = false)
        : bLagged(bInLagged), bListenServer(bInListenServer), NumPlayers(InNumPlayers) {}

    virtual bool Update() override
    {
        ULevelEditorPlaySettings* Settings = DuplicateObject(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
        Settings->SetPlayNetMode(bListenServer ? EPlayNetMode::PIE_ListenServer : EPlayNetMode::PIE_Client);
        Settings->SetPlayNumberOfClients(NumPlayers);
        Settings->SetRunUnderOneProcess(true);

        FLevelEditorPlayNetworkEmulationSettings& Emulation = Settings->NetworkEmulationSettings;
        Emulation.bIsNetworkEmulationEnabled = bLagged;
        Emulation.EmulationTarget = NetworkEmulationTarget::Any;
        Emulation.CurrentProfile = TEXT("Custom");
        for (FNetworkEmulationPacketSettings* Packets : { &Emulation.OutPackets, &Emulation.InPackets })
        {
            // ~150 ms round trip with jitter, 3% loss each way.
            Packets->MinLatency = bLagged ? 60 : 0;
            Packets->MaxLatency = bLagged ? 90 : 0;
            Packets->PacketLossPercentage = bLagged ? 3 : 0;
        }

        FRequestPlaySessionParams Params;
        Params.WorldType = EPlaySessionWorldType::PlayInEditor;
        Params.EditorPlaySettings = Settings;
        GEditor->RequestPlaySession(Params);
        return true;
    }

private:
    bool bLagged = false;
    bool bListenServer = false;
    int32 NumPlayers = 1;
};


/** Drives the client pawn through every scenario and checks what the client sees against the server. */
class FSmoothSyncScenarioCommand : public IAutomationLatentCommand
{
public:
    FSmoothSyncScenarioCommand(FAutomationTestBase* InTest, bool bInLagged, bool bInServerAuthority)
        : Test(InTest), bLagged(bInLagged), bServerAuthority(bInServerAuthority) {}

    virtual bool Update() override
    {
        const float DeltaTime = FApp::GetDeltaTime();
        PhaseTime += DeltaTime;
        TotalTime += DeltaTime;

        if (TotalTime > 120.0f)
        {
            Test->AddError(TEXT("Scenario timed out."));
            return true;
        }

        Worlds = SmoothSyncNetworkTest::FindPIEWorlds();
        Pawn = SmoothSyncNetworkTest::GetLocalPawn(Worlds.Client);

        switch (Phase)
        {
        case EPhase::WaitForPlayers:  return TickWaitForPlayers();
        case EPhase::Settle:          return TickSettle();
        case EPhase::Inertia:         return TickInertia();
        case EPhase::Push:            return TickPush();
        case EPhase::Rest:            return TickRest();
        case EPhase::Teleport:        return TickTeleport();
        case EPhase::PlayerTeleport:  return TickPlayerTeleport();
        case EPhase::RemoteCharacter: return TickRemoteCharacter();
        default:                      return true;
        }
    }

private:

    enum class EPhase { WaitForPlayers, Settle, Inertia, Push, Rest, Teleport, PlayerTeleport, RemoteCharacter };

    void SetPhase(EPhase InPhase)
    {
        Phase = InPhase;
        PhaseTime = 0.0f;
        PhaseStep = 0;
    }

    FString Label() const
    {
        return FString::Printf(TEXT("[%s, %s]"), bLagged ? TEXT("lag 150ms loss 3%") : TEXT("no lag"),
            bServerAuthority ? TEXT("server authority") : TEXT("client authority"));
    }

    bool TickWaitForPlayers()
    {
        if (Worlds.Server && Worlds.Client && Pawn)
        {
            // The CharacterMovementComponent only sends moves once possession is acknowledged on both sides.
            const APlayerController* ClientPC = Worlds.Client->GetFirstPlayerController();
            const APawn* ServerPawn = SmoothSyncNetworkTest::GetServerPlayerPawn(Worlds.Server);
            const APlayerController* ServerPC = ServerPawn ? Cast<APlayerController>(ServerPawn->GetController()) : nullptr;
            const bool bClientAcked = ClientPC && ClientPC->AcknowledgedPawn == Pawn;
            const bool bServerAcked = ServerPC && ServerPC->AcknowledgedPawn == ServerPawn;

            if ((!bClientAcked || !bServerAcked) && PhaseTime < 20.0f)
                return false;

            Test->AddInfo(FString::Printf(TEXT("%s Possession acknowledged: client %d, server %d (client Player: %s)."),
                *Label(), bClientAcked, bServerAcked, ClientPC && ClientPC->Player ? *ClientPC->Player->GetName() : TEXT("none")));

            PlayerSpawnLocation = Pawn->GetActorLocation();

            // The example pawn is client-authoritative; this variant checks the server-authoritative path instead.
            if (bServerAuthority)
            {
                for (const APawn* PlayerPawn : { static_cast<const APawn*>(Pawn), ServerPawn })
                {
                    if (USmoothSyncComponent* Sync = PlayerPawn ? PlayerPawn->FindComponentByClass<USmoothSyncComponent>() : nullptr)
                        Sync->bIsClientAuthoritative = false;
                }
            }

            SetPhase(EPhase::Settle);
            return false;
        }

        if (PhaseTime > 40.0f)
        {
            Test->AddError(TEXT("PIE server/client/pawn never became available."));
            return true;
        }
        return false;
    }

    // --- Props fall and stop; resting ones must go net dormant on the server ---
    bool TickSettle()
    {
        int32 NumProps = 0;
        int32 NumDormant = 0;
        for (TActorIterator<ASmoothPhysicsProp> It(Worlds.Server); It; ++It)
        {
            ++NumProps;
            if (const USmoothSyncComponent* Sync = It->FindComponentByClass<USmoothSyncComponent>())
                NumDormant += Sync->IsDormantAtRest() ? 1 : 0;
        }

        if (PhaseTime < 4.0f || (NumDormant == 0 && PhaseTime < 25.0f))
            return false;

        Test->AddInfo(FString::Printf(TEXT("%s Dormancy: %d / %d resting physics props dormant on the server."), *Label(), NumDormant, NumProps));
        if (NumProps > 0 && NumDormant == 0)
            Test->AddError(TEXT("No resting physics prop went dormant."));

        InertiaStartRotation = Pawn->FindComponentByClass<UStaticMeshComponent>()->GetComponentQuat();
        SetPhase(EPhase::Inertia);
        return false;
    }

    // --- Rolling ball pawn: accelerates, glides after release, rolls the same on client and server ---
    bool TickInertia()
    {
        const UStaticMeshComponent* ClientBall = Pawn->FindComponentByClass<UStaticMeshComponent>();
        const APawn* ServerPawn = SmoothSyncNetworkTest::GetServerPlayerPawn(Worlds.Server);

        // The roll is integrated locally on each machine (path dependent), so orientations may differ:
        // check that the server copy rolls whenever it moves.
        if (ServerPawn)
        {
            const FVector ServerLocation = ServerPawn->GetActorLocation();
            const FQuat ServerBallRotation = ServerPawn->FindComponentByClass<UStaticMeshComponent>()->GetComponentQuat();
            if (FVector::Dist2D(ServerLocation, LastServerPawnLocation) > 2.0)
            {
                ++ServerFramesMoving;
                ServerFramesRolling += ServerBallRotation.AngularDistance(LastServerBallRotation) > FMath::DegreesToRadians(0.5f) ? 1 : 0;
            }
            LastServerPawnLocation = ServerLocation;
            LastServerBallRotation = ServerBallRotation;
        }

        if (PhaseTime < 2.0f)
        {
            Pawn->AddMovementInput(FVector::RightVector, 1.0f, true);
            SpeedAtRelease = SmoothSyncNetworkTest::HorizontalSpeed(Pawn);
            return false;
        }

        if (PhaseStep == 0 && PhaseTime >= 2.3f)
        {
            PhaseStep = 1;
            const float SpeedAfter = SmoothSyncNetworkTest::HorizontalSpeed(Pawn);
            const float RolledDegrees = FMath::RadiansToDegrees(InertiaStartRotation.AngularDistance(ClientBall->GetComponentQuat()));
            Test->AddInfo(FString::Printf(TEXT("%s Inertia: %.0f u/s at release, %.0f u/s 0.3 s later. Ball rotated %.0f deg."), *Label(), SpeedAtRelease, SpeedAfter, RolledDegrees));

            if (SpeedAtRelease < 400.0f)
                Test->AddError(FString::Printf(TEXT("Pawn only reached %.0f u/s."), SpeedAtRelease));
            if (SpeedAfter < SpeedAtRelease * 0.5f)
                Test->AddError(TEXT("Pawn stopped almost instantly: no inertia."));
        }

        if (PhaseTime < 5.0f)
            return false;

        Test->AddInfo(FString::Printf(TEXT("%s Server copy rolled on %d of %d moving frames."), *Label(), ServerFramesRolling, ServerFramesMoving));
        if (ServerFramesMoving == 0 || ServerFramesRolling < ServerFramesMoving * 0.8f)
            Test->AddError(TEXT("The server copy of the pawn does not roll while moving."));

        // Next target: the closest prop on the ground (ideally a dormant one, which also tests waking up).
        float BestDistance = TNumericLimits<float>::Max();
        for (TActorIterator<ASmoothPhysicsProp> It(Worlds.Server); It; ++It)
        {
            if (FMath::Abs(It->GetActorLocation().Z - Pawn->GetActorLocation().Z) > 100.0f)
                continue;

            const USmoothSyncComponent* Sync = It->FindComponentByClass<USmoothSyncComponent>();
            const float Distance = FVector::Dist(It->GetActorLocation(), Pawn->GetActorLocation()) - (Sync && Sync->IsDormantAtRest() ? 100000.0f : 0.0f);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                TargetName = It->GetFName();
            }
        }

        AActor* ServerTarget = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Server, TargetName);
        if (!ServerTarget)
        {
            Test->AddError(TEXT("No physics prop to push."));
            return true;
        }

        TargetStart = ServerTarget->GetActorLocation();
        SetPhase(EPhase::Push);
        return false;
    }

    // --- Push a prop: the server must move it; the client copy near the player must stay close to it ---
    bool TickPush()
    {
        AActor* ServerTarget = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Server, TargetName);
        AActor* ClientTarget = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Client, TargetName);
        if (!ServerTarget || !ClientTarget)
        {
            Test->AddError(TEXT("Push target missing on server or client."));
            return true;
        }

        const FVector ToTarget = (ClientTarget->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();
        Pawn->AddMovementInput(ToTarget, 1.0f, true);

        // Server corrections show up as jumps that the pawn's velocity doesn't explain: on the capsule (raw corrections)
        // and on the ball mesh (what the player actually sees, after owner correction smoothing).
        const FVector PawnLocation = Pawn->GetActorLocation();
        const FVector MeshLocation = Pawn->FindComponentByClass<UStaticMeshComponent>()->GetComponentLocation();
        const FVector ExpectedStep = Pawn->GetVelocity() * FApp::GetDeltaTime();
        if (PhaseTime > 0.1f)
        {
            const FVector CapsuleJump = (PawnLocation - LastPawnLocation) - ExpectedStep;
            const FVector MeshJump = (MeshLocation - LastMeshLocation) - ExpectedStep;
            PawnPops += CapsuleJump.Size() > 25.0f ? 1 : 0;
            if (MeshJump.Size() > 25.0f)
            {
                ++VisualPops;
                Test->AddInfo(FString::Printf(TEXT("%s Visible pop at %.2fs: mesh %s, capsule %s, step %s, dt %.3f"), *Label(), PhaseTime,
                    *MeshJump.ToCompactString(), *CapsuleJump.ToCompactString(), *ExpectedStep.ToCompactString(), FApp::GetDeltaTime()));
            }
        }
        LastPawnLocation = PawnLocation;
        LastMeshLocation = MeshLocation;

        const float Displacement = FVector::Dist(ServerTarget->GetActorLocation(), TargetStart);
        if (FVector::Dist(Pawn->GetActorLocation(), ClientTarget->GetActorLocation()) < 400.0f)
            MaxNearError = FMath::Max(MaxNearError, static_cast<float>(FVector::Dist(ClientTarget->GetActorLocation(), ServerTarget->GetActorLocation())));

        if (Displacement < 600.0f && PhaseTime < 10.0f)
            return false;

        Test->AddInfo(FString::Printf(TEXT("%s Push: server moved the prop %.0f units. Max client/server gap near the player: %.0f units. Player corrections: %d, visible pops: %d."),
            *Label(), Displacement, MaxNearError, PawnPops, VisualPops));
        if (Displacement < 300.0f)
            Test->AddError(FString::Printf(TEXT("Prop barely moved when pushed (%.0f units)."), Displacement));
        if (!bServerAuthority && PawnPops > 2)
            Test->AddError(FString::Printf(TEXT("The client-authoritative player pawn was corrected %d times while pushing."), PawnPops));
        if (VisualPops > 2)
            Test->AddError(FString::Printf(TEXT("The player pawn visibly popped %d times while pushing."), VisualPops));

        SetPhase(EPhase::Rest);
        return false;
    }

    // --- Once the prop stops on the server, the client copy must stop at the same place (no overshoot) ---
    bool TickRest()
    {
        AActor* ServerTarget = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Server, TargetName);
        AActor* ClientTarget = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Client, TargetName);

        const bool bServerResting = ServerTarget->GetVelocity().Size() < 1.0f;
        RestTime = bServerResting ? RestTime + FApp::GetDeltaTime() : 0.0f;

        if (RestTime < 1.5f && PhaseTime < 20.0f)
            return false;

        // A hard kick can send the prop off the map: once it falls out of net relevancy the client legitimately stops updating it.
        const bool bLeftPlayArea = ServerTarget->GetActorLocation().Z < TargetStart.Z - 300.0f || !bServerResting;
        const float RestError = FVector::Dist(ServerTarget->GetActorLocation(), ClientTarget->GetActorLocation());
        if (bLeftPlayArea)
        {
            Test->AddWarning(FString::Printf(TEXT("%s Rest check skipped: the pushed prop left the play area or never stopped."), *Label()));
        }
        else
        {
            Test->AddInfo(FString::Printf(TEXT("%s Rest: client copy %.1f units from the server position."), *Label(), RestError));
            if (RestError > 15.0f)
                Test->AddError(FString::Printf(TEXT("Client copy did not settle on the server position (%.1f units off)."), RestError));
        }

        // Teleport the calmest other prop next (it is made kinematic for the check).
        float LowestSpeed = TNumericLimits<float>::Max();
        for (TActorIterator<ASmoothPhysicsProp> It(Worlds.Server); It; ++It)
        {
            if (It->GetFName() != TargetName && It->GetVelocity().Size() < LowestSpeed)
            {
                LowestSpeed = It->GetVelocity().Size();
                TeleportName = It->GetFName();
            }
        }

        SetPhase(EPhase::Teleport);
        return false;
    }

    // --- NotifyTeleported: the client copy must jump, never slide through the middle ---
    bool TickTeleport()
    {
        AActor* ServerProp = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Server, TeleportName);
        AActor* ClientProp = SmoothSyncNetworkTest::FindNamed<AActor>(Worlds.Client, TeleportName);
        if (!ServerProp || !ClientProp)
        {
            Test->AddError(TEXT("No resting prop available for the teleport check."));
            return true;
        }

        if (PhaseStep == 0)
        {
            PhaseStep = 1;
            TeleportFrom = ServerProp->GetActorLocation();
            TeleportTo = TeleportFrom + FVector(0.0f, 1500.0f, 0.0f);

            // Kinematic for the check, so it stays where it is put.
            if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(ServerProp->GetRootComponent()))
                Root->SetSimulatePhysics(false);

            ServerProp->SetActorLocation(TeleportTo, false, nullptr, ETeleportType::TeleportPhysics);
            ServerProp->FindComponentByClass<USmoothSyncComponent>()->NotifyTeleported();
            return false;
        }

        const FVector ClientLocation = ClientProp->GetActorLocation();
        if (FVector::Dist(ClientLocation, TeleportFrom) > 200.0f && FVector::Dist(ClientLocation, TeleportTo) > 200.0f)
            ++TeleportMidpointFrames;

        if (PhaseTime < 2.0f)
            return false;

        const float FinalError = FVector::Dist(ClientLocation, TeleportTo);
        Test->AddInfo(FString::Printf(TEXT("%s Teleport: %d frame(s) between the two points, final error %.1f units."), *Label(), TeleportMidpointFrames, FinalError));
        if (TeleportMidpointFrames > 0)
            Test->AddError(TEXT("Teleported prop slid through intermediate positions on the client."));
        if (FinalError > 20.0f)
            Test->AddError(TEXT("Teleported prop did not reach its new position on the client."));

        SetPhase(EPhase::PlayerTeleport);
        return false;
    }

    // --- The server respawns the player: it must stay there, on the server and on the owning client (any authority) ---
    bool TickPlayerTeleport()
    {
        APawn* ServerPawn = SmoothSyncNetworkTest::GetServerPlayerPawn(Worlds.Server);
        if (!ServerPawn)
            return (Test->AddError(TEXT("Player pawn missing on the server.")), true);

        if (PhaseStep == 0)
        {
            PhaseStep = 1;
            const float DistanceFromSpawn = FVector::Dist2D(ServerPawn->GetActorLocation(), PlayerSpawnLocation);
            if (DistanceFromSpawn < 300.0f)
                Test->AddWarning(FString::Printf(TEXT("%s Player respawn checked over a short distance (%.0f units)."), *Label(), DistanceFromSpawn));

            ServerPawn->SetActorLocation(PlayerSpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
            ServerPawn->FindComponentByClass<USmoothSyncComponent>()->NotifyTeleported();
            return false;
        }

        if (PhaseTime < 2.0f)
            return false;

        const float ServerError = FVector::Dist2D(ServerPawn->GetActorLocation(), PlayerSpawnLocation);
        const float ClientError = FVector::Dist2D(Pawn->GetActorLocation(), PlayerSpawnLocation);
        Test->AddInfo(FString::Printf(TEXT("%s Player respawn by the server: %.0f units off on the server, %.0f on the owning client."), *Label(), ServerError, ClientError));
        if (ServerError > 100.0f || ClientError > 100.0f)
            Test->AddError(TEXT("The player teleported by the server did not stay there."));

        SetPhase(EPhase::RemoteCharacter);
        return false;
    }

    // --- Another player seen by this client: CMC smoothing vs Smooth Sync on the same zigzag ---
    bool TickRemoteCharacter()
    {
        constexpr float WarmUp = 1.5f;
        constexpr float SegmentDuration = 6.0f;

        if (PhaseStep == 0)
        {
            PhaseStep = 1;
            const APawn* ServerPlayer = SmoothSyncNetworkTest::GetServerPlayerPawn(Worlds.Server);
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
            RemoteServerPawn = Worlds.Server->SpawnActor<ASmoothPawn>(ASmoothPawn::StaticClass(),
                ServerPlayer->GetActorLocation() + FVector(400.0f, 400.0f, 0.0f), FRotator::ZeroRotator, SpawnParams);
            if (!RemoteServerPawn)
            {
                Test->AddError(TEXT("Could not spawn the remote character."));
                return true;
            }
            RemoteServerPawn->SpawnDefaultController();
            return false;
        }

        RemoteServerPawn->AddMovementInput(SmoothSyncNetworkTest::SquarePathDirection(PhaseTime), 1.0f, true);

        APawn* ClientCopy = SmoothSyncNetworkTest::FindSimulatedPawn(Worlds.Client);
        if (!ClientCopy)
            return PhaseTime > 10.0f ? (Test->AddError(TEXT("The remote character never reached the client.")), true) : false;

        // First segment: the CMC's own smoothing. Second: Smooth Sync.
        const int32 Mode = PhaseTime < WarmUp + SegmentDuration ? 0 : 1;
        if (USmoothSyncComponent* Sync = ClientCopy->FindComponentByClass<USmoothSyncComponent>())
            Sync->bDisableSmoothing = Mode == 0;

        // Skip the start of each segment (switching systems is not part of the measure).
        const float SegmentTime = PhaseTime - WarmUp - Mode * SegmentDuration;
        RemoteSmoothness[Mode].Add(ClientCopy->GetActorLocation(), FApp::GetDeltaTime(), SegmentTime > 0.5f, 800.0f);

        if (PhaseTime < WarmUp + 2.0f * SegmentDuration)
            return false;

        Test->AddInfo(FString::Printf(TEXT("%s AI character, CMC smoothing: %s."), *Label(), *RemoteSmoothness[0].Describe()));
        Test->AddInfo(FString::Printf(TEXT("%s AI character, Smooth Sync:   %s."), *Label(), *RemoteSmoothness[1].Describe()));
        if (RemoteSmoothness[1].Pops > 0)
            Test->AddError(TEXT("The remote character popped with Smooth Sync."));

        return true;
    }

    FAutomationTestBase* Test = nullptr;
    bool bLagged = false;
    bool bServerAuthority = false;

    EPhase Phase = EPhase::WaitForPlayers;
    float PhaseTime = 0.0f;
    float TotalTime = 0.0f;
    int32 PhaseStep = 0;

    SmoothSyncNetworkTest::FPIEWorlds Worlds;
    APawn* Pawn = nullptr;

    FQuat InertiaStartRotation = FQuat::Identity;
    float SpeedAtRelease = 0.0f;
    FVector LastServerPawnLocation = FVector::ZeroVector;
    FQuat LastServerBallRotation = FQuat::Identity;
    int32 ServerFramesMoving = 0;
    int32 ServerFramesRolling = 0;

    FName TargetName;
    FVector TargetStart = FVector::ZeroVector;
    float MaxNearError = 0.0f;
    float RestTime = 0.0f;

    FVector PlayerSpawnLocation = FVector::ZeroVector;
    FVector LastPawnLocation = FVector::ZeroVector;
    FVector LastMeshLocation = FVector::ZeroVector;
    int32 PawnPops = 0;
    int32 VisualPops = 0;

    APawn* RemoteServerPawn = nullptr;
    SmoothSyncNetworkTest::FMotionSmoothness RemoteSmoothness[2];

    FName TeleportName;
    FVector TeleportFrom = FVector::ZeroVector;
    FVector TeleportTo = FVector::ZeroVector;
    int32 TeleportMidpointFrames = 0;
};


/**
 * Two clients: client A's player runs a square path, client B watches it. Compares how smooth A looks on B
 * with the CMC's own smoothing, then with Smooth Sync.
 */
class FSmoothSyncRemotePlayerCommand : public IAutomationLatentCommand
{
public:
    FSmoothSyncRemotePlayerCommand(FAutomationTestBase* InTest, bool bInLagged, bool bInListenServer)
        : Test(InTest), bLagged(bInLagged), bListenServer(bInListenServer) {}

    virtual bool Update() override
    {
        constexpr float WarmUp = 1.5f;
        constexpr float SegmentDuration = 6.0f;

        const float DeltaTime = FApp::GetDeltaTime();
        Time += DeltaTime;

        // Dedicated server: client A moves, client B watches. Listen server: the only client moves, the host watches.
        const SmoothSyncNetworkTest::FPIEWorlds Worlds = SmoothSyncNetworkTest::FindPIEWorlds();
        const bool bWorldsReady = bListenServer ? (Worlds.Server && Worlds.Clients.Num() == 1) : Worlds.Clients.Num() == 2;
        UWorld* ObserverWorld = bWorldsReady ? (bListenServer ? Worlds.Server : Worlds.Clients[1]) : nullptr;
        APawn* PawnA = bWorldsReady ? SmoothSyncNetworkTest::GetLocalPawn(Worlds.Clients[0]) : nullptr;
        APawn* CopyOnB = ObserverWorld ? (bListenServer ? FindRemotePlayerOnServer(ObserverWorld) : SmoothSyncNetworkTest::FindSimulatedPawn(ObserverWorld)) : nullptr;
        const APlayerController* PCA = PawnA ? Cast<APlayerController>(PawnA->GetController()) : nullptr;

        // Wait until A's moves reach the server (possession acknowledged) and B sees A.
        if (!StartTime.IsSet())
        {
            if (PCA && PCA->AcknowledgedPawn == PawnA && CopyOnB)
                StartTime = Time;
            else if (Time > 40.0f)
                return (Test->AddError(TEXT("Two players never became available.")), true);
            return false;
        }

        if (!PawnA || !CopyOnB)
            return (Test->AddError(TEXT("Lost a player during the test.")), true);

        // Both players spawn next to each other: first move A away from B so they never collide during the measure.
        constexpr float MoveAway = 2.0f;
        const float PhaseTime = Time - StartTime.GetValue() - MoveAway;
        if (PhaseTime < 0.0f)
        {
            const APawn* PawnB = SmoothSyncNetworkTest::GetLocalPawn(ObserverWorld);
            const FVector Away = PawnB ? (PawnA->GetActorLocation() - PawnB->GetActorLocation()).GetSafeNormal2D() : FVector::ForwardVector;
            PawnA->AddMovementInput(Away.IsNearlyZero() ? FVector::ForwardVector : Away, 1.0f, true);
            return false;
        }
        PawnA->AddMovementInput(SmoothSyncNetworkTest::SquarePathDirection(PhaseTime), 1.0f, true);

        const int32 Mode = PhaseTime < WarmUp + SegmentDuration ? 0 : 1;
        if (USmoothSyncComponent* Sync = CopyOnB->FindComponentByClass<USmoothSyncComponent>())
            Sync->bDisableSmoothing = Mode == 0;

        // Measured on the visible ball mesh (on a listen server the smoothing is visual only).
        const UStaticMeshComponent* Ball = CopyOnB->FindComponentByClass<UStaticMeshComponent>();
        const float SegmentTime = PhaseTime - WarmUp - Mode * SegmentDuration;
        Smoothness[Mode].Add(Ball->GetComponentLocation(), DeltaTime, SegmentTime > 0.5f, 800.0f);

        // Visible rotation: the ball's roll. Jitter = how much its spin speed changes from frame to frame.
        const FQuat BallRotation = Ball->GetComponentQuat();
        if (DeltaTime > 0.0f)
        {
            const float SpinSpeed = FMath::RadiansToDegrees(BallRotation.AngularDistance(LastBallRotation)) / DeltaTime;
            if (SegmentTime > 0.5f)
            {
                SpinJitterSum[Mode] += FMath::Abs(SpinSpeed - LastSpinSpeed);
                ++SpinJitterCount[Mode];
            }
            LastSpinSpeed = SpinSpeed;
        }
        LastBallRotation = BallRotation;

        if (PhaseTime < WarmUp + 2.0f * SegmentDuration)
            return false;

        const FString Label = FString::Printf(TEXT("[%s%s]"), bLagged ? TEXT("lag 150ms loss 3%") : TEXT("no lag"), bListenServer ? TEXT(", seen by listen server host") : TEXT(""));
        auto SpinJitter = [this](int32 InMode) { return SpinJitterCount[InMode] ? SpinJitterSum[InMode] / SpinJitterCount[InMode] : 0.0f; };
        Test->AddInfo(FString::Printf(TEXT("%s Remote player, CMC smoothing: %s, roll jitter %.0f deg/s."), *Label, *Smoothness[0].Describe(), SpinJitter(0)));
        Test->AddInfo(FString::Printf(TEXT("%s Remote player, Smooth Sync:   %s, roll jitter %.0f deg/s."), *Label, *Smoothness[1].Describe(), SpinJitter(1)));
        if (Smoothness[1].Pops > 0)
            Test->AddError(TEXT("The remote player popped with Smooth Sync."));
        if (Smoothness[1].Mean() >= Smoothness[0].Mean())
            Test->AddError(TEXT("The remote player is not smoother with Smooth Sync than with the CMC's smoothing."));

        return true;
    }

private:
    /** On a listen server: the pawn of a player who is not the host. */
    static APawn* FindRemotePlayerOnServer(UWorld* InServer)
    {
        for (TActorIterator<APawn> It(InServer); It; ++It)
        {
            if (It->IsPlayerControlled() && !It->IsLocallyControlled())
                return *It;
        }
        return nullptr;
    }

    FAutomationTestBase* Test = nullptr;
    bool bLagged = false;
    bool bListenServer = false;
    float Time = 0.0f;
    TOptional<float> StartTime;
    SmoothSyncNetworkTest::FMotionSmoothness Smoothness[2];
    FQuat LastBallRotation = FQuat::Identity;
    float LastSpinSpeed = 0.0f;
    float SpinJitterSum[2] = { 0.0f, 0.0f };
    int32 SpinJitterCount[2] = { 0, 0 };
};


/**
 * Two physics crates resting on a kinematic platform driven in a circle by the server (a truck bed), seen by the client
 * the way the driver sees it: the platform in the present. One crate is synced in world space, the other got
 * SetSyncBase(platform) when it was loaded. Compares where the client shows each crate on the platform.
 */
class FSmoothSyncMovingBaseCommand : public IAutomationLatentCommand
{
public:
    FSmoothSyncMovingBaseCommand(FAutomationTestBase* InTest, bool bInLagged)
        : Test(InTest), bLagged(bInLagged) {}

    virtual bool Update() override
    {
        const float DeltaTime = FApp::GetDeltaTime();
        Time += DeltaTime;
        if (Time > 90.0f)
            return (Test->AddError(TEXT("Moving base test timed out.")), true);

        const SmoothSyncNetworkTest::FPIEWorlds Worlds = SmoothSyncNetworkTest::FindPIEWorlds();
        const APawn* ServerPlayer = Worlds.Server ? SmoothSyncNetworkTest::GetServerPlayerPawn(Worlds.Server) : nullptr;

        if (!ServerPlatform)
        {
            if (!ServerPlayer || !Worlds.Client || Time < 3.0f)
                return Time > 40.0f ? (Test->AddError(TEXT("PIE server/client never became available.")), true) : false;

            // High above the map: nothing else touches the platform or the crates.
            Center = ServerPlayer->GetActorLocation() + FVector(-Radius, 0.0f, 2000.0f);
            if (!SpawnActors(Worlds.Server))
                return (Test->AddError(TEXT("Could not spawn the platform and the crates.")), true);
            StartTime = Time;
            return false;
        }

        // Drive the platform: speed ramps up once the crates settled, the platform faces its direction of travel.
        const float RunTime = Time - StartTime - SettleTime;
        if (RunTime > 0.0f)
        {
            const float CurrentSpeed = Speed * FMath::Clamp(RunTime / RampTime, 0.0f, 1.0f);
            Angle += CurrentSpeed / Radius * DeltaTime;
            ServerPlatform->SetActorLocationAndRotation(Center + Radius * FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f),
                FRotator(0.0f, FMath::RadiansToDegrees(Angle) + 90.0f, 0.0f));
        }

        AActor* ClientPlatform = FindClientPlatform(Worlds.Client);
        AActor* ClientCrates[2] = { nullptr, nullptr };
        if (ClientPlatform)
            FindClientCrates(Worlds.Client, ClientPlatform, ClientCrates);
        if (!ClientPlatform || !ClientCrates[0] || !ClientCrates[1])
            return RunTime > 5.0f ? (Test->AddError(TEXT("The platform or the crates never reached the client.")), true) : false;

        // The client sees the platform the way its driver would: in the present, like a locally driven vehicle.
        if (USmoothSyncComponent* PlatformSync = ClientPlatform->FindComponentByClass<USmoothSyncComponent>())
            PlatformSync->bDisableSmoothing = true;
        ClientPlatform->SetActorTransform(ServerPlatform->GetActorTransform());

        if (RunTime <= 0.0f)
            return false;

        // Pops over the whole ride (start included), place on the platform once at full speed.
        const bool bFullSpeed = RunTime > RampTime + 1.0f;
        for (int32 i = 0; i < 2; ++i)
        {
            Smoothness[i].Add(ClientCrates[i]->GetActorLocation(), DeltaTime, true, Speed);
            if (!bFullSpeed)
                continue;

            // The crate's place on the platform, on each machine. The server one barely changes while the crate rests.
            const FVector ServerOnPlatform = ServerPlatform->GetActorTransform().InverseTransformPositionNoScale(ServerCrates[i]->GetActorLocation());
            const FVector ClientOnPlatform = ClientPlatform->GetActorTransform().InverseTransformPositionNoScale(ClientCrates[i]->GetActorLocation());
            const float Error = FVector::Dist(ServerOnPlatform, ClientOnPlatform);
            ErrorSum[i] += Error;
            MaxError[i] = FMath::Max(MaxError[i], Error);
            ++ErrorCount[i];

            FirstServerOnPlatform[i] = FirstServerOnPlatform[i].Get(ServerOnPlatform);
            MaxServerDrift = FMath::Max(MaxServerDrift, static_cast<float>(FVector::Dist(ServerOnPlatform, FirstServerOnPlatform[i].GetValue())));
        }

        if (RunTime < RampTime + 1.0f + MeasureTime)
            return false;

        const FString Label = bLagged ? TEXT("[lag 150ms loss 3%]") : TEXT("[no lag]");
        for (int32 i = 0; i < 2; ++i)
        {
            Test->AddInfo(FString::Printf(TEXT("%s Crate on a platform at %.0f u/s, %s: client shows it %.1f units off its place on average, %.1f max. %s."),
                *Label, Speed, i == 0 ? TEXT("world space ") : TEXT("SetSyncBase"), ErrorCount[i] ? ErrorSum[i] / ErrorCount[i] : 0.0f, MaxError[i], *Smoothness[i].Describe()));
        }
        Test->AddInfo(FString::Printf(TEXT("%s The crates slid up to %.1f units on the server platform during the measure."), *Label, MaxServerDrift));

        if (MaxServerDrift > 50.0f)
            Test->AddWarning(TEXT("A crate did not stay put on the server platform: the measure is less meaningful."));
        if (MaxError[1] > 15.0f + MaxServerDrift)
            Test->AddError(TEXT("With SetSyncBase, the client copy does not stay glued to the platform."));
        if (Smoothness[1].Pops > 0)
            Test->AddError(TEXT("The crate with SetSyncBase popped on the client during the ride."));

        return true;
    }

private:
    static constexpr float Radius = 2000.0f;
    static constexpr float Speed = 700.0f;
    static constexpr float RampTime = 3.0f;
    static constexpr float SettleTime = 2.0f;
    static constexpr float MeasureTime = 6.0f;

    /** Crates sit side by side across the bed: world space on the left (-Y), SetSyncBase on the right (+Y). */
    static constexpr float CrateSideOffset = 75.0f;

    /** Spawns a cube-shaped actor. The mesh is set before BeginPlay (the crates start simulating there). */
    template<typename ActorType>
    static AActor* SpawnCube(UWorld* InServer, const FTransform& InTransform)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        ActorType* Actor = Cube ? InServer->SpawnActorDeferred<ActorType>(ActorType::StaticClass(), InTransform) : nullptr;
        if (!Actor)
            return nullptr;

        Actor->template FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Cube);
        Actor->FinishSpawning(InTransform);
        return Actor;
    }

    bool SpawnActors(UWorld* InServer)
    {
        // A 6 x 3 m bed facing its direction of travel, driven by this test instead of its own circling.
        const FTransform PlatformTransform(FRotator(0.0f, 90.0f, 0.0f), Center + FVector(Radius, 0.0f, 0.0f), FVector(6.0f, 3.0f, 0.4f));
        ServerPlatform = SpawnCube<ASmoothProp>(InServer, PlatformTransform);
        if (!ServerPlatform)
            return false;

        ServerPlatform->SetActorTickEnabled(false);
        UPrimitiveComponent* Bed = Cast<UPrimitiveComponent>(ServerPlatform->GetRootComponent());
        Bed->SetCollisionProfileName(TEXT("BlockAll"));

        for (int32 i = 0; i < 2; ++i)
        {
            const FVector OnBed(0.0f, i == 0 ? -CrateSideOffset : CrateSideOffset, 80.0f);
            ServerCrates[i] = SpawnCube<ASmoothPhysicsProp>(InServer, FTransform(PlatformTransform.GetLocation() + PlatformTransform.GetRotation().RotateVector(OnBed)));
            if (!ServerCrates[i])
                return false;

            // Only this test's moves are measured: the crates never sleep on the server.
            USmoothSyncComponent* Sync = ServerCrates[i]->FindComponentByClass<USmoothSyncComponent>();
            Sync->bUseDormancyWhenAtRest = false;
            if (i == 1)
                Sync->SetSyncBase(Bed);
        }
        return true;
    }

    /** The test's actors are the only ones of these exact classes high above the map (the level uses Blueprint subclasses). */
    AActor* FindClientPlatform(UWorld* InClient) const
    {
        for (TActorIterator<ASmoothProp> It(InClient); It; ++It)
        {
            if (It->GetClass() == ASmoothProp::StaticClass() && It->GetActorLocation().Z > Center.Z - 500.0f)
                return *It;
        }
        return nullptr;
    }

    void FindClientCrates(UWorld* InClient, const AActor* InClientPlatform, AActor* OutCrates[2]) const
    {
        for (TActorIterator<ASmoothPhysicsProp> It(InClient); It; ++It)
        {
            if (It->GetClass() == ASmoothPhysicsProp::StaticClass() && It->GetActorLocation().Z > Center.Z - 500.0f)
            {
                const double Side = InClientPlatform->GetActorTransform().InverseTransformPositionNoScale(It->GetActorLocation()).Y;
                OutCrates[Side < 0.0 ? 0 : 1] = *It;
            }
        }
    }

    FAutomationTestBase* Test = nullptr;
    bool bLagged = false;
    float Time = 0.0f;
    float StartTime = 0.0f;
    float Angle = 0.0f;
    FVector Center = FVector::ZeroVector;
    AActor* ServerPlatform = nullptr;
    AActor* ServerCrates[2] = { nullptr, nullptr };

    TOptional<FVector> FirstServerOnPlatform[2];
    float MaxServerDrift = 0.0f;
    float ErrorSum[2] = { 0.0f, 0.0f };
    float MaxError[2] = { 0.0f, 0.0f };
    int32 ErrorCount[2] = { 0, 0 };
    SmoothSyncNetworkTest::FMotionSmoothness Smoothness[2];
};


/** Only this test's own checks decide the result: errors logged by other plugins of the host project are ignored. */
class FSmoothSyncNetworkTestBase : public FAutomationTestBase
{
public:
    FSmoothSyncNetworkTestBase(const FString& InName, const bool bInComplexTask) : FAutomationTestBase(InName, bInComplexTask) {}
    virtual bool SuppressLogErrors() override { return true; }
    virtual bool SuppressLogWarnings() override { return true; }
};

IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FSmoothSyncNetworkScenariosTest, FSmoothSyncNetworkTestBase, "SmoothSync.Network.Scenarios",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

void FSmoothSyncNetworkScenariosTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
    OutBeautifiedNames.Add(TEXT("NoLag"));
    OutTestCommands.Add(TEXT("0"));
    OutBeautifiedNames.Add(TEXT("Lag150ms_Loss3"));
    OutTestCommands.Add(TEXT("1"));
    OutBeautifiedNames.Add(TEXT("Lag150ms_Loss3_ServerAuthority"));
    OutTestCommands.Add(TEXT("2"));
}

bool FSmoothSyncNetworkScenariosTest::RunTest(const FString& Parameters)
{
    const bool bLagged = Parameters != TEXT("0");
    const bool bServerAuthority = Parameters == TEXT("2");

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(SmoothSyncNetworkTest::MapPath));
    ADD_LATENT_AUTOMATION_COMMAND(FSmoothSyncStartNetworkPIE(bLagged));
    ADD_LATENT_AUTOMATION_COMMAND(FSmoothSyncScenarioCommand(this, bLagged, bServerAuthority));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    return true;
}


IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FSmoothSyncRemotePlayerTest, FSmoothSyncNetworkTestBase, "SmoothSync.Network.RemotePlayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

void FSmoothSyncRemotePlayerTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
    OutBeautifiedNames.Add(TEXT("NoLag"));
    OutTestCommands.Add(TEXT("0"));
    OutBeautifiedNames.Add(TEXT("Lag150ms_Loss3"));
    OutTestCommands.Add(TEXT("1"));
    OutBeautifiedNames.Add(TEXT("ListenServerHost_Lag150ms_Loss3"));
    OutTestCommands.Add(TEXT("2"));
}

bool FSmoothSyncRemotePlayerTest::RunTest(const FString& Parameters)
{
    const bool bLagged = Parameters != TEXT("0");
    const bool bListenServer = Parameters == TEXT("2");

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(SmoothSyncNetworkTest::MapPath));
    ADD_LATENT_AUTOMATION_COMMAND(FSmoothSyncStartNetworkPIE(bLagged, 2, bListenServer));
    ADD_LATENT_AUTOMATION_COMMAND(FSmoothSyncRemotePlayerCommand(this, bLagged, bListenServer));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    return true;
}


IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FSmoothSyncMovingBaseTest, FSmoothSyncNetworkTestBase, "SmoothSync.Network.MovingBase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

void FSmoothSyncMovingBaseTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
    OutBeautifiedNames.Add(TEXT("NoLag"));
    OutTestCommands.Add(TEXT("0"));
    OutBeautifiedNames.Add(TEXT("Lag150ms_Loss3"));
    OutTestCommands.Add(TEXT("1"));
}

bool FSmoothSyncMovingBaseTest::RunTest(const FString& Parameters)
{
    const bool bLagged = Parameters != TEXT("0");

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(SmoothSyncNetworkTest::MapPath));
    ADD_LATENT_AUTOMATION_COMMAND(FSmoothSyncStartNetworkPIE(bLagged));
    ADD_LATENT_AUTOMATION_COMMAND(FSmoothSyncMovingBaseCommand(this, bLagged));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    return true;
}

#endif
