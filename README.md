# Smooth Network Sync

Smooth, bandwidth-friendly transform replication for any actor, in one component.

Drop the **Smooth Sync** component on a replicated actor and its movement looks smooth on every client, even with lag and packet loss: no more jittery props, sliding teleports or objects overshooting when they stop.

Full documentation: [Notion](https://app.notion.com/p/Smooth-Network-Sync-37d610df67648085a295c1b607f5ae3d?source=copy_link)

**Features**

- Smoother remote players: on Characters, Smooth Sync takes over how other players are displayed. In the automated tests at 150 ms ping and 3% loss, a zigzagging character shows 13x less mean jerk than with the CharacterMovementComponent's own smoothing.
- Smooth server-authoritative players: corrections slide instead of popping, and a provided movement component avoids most corrections when pushing physics props.
- Client-authoritative Characters: no server corrections for the owning player (even when pushing physics props), with a server speed check.
- Adaptive interpolation: the display delay follows the measured network quality.
- Capped extrapolation when packets are late, with smooth recovery instead of pops.
- Objects near the local player are shown in the player's present, so pushing and catching line up.
- Server-authoritative pawns with soft owner correction, or client-authoritative pawns with anti-cheat speed checks.
- Explicit teleports, sync relative to a moving parent (attachments, platforms, cargo in vehicles), optional scale and angular velocity.
- Compact wire format, push-model replication, distance LOD and net dormancy for resting objects.
- Debug drawing, `stat SmoothSync`, and automated multiplayer tests.

---

## Contents

1. [Quick start](#quick-start)
2. [Characters (players)](#characters-players)
3. [Presets](#presets)
4. [Common setups](#common-setups)
5. [Blueprint and C++ API](#blueprint-and-c-api)
6. [Settings reference](#settings-reference)
7. [How it works](#how-it-works)
8. [Performance](#performance)
9. [Debugging and testing](#debugging-and-testing)
10. [Troubleshooting](#troubleshooting)
11. [Notes](#notes)

---

## Quick start

1. Make sure your actor replicates (**Replicates** checked, or `bReplicates = true`).
2. Add the **Smooth Sync** component.
3. Pick a **Preset** in the details panel: `Prop`, `Physics Object` or `Vehicle`.

That's it. On regular actors the component turns off **Replicate Movement** (it replaces it); on Characters it keeps it on (see [Characters](#characters-players)). Setup mistakes are reported in the Output Log (category `LogSmoothSync`) and through Data Validation.

In C++:

```cpp
#include "Components/SmoothSyncComponent.h"

AMyProp::AMyProp()
{
    bReplicates = true;

    SmoothSync = CreateDefaultSubobject<USmoothSyncComponent>(TEXT("SmoothSync"));
    SmoothSync->ApplyPreset(ESmoothSyncPreset::Prop);
}
```

The example map is `Content/Examples/Maps/MapTest_SmoothSync`. Play it with **Net Mode: Play As Client** to see every case.

---

## Characters (players)

Add Smooth Sync to your Character like any other actor. It works **with** the CharacterMovementComponent instead of replacing it:

| Who | Handled by |
|---|---|
| The owning player's own character | The CMC's client prediction (unchanged). |
| The server | The CMC simulates the moves and pushes physics props (unchanged). |
| **Other players' characters** on each client | **Smooth Sync**: adaptive interpolation instead of the CMC's smoothing. The CMC's velocity is fed with the smoothed one, so animation blueprints keep working. |
| **Remote players seen by a listen server host** | **Smooth Sync** (**Smooth Remote Players On Listen Server**): the collision follows the received moves, the visuals (the root's children) follow them smoothly over **Listen Server Smoothing Time** (0.08 s). |

**Replicate Movement must stay on** for Characters: the CMC only sends the owning client's moves to the server while it is on (with it off, the character moves on its owner's screen but never on the server). The component turns it back on if needed.

### Server-authoritative or client-authoritative?

| Mode | Owner's experience | Cheating | When |
|---|---|---|---|
| Server-authoritative (default) | The server corrects the player when positions differ. Smooth Sync hides corrections (see below), and `USmoothSyncCharacterMovementComponent` avoids most of them when touching physics props. | Protected | Competitive games |
| Client-authoritative (**Is Client Authoritative**) | **Never corrected**: the server still simulates the player's moves (props get pushed), then accepts the player's position. | **Max Client Move Speed** (horizontal) is checked every 0.25 s; on violation the server takes over and corrects the player for one second. | Co-op, physics-heavy games |

Measured in the automated tests (150 ms ping, 3% loss, pushing a physics ball): client authority gives 0 corrections; server authority with both techniques below gives about one correction and at most one small visible pop.

**Speed hacks in client-authoritative mode**: **Max Client Move Speed** catches positions that move too fast for the client's move clock, but a cheater can also speed up that clock. Turn on the engine's time discrepancy detection in your project's `Config/DefaultGame.ini` (the last line makes it correct client-authoritative players too):

```ini
[/Script/Engine.GameNetworkManager]
bMovementTimeDiscrepancyDetection=true
bMovementTimeDiscrepancyResolution=true
bMovementTimeDiscrepancyForceCorrectionsDuringResolution=true
```

**Respawns in client-authoritative mode**: moving the player on the server (respawn, teleport) and calling **Notify Teleported** is enough. The server stops trusting the client for one second, so the client is corrected to the new position instead of overriding it.

### Smooth corrections for server-authoritative players

Two techniques, usable together:

1. **Hide corrections** (on by default, **Smooth Owner Corrections**): when the server corrects the owning player, the collision snaps right away, but the visuals (the root's children: meshes, camera boom) slide back over **Owner Correction Smoothing Time** (0.15 s) instead of popping. Corrections larger than **Max Owner Correction Smoothing Distance** (200) still snap.
2. **Avoid corrections near physics objects** (`USmoothSyncCharacterMovementComponent`): client and server never collide with a physics object at exactly the same place, and the stock CMC corrects anything above ~1.7 units. While the character touches a simulating body, this movement component tolerates **Physics Contact Error Tolerance** (50 units) and keeps the client's position. Outside of contacts, corrections are as strict as usual.

```cpp
AMyCharacter::AMyCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<USmoothSyncCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
}
```

The example pawn (`ASmoothPawn`) uses both, and is client-authoritative with `Max Client Move Speed` 1500. Uncheck **Is Client Authoritative** on its Smooth Sync component to try the server-authoritative mode.

**Bandwidth note**: Characters keep the engine's movement replication (needed by the CMC) on top of Smooth Sync's snapshots, so remote characters cost a bit more bandwidth than with the CMC alone.

---

## Presets

Picking a preset fills the related settings. Editing one of them afterwards switches the preset back to `Custom`.

| Preset | For | Rates (pos / rot) | Delay | Max extrapolation | Extras |
|---|---|---|---|---|---|
| `Prop` | Objects moved by gameplay code on the server: doors, platforms, pickups, AI | 20 / 20 Hz | 0.10 s | 0.25 s | |
| `Physics Object` | Objects simulated by physics on the server: crates, debris, balls | 30 / 30 Hz | 0.10 s | 0.30 s | Angular velocity, shown in the present near the local player |
| `Vehicle` | Fast player-driven pawns, usually client-authoritative | 30 / 30 Hz | 0.08 s | 0.50 s | Angular velocity, Max Client Move Speed 6000 |

---

## Common setups

### Physics objects pushed by players

1. Preset `Physics Object`. Its **Display In Present Near Local Player** keeps the client copy where the server object really is when the player is close, so the player collides with it at the right place (pushing, jumping on it).
2. Simulate physics **on the server only**. Client copies stay kinematic and follow the snapshots; a simulating client copy would fight them every frame.
3. Keep the copy **blocking the Pawn channel** on clients. If it ignored pawns, the player would sink into it locally while standing on it on the server, and get ejected by the server's correction.
4. Give the object some **damping**. Chaos has no rolling resistance, so an undamped ball rolls forever and never comes to rest.
5. Characters push props with their CMC (**Push Force Factor**). The default (750000, not scaled to mass) is tuned for heavy crates and launches light props across the map. For light props, enable **Push Force Scaled To Mass** and lower the factor (the example pawn uses 6000, with **Initial Push Force Factor** 1000).

```cpp
void AMyPhysicsProp::BeginPlay()
{
    Super::BeginPlay();

    Mesh->SetSimulatePhysics(HasAuthority());
}
```

`ASmoothPhysicsProp` in the example module does all of this.

### Many props in a level

Enable **Use Dormancy When At Rest**. Once an object has rested for **Dormancy Delay**, the server stops replicating the whole actor until it moves again, so resting props cost nothing on the server.

Only use it on actors whose other replicated properties don't change while they rest: a dormant actor replicates nothing.

### Player vehicle with local physics (client-authoritative)

1. Preset `Vehicle`, check **Is Client Authoritative**.
2. Set **Max Client Move Speed** above the vehicle's top speed. The server rejects faster moves and snaps the client back.
3. Implement **Smooth Syncable** if the vehicle needs to apply the transform itself (see [the API](#applying-the-transform-yourself)).

The owning client sends its transform. The server checks it, interpolates it like a remote copy, and relays the original snapshots to the other clients. The owner never receives its own transform back.

### Server-authoritative pawn (not a Character)

Leave **Is Client Authoritative** off. Send your input to the server yourself; the server moves the pawn and Smooth Sync replicates it. On the owning client, the pawn is softly pulled toward the server position (**Owner Correction** settings): a spring for physics bodies, an interpolation otherwise.

### Teleports and respawns

Call **Notify Teleported** right after moving the actor, on the machine that moved it. Clients then snap instead of sliding across the map.

- On the server: works in both modes. In client-authoritative mode the owning client is moved to the new transform too, and its states are ignored until it acknowledges it (Characters: the CMC corrects it).
- On the owning client (client-authoritative mode): the server accepts the jump without speed checks.

Jumps far larger than the expected travel (**Teleport Distance Threshold**) also snap.

### Objects on moving parents

When the actor's root is attached to another component (a ship, an elevator, a vehicle), its transform is synced relative to the parent (**Sync Relative To Parent**, on by default). It stays glued to the parent on every client, even while the parent moves.

The same setting covers **Characters standing on moving platforms** (elevators, ship decks, moving floors): while the character's movement base moves, its position is synced relative to that base, so remote copies ride the platform instead of sliding behind it. The base must be replicated and moved by gameplay code (kinematic); physics bodies (balls, crates) are never used as a base.

### Cargo: physics objects carried by a vehicle

Physics crates in a truck bed, barrels on a ship deck: they can't be attached (they must keep simulating), and in world space their remote copies trail behind the vehicle. The player driving sees it worst, since their vehicle is shown in the present and the crates arrive with the network delay: at 700 u/s with 150 ms of ping, crates are shown about 2 m off their place in the bed.

On the server, call **Set Sync Base** on the crate's Smooth Sync with the vehicle's component (its mesh or a cargo bed component), typically from the overlap of a cargo volume, and **Set Sync Base (None)** when it leaves:

```cpp
void AMyTruck::OnCargoBeginOverlap(UPrimitiveComponent*, AActor* Other, UPrimitiveComponent*, int32, bool, const FHitResult&)
{
    if (USmoothSyncComponent* Sync = Other->FindComponentByClass<USmoothSyncComponent>())
        Sync->SetSyncBase(BedMesh);
}

void AMyTruck::OnCargoEndOverlap(UPrimitiveComponent*, AActor* Other, UPrimitiveComponent*, int32)
{
    if (USmoothSyncComponent* Sync = Other->FindComponentByClass<USmoothSyncComponent>())
        Sync->SetSyncBase(nullptr);
}
```

The crates keep simulating on the server, and their snapshots are relative to the vehicle: every client, the driver included, shows them in their place in the bed (measured: 0.1 units off at 700 u/s with 150 ms of ping). A crate resting in the bed sends nothing at all while the vehicle drives, and goes dormant if **Use Dormancy When At Rest** is on. The base can be a physics body (a vehicle usually is); it must belong to a replicated actor.

Switching a base on or off while the vehicle moves is smooth, but the crate catches up with its place over a tenth of a second: set the base when loading, not at full speed.

### Scale

Check **Sync Scale** to replicate and interpolate the 3D scale. It costs bandwidth only when enabled.

---

## Blueprint and C++ API

### Applying the transform yourself

By default the component calls `SetActorTransform` (and sets the physics velocities when the root simulates). To move only a visual mesh, feed an animation, or drive physics your own way, implement the **Smooth Syncable** interface on the actor:

```cpp
class AMyVehicle : public APawn, public ISmoothSyncable
{
    virtual void ApplySmoothedTransform_Implementation(const FTransform& InNewTransform, const FVector& InNewVelocity) override;
};
```

It is called every frame on remote copies, and on the server for client-authoritative owners. Bind **On Hard Snap Triggered** to know when a move is a teleport.

### Functions

| Name | Call it on | What it does |
|---|---|---|
| `NotifyTeleported()` | Server (owning client when client-authoritative) | Remote copies snap instead of sliding. |
| `SetSyncBase(Component)` | Server | Syncs the owner relative to a moving component without attaching it (cargo). None goes back to world space. |
| `GetSyncBase()` | Server | The base set with `SetSyncBase`. |
| `ApplyPreset(Preset)` | Anywhere | Applies a preset's values. |
| `ResetInterpolation()` | Clients | Drops received snapshots; the next one is applied directly. |
| `IsLagging()` | Clients | Snapshots stopped arriving and extrapolation is capped. |
| `GetCurrentInterpolationDelay()` | Clients | Current display delay in seconds. |
| `GetPredictionError()` | Owning client | Distance between the local pawn and the server's position. |
| `IsDormantAtRest()` | Server | The actor is net dormant because it rests. |

### Events

| Name | Fired when |
|---|---|
| `OnNetworkLagDetected` | Snapshots stopped arriving while the object was moving. |
| `OnNetworkLagRecovered` | Snapshots arrive again. |
| `OnHardSnapTriggered` | The object was snapped (teleport or large correction). Once per snap. |

---

## Settings reference

Basic settings are visible by default; tuning settings are under the **Advanced** arrow of each category *(adv.)*.

| Category | Setting | Default | Meaning |
|---|---|---|---|
| Smooth Sync | Preset | Custom | Fills the related settings. |
| | Is Client Authoritative | off | The owning client sends its transform. |
| | Sync Scale | off | Replicate the 3D scale. |
| | Disable Smoothing | off | Stop moving the owner on this machine (local physics, cinematics). |
| Network | Position / Rotation Update Rate | 20 / 30 Hz | Snapshots sent per second, capped by the actor's Net Update Frequency. |
| | Sync Angular Velocity | off | Keep fast spins correct at low rates, extrapolate rotation. |
| | Use Dormancy When At Rest | off | Stop replicating resting actors. |
| | Sync Relative To Parent | on | Sync relative to the attach parent, the Set Sync Base component, or a Character's moving platform. |
| | Position Precision *(adv.)* | 1 mm | 1 cm, 1 mm or 0.1 mm on the wire. |
| | Distance LOD *(adv.)* | on | Lower rates beyond 5000 / 15000 units from every player. |
| Interpolation | Interpolation Delay | 0.1 s | How far in the past remote copies are shown (minimum when adaptive). |
| | Adaptive Interpolation Delay | on | Grow the delay on bad connections, up to Max Interpolation Delay (0.35 s). |
| | Max Extrapolation Time | 0.5 s | How long to keep moving a copy when packets stop. |
| | Teleport Distance Threshold | 1000 | Unexplained jumps beyond this snap. |
| | Error Smoothing Time *(adv.)* | 0.1 s | Blend out contradicted extrapolation instead of popping. |
| Local Player | Display In Present Near Local Player | off | Show nearby copies in the player's present. |
| | Present Time Near / Far Distance | 400 / 1200 | Fully in the present below Near, back to normal beyond Far. |
| Owner Correction | Smooth Owner Corrections | on | Characters: server corrections slide the visuals instead of popping. |
| | Prediction Error Tolerance | 50 | Non-Character pawns: error tolerated before correcting the owning client. |
| Listen Server | Smooth Remote Players On Listen Server | on | Characters: smooth remote players' visuals on the host. |
| | Listen Server Smoothing Time | 0.08 s | How quickly the host's visuals catch up with received moves. |
| Client Authority | Max Client Move Speed | 0 (off) | Server rejects faster client moves. Set it in client-authoritative mode. |
| Debug | Show Debug Path | off | Draw this component's debug info. |

---

## How it works

- **Sending (server)**: the transform is sampled at the update rates. A snapshot is sent only when something changed, plus one final "at rest" snapshot when the object stops, so remote copies never overshoot. Rates drop for objects far from every player.
- **Wire format**: quantized position, velocity only while moving, rotation as "smallest three" (50 bits), timestamps as 16-bit milliseconds, scale and angular velocity only when enabled.
- **Receiving (clients)**: snapshots go into a small ring buffer. Copies are shown **Interpolation Delay** seconds in the past with cubic Hermite interpolation using the sent velocities. Past the newest snapshot, they are extrapolated for at most **Max Extrapolation Time**. When a new snapshot contradicts what was shown, the difference is blended out.
- **Idle cost**: a copy that reached its resting snapshot is skipped until the next one arrives.
- **One tick for all**: a world subsystem updates every component in a single loop; components have no tick of their own.

---

## Performance

- Enable push-model replication: `Net.IsPushModelEnabled=1` (`DefaultEngine.ini`, `[SystemSettings]`). The component marks its properties dirty only when it sends.
- Enable **Use Dormancy When At Rest** on props that rest most of the time.
- Keep the actor's **Net Update Frequency** at or above the update rates (a warning is logged otherwise).
- Use **Position Precision 1 cm** for props that don't need finer accuracy.
- Lower the update rates of slow or distant objects; the adaptive delay compensates.

---

## Debugging and testing

- **Debug drawing**: console `smoothsync.Debug 1` (all components) or **Show Debug Path** (one component). Shows buffered snapshots, delay, state (interpolating, extrapolating, lagging, present time), bytes per second and snap count, plus an on-screen summary per server and client.
- **Stats**: console `stat SmoothSync`: tick cost, component count, idle and dormant components, snapshots sent and received, snaps.
- **Bad connections in PIE**: Editor Preferences > Level Editor > Play > Multiplayer Options > **Network Emulation**.
- **Automated tests**: Session Frontend > Automation, filter `SmoothSync`, or run
  `UnrealEditor <Project>.uproject -ExecCmds="Automation RunTests SmoothSync; Quit"`.
  Unit tests cover the wire format and buffers. Network tests play the example map with a dedicated server and a client, without lag and with 150 ms ping / 3% loss (client and server authority), and check dormancy, pushing, player corrections, rest convergence, teleports, the example pawn, cargo on a moving platform as seen by its driver (world space vs `SetSyncBase`), and compare how smooth a remote character (server-driven, and another player seen by a second client) looks with the CMC's smoothing vs Smooth Sync.

---

## Troubleshooting

**Nothing moves on clients.**
The actor must replicate (**Replicates** checked) and its root must be **Movable**. Both are reported in the log.

**My Character moves on its own screen but not on the server or for other players.**
**Replicate Movement** is off on a Character. Turn it back on: the CharacterMovementComponent needs it to send moves (see [Characters](#characters-players)).

**My player jitters or gets teleported back when bumping props.**
The server corrects the player because client and server never collide at exactly the same place. Use `USmoothSyncCharacterMovementComponent` and keep **Smooth Owner Corrections** on (see [Smooth corrections](#smooth-corrections-for-server-authoritative-players)), or make the Character client-authoritative (**Is Client Authoritative** + **Max Client Move Speed**).

**Props fly away when a Character touches them.**
The CMC's default push is too strong for light props. In the Character Movement component, enable **Push Force Scaled To Mass** and lower **Push Force Factor** (3000 to 6000 is a good start).

**The player gets stuck on props or pushes them weakly.**
Enable **Display In Present Near Local Player** on the prop (on in the `Physics Object` preset): otherwise the client copy is shown slightly in the past and blocks the player where the prop *was*.

**The player sinks into a prop and gets ejected when jumping on it.**
The client copy ignores the Pawn channel. Keep it blocking pawns so client and server collide the same way.

**The camera shakes when the player bumps into props.**
Client and server never collide at exactly the same place, so the server corrects the player by a few units. Enable **Camera Lag** on the spring arm (the example pawn uses a lag speed of 20) to hide these corrections.

**Physics objects never stop (and never go dormant).**
Add linear and angular damping to the body.

**Objects slide across the map when respawned.**
Call `NotifyTeleported()` on the server after moving them.

**Physics objects in a moving vehicle trail behind it or sink into it on clients.**
Call `SetSyncBase(VehicleComponent)` on the server while they are in the vehicle (see [Cargo](#cargo-physics-objects-carried-by-a-vehicle)).

**Movement stutters on a bad connection.**
Raise **Max Interpolation Delay**, or the update rates. Check `smoothsync.Debug 1`: red means snapshots stopped arriving.

**"NetUpdateFrequency is lower than the update rates" in the log.**
Raise the actor's Net Update Frequency, or lower the update rates.

---

## Notes

- **Engine**: Unreal Engine 5.3+ (dynamic replication conditions). Compiled and tested on 5.7.
- **Iris**: supported through Iris' generic struct serialization (declared in the plugin's `Config/DefaultEngine.ini`). Works, with larger packets than the default replication system.
- **Modules**: `SmoothNetworkSync` (runtime, the only one your game needs), `SmoothNetworkSyncExamples` (demo actors used by the example map), `SmoothNetworkSyncTests` (editor-only automated tests).
- **Upgrading from 1.0**: the component was renamed from `UNetworkInterpolatorComponent` to `USmoothSyncComponent`. Existing assets are redirected automatically; update C++ includes to `Components/SmoothSyncComponent.h` and `Interfaces/SmoothSyncable.h`.
