// Automation specs for the projectile network lifecycle.
//
// Single-process specs that prove the *mechanism* of the client-simulated flight
// + teardown fix: the bNetTemporary CDO family split, the replicated-velocity PMC
// seed in isolation, the un-gated Explode teardown, and the authority-only damage
// gate. The networked truth (client copies fly and despawn over a real
// server+2-client pair, no stale accumulation) is proven by
// the multi-process net harness, NOT here.
// A standalone test world is always ROLE_Authority, so
// the client path is reached by forcing SetRole(ROLE_SimulatedProxy) on the actor
// under test (HasAuthority() then reads false); this is the single-process seam
// for the role-gated behavior, and it is honestly weaker than the live harness.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTSoulBarbProjectile.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"
#include "../WoTDecal.h"
#include "../WoTBeamEffect.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Projectiles/WoTFireworksProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "../Projectiles/WoTIceProjectile.h"
#include "../Projectiles/WoTWallOfAirProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTTracerProjectile.h"
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../WoTVertexAnimComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/SkeletalMesh.h"
#include "../WoTCharacter.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	// Shared minimal ticking game world. NM_Standalone
	// => spawned actors are ROLE_Authority; the client path is reached by forcing
	// SetRole(ROLE_SimulatedProxy) on the actor under test.
	FActorSpawnParameters ProjAlwaysSpawnParams()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}
}

// The bNetTemporary CDO family split the whole fix keys on: moving Generic weaves
// are temporaries (the broken set); persistent-channel seekers + WallOfAir are not
// (must stay driven by the replicated stream, untouched by the client seed).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProjFamilySplitSpec,
	"WoT.Net.Projectile.FamilySplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProjFamilySplitSpec::RunTest(const FString&)
{
	TestTrue(TEXT("Dart is bNetTemporary"), GetDefault<AWoTDartProjectile>()->bNetTemporary);
	TestTrue(TEXT("Fireball is bNetTemporary"), GetDefault<AWoTFireballProjectile>()->bNetTemporary);
	TestTrue(TEXT("EarthTremor is bNetTemporary"), GetDefault<AWoTEarthTremorProjectile>()->bNetTemporary);
	TestTrue(TEXT("Fireworks is bNetTemporary"), GetDefault<AWoTFireworksProjectile>()->bNetTemporary);

	TestFalse(TEXT("Seeking base is not bNetTemporary"), GetDefault<AWoTSeekingProjectile>()->bNetTemporary);
	TestFalse(TEXT("Ice (seeker) is not bNetTemporary"), GetDefault<AWoTIceProjectile>()->bNetTemporary);
	TestFalse(TEXT("WallOfAir is not bNetTemporary"), GetDefault<AWoTWallOfAirProjectile>()->bNetTemporary);
	// ExpWard is a deliberate divergence: a stationary ward opts into a persistent
	// channel so a server-only early detonation propagates instead of lingering.
	TestFalse(TEXT("ExpWard is not bNetTemporary (persistent channel)"), GetDefault<AWoTExpWardProjectile>()->bNetTemporary);
	return true;
}

// The seed mechanism in isolation: given a replicated initial velocity, the helper
// puts it in the PMC and the actor flies. (Direct call — role-independent; the
// role-gated wiring is the next spec + the live harness.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProjClientSeedSpec,
	"WoT.Net.Projectile.ClientVelocitySeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProjClientSeedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDartProjectile* Proj = World->SpawnActor<AWoTDartProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	TestNotNull(TEXT("dart spawned"), Proj);
	if (!Proj) { WoTTest::DestroySpecWorld(World); return false; }

	// Mimic the inert client copy: zero the PMC velocity the server BeginPlay set.
	Proj->ProjectileMovement->Velocity = FVector::ZeroVector;

	// A distinct replicated velocity (not Speed*rotation, so the assert can't pass
	// off the authority velocity-set) — then seed from it.
	const FVector V(0.f, 1500.f, 0.f);
	FRepMovement Rep = Proj->GetReplicatedMovement();
	Rep.LinearVelocity = V;
	Proj->SetReplicatedMovement(Rep);
	Proj->ApplyReplicatedInitialVelocity();

	TestTrue(TEXT("PMC velocity seeded from replicated LinearVelocity"),
		Proj->ProjectileMovement->Velocity.Equals(V, 1.0f));
	TestEqual(TEXT("PMC InitialSpeed = |V|"),
		FMath::RoundToInt(Proj->ProjectileMovement->InitialSpeed), 1500);

	const FVector P0 = Proj->GetActorLocation();
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("seeded dart translates along +Y"),
		(Proj->GetActorLocation() - P0).Y > 10.f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The BeginPlay wiring: a bNetTemporary sim proxy runs the client seed branch (not
// the authority velocity-set, not frozen). Deferred spawn lets us force the role +
// a replicated velocity before BeginPlay.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProjBeginPlaySeedSpec,
	"WoT.Net.Projectile.ClientBeginPlaySeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProjBeginPlaySeedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FTransform TM(FRotator::ZeroRotator, FVector(0, 0, 100));
	AWoTDartProjectile* Proj = World->SpawnActorDeferred<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), TM, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	TestNotNull(TEXT("dart deferred-spawned"), Proj);
	if (!Proj) { WoTTest::DestroySpecWorld(World); return false; }

	// Force the client role + a replicated initial velocity BEFORE BeginPlay.
	Proj->SetRole(ROLE_SimulatedProxy);
	FRepMovement Rep = Proj->GetReplicatedMovement();
	Rep.LinearVelocity = FVector(0.f, 1500.f, 0.f);
	Proj->SetReplicatedMovement(Rep);
	Proj->FinishSpawning(TM);   // -> BeginPlay -> client branch

	// Facing +X: an authority BeginPlay would set (Speed,0,0)=(2000,0,0); a frozen
	// client stays (0,0,0). The client seed branch must give the replicated (0,1500,0).
	TestFalse(TEXT("client copy is not authority"), Proj->HasAuthority());
	TestTrue(TEXT("BeginPlay client branch seeded replicated velocity"),
		Proj->ProjectileMovement->Velocity.Equals(FVector(0.f, 1500.f, 0.f), 1.0f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Client-simulated impact teardown: a wall hit runs Explode->Destroy regardless of
// role (UE1 simulated HitWall, Engine.Projectile — Explode is outside the
// authority gate). Forced sim proxy so this is a genuine RED for the un-gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProjExplodeTeardownSpec,
	"WoT.Net.Projectile.ExplodeTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProjExplodeTeardownSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDartProjectile* Proj = World->SpawnActor<AWoTDartProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	TestNotNull(TEXT("dart spawned"), Proj);
	if (!Proj) { WoTTest::DestroySpecWorld(World); return false; }

	Proj->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("under test as sim proxy"), Proj->HasAuthority());

	FHitResult Hit;
	Proj->NotifyHit(nullptr, nullptr, nullptr, true,
		FVector(50, 0, 100), FVector(-1, 0, 0), FVector::ZeroVector, Hit);
	WoTTest::TickWorld(World, 2);   // let the queued Destroy resolve
	TestFalse(TEXT("client copy self-destructs on wall hit"), IsValid(Proj));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Cosmetics-local / server-damage-authoritative: a client-role touch
// runs simulated teardown but applies NO damage; the authority copy does. (The
// victim's own TakeDamage is authority-gated too, but it is always authority in a
// standalone world, so this isolates the PROJECTILE-side ProcessDamage gate — which
// is the one that also guards non-self-gating targets: movers, wards, splash.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProjDamageAuthoritySpec,
	"WoT.Net.Projectile.DamageAuthorityOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProjDamageAuthoritySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	TestNotNull(TEXT("victim spawned"), Victim);
	if (!Victim) { WoTTest::DestroySpecWorld(World); return false; }
	// A large health pool so a single Dart is non-lethal — the observable is the
	// damage DELTA, not death (Died() would pull in the un-possessed-pawn kill path).
	const int32 H0 = 1000;
	Victim->Health = H0;

	// Projectiles spawn OFF the victim and are driven in by hand: a Dart spawned
	// ON the victim would fire ProcessTouch during SpawnActor (the Ice/EarthTremor
	// begin-overlap gotcha, WoTGenericProjectile.cpp), at authority + before SetRole,
	// applying damage and Destroy()ing the copy out from under the test.
	// client-role projectile: simulated teardown, but damage must be skipped.
	AWoTDartProjectile* ClientProj = World->SpawnActor<AWoTDartProjectile>(
		FVector(-100, -100, 100), FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	TestNotNull(TEXT("client dart spawned"), ClientProj);
	if (!ClientProj) { WoTTest::DestroySpecWorld(World); return false; }
	ClientProj->SetRole(ROLE_SimulatedProxy);
	ClientProj->ProcessTouch(Victim, Victim->GetActorLocation());
	WoTTest::TickWorld(World, 2);
	TestEqual(TEXT("client-side touch applies NO damage"), Victim->Health, H0);

	// authority copy: server stays damage-authoritative.
	AWoTDartProjectile* SrvProj = World->SpawnActor<AWoTDartProjectile>(
		FVector(-100, 100, 100), FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	TestNotNull(TEXT("server dart spawned"), SrvProj);
	if (!SrvProj) { WoTTest::DestroySpecWorld(World); return false; }
	SrvProj->ProcessTouch(Victim, Victim->GetActorLocation());
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("authority touch applies damage"), Victim->Health < H0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Fireworks tears down on the client via its own FUSE, not impact: its NotifyHit
// bounces off walls (never explodes), so the impact path can't reach it. A
// sim-proxy copy must run the fuse (the SetTimer is not authority-gated) and
// self-destruct well before its 140s LifeSpan. Deferred spawn forces the sim-proxy role
// BEFORE BeginPlay so this proves the client actually schedules the fuse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProjFireworksFuseSpec,
	"WoT.Net.Projectile.FireworksClientFuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProjFireworksFuseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FTransform TM(FRotator::ZeroRotator, FVector(0, 0, 100));
	AWoTFireworksProjectile* FW = World->SpawnActorDeferred<AWoTFireworksProjectile>(
		AWoTFireworksProjectile::StaticClass(), TM, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	TestNotNull(TEXT("fireworks deferred-spawned"), FW);
	if (!FW) { WoTTest::DestroySpecWorld(World); return false; }
	FW->SetRole(ROLE_SimulatedProxy);
	FW->FinishSpawning(TM);   // BeginPlay runs as sim proxy -> fuse scheduled
	TestFalse(TEXT("under test as sim proxy"), FW->HasAuthority());

	// Tick past MaxExpTime (6s) so the client's own fuse fires -> Detonate -> Explode ->
	// Destroy (splash authority-gated, so damage-free). Use the small 1/60s step (as
	// WoTTest::TickWorld does): UWorld clamps a per-tick delta to WorldSettings
	// MaxUndilatedFrameTime (0.4s), so coarse 1.0s ticks advance timers only ~0.4s each —
	// 8 of them cover just ~3.2s and flakily miss the random 2-6s fuse. 480 * 1/60 = 8s,
	// with an early-out once the fuse has torn the copy down.
	for (int32 i = 0; i < 480 && IsValid(FW); ++i)
	{
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
	}
	TestFalse(TEXT("client fireworks self-destructs on its fuse (no 140s LifeSpan leak)"),
		IsValid(FW));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The weave display binds — every mesh weave resolves its imported mesh at
// BeginPlay at the authored DrawScale ONLY (the meshmap scale is baked into the
// geometry), loop weaves animate, lights carry the UE1 recipe.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWeaveDisplaySpec,
	"WoT.Net.Projectiles.WeaveDisplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWeaveDisplaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto Visual = [](AActor* A) -> UWoTVertexAnimComponent*
	{
		return A ? A->FindComponentByClass<UWoTVertexAnimComponent>() : nullptr;
	};
	auto MeshName = [&](AActor* A) -> FString
	{
		UWoTVertexAnimComponent* V = Visual(A);
		USkeletalMesh* M = V ? V->GetSkeletalMeshAsset() : nullptr;
		return M ? M->GetName() : FString();
	};

	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator,
		ProjAlwaysSpawnParams());
	TestEqual(TEXT("Dart binds xboxdart"), MeshName(Dart), FString(TEXT("xboxdart")));
	TestEqual(TEXT("Dart DrawScale 3.0 (authored)"),
	          Visual(Dart)->GetRelativeScale3D(), FVector(3.0f));
	if (UPointLightComponent* L = Dart->FindComponentByClass<UPointLightComponent>())
	{
		TestEqual(TEXT("Dart light radius 25x(2+1)"), L->AttenuationRadius, 75.0f);
	}

	AWoTTracerProjectile* Tracer = World->SpawnActor<AWoTTracerProjectile>(
		AWoTTracerProjectile::StaticClass(), FVector(500, 0, 100), FRotator::ZeroRotator,
		ProjAlwaysSpawnParams());
	TestEqual(TEXT("Tracer binds its eye"), MeshName(Tracer), FString(TEXT("Tracer")));
	TestTrue(TEXT("Tracer loops (LoopAnim 'Tracer' 0.1)"), Visual(Tracer)->IsAnimating());
	Tracer->Tick(0.1f);
	const float FirstTracerSpin = Tracer->GetSpinRollDegPerSec();
	Tracer->Tick(0.1f);
	TestTrue(TEXT("Tracer SpinRate accumulates into an accelerating roll"),
		FirstTracerSpin > 0.0f
			&& FMath::IsNearlyEqual(Tracer->GetSpinRollDegPerSec(),
				FirstTracerSpin * 2.0f, 0.01f));

	AWoTWallOfAirProjectile* Wall = World->SpawnActor<AWoTWallOfAirProjectile>(
		AWoTWallOfAirProjectile::StaticClass(), FVector(1000, 0, 100),
		FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	TestEqual(TEXT("WallOfAir binds"), MeshName(Wall), FString(TEXT("WallOfAir")));
	TestTrue(TEXT("WallOfAir shimmers (LoopAnim 'All')"), Visual(Wall)->IsAnimating());

	TestEqual(TEXT("ExpWard binds the rune"),
	          MeshName(World->SpawnActor<AWoTExpWardProjectile>(
			AWoTExpWardProjectile::StaticClass(), FVector(1500, 0, 100),
			FRotator::ZeroRotator, ProjAlwaysSpawnParams())),
	          FString(TEXT("ExpWard")));
	TestEqual(TEXT("AMA binds the shipped pickup mesh"),
	          MeshName(World->SpawnActor<AWoTAngrealAMAProjectile>(
			AWoTAngrealAMAProjectile::StaticClass(), FVector(2000, 0, 100),
			FRotator::ZeroRotator, ProjAlwaysSpawnParams())),
	          FString(TEXT("AngrealAntiMagicAuraPickup")));
	TestEqual(TEXT("Fireball binds the rock"),
	          MeshName(World->SpawnActor<AWoTFireballProjectile>(
			AWoTFireballProjectile::StaticClass(), FVector(2500, 0, 100),
			FRotator::ZeroRotator, ProjAlwaysSpawnParams())),
	          FString(TEXT("LavaRock")));

	// --- 6a-iii: the trail sprayer bundles. Registry rows verbatim, the Fireball
	// bundle spawns once moving past SprayerDelay, fade-on-destroy. ---
	{
		const FWoTSprayerBundleRow* FB2 = WoTWeaveFX::Find(TEXT("FireballSprayer2"));
		if (TestNotNull(TEXT("FireballSprayer2 row"), FB2))
		{
			TestEqual(TEXT("head Volume 60"), FB2->Volume, 60.0f);
			TestEqual(TEXT("head MinVolume 40 (ParticleDensity floor)"), FB2->MinVolume, 40.0f);
			TestEqual(TEXT("head templates 7"), FB2->Templates.Num(), 7);
			TestEqual(TEXT("head lives the whole flight (the exported LifeSpan=0.1 "
			               "is the struct-bug artifact PreBeginPlay zeroes)"),
			          FB2->SelfDestroySeconds, 0.0f);
		}
		TestNotNull(TEXT("TracerSprayer row"), WoTWeaveFX::Find(TEXT("TracerSprayer")));
		TestNotNull(TEXT("SnowSprayer03 row"), WoTWeaveFX::Find(TEXT("SnowSprayer03")));
		TestNotNull(TEXT("LightningSprayer02 row (consumer lands with the class)"),
		            WoTWeaveFX::Find(TEXT("LightningSprayer02")));

		AWoTParticleSprayer* Probe = WoTWeaveFX::SpawnLocal(
			*World, TEXT("FireballSprayer2"), FVector(4000, 0, 100), FRotator::ZeroRotator);
		if (TestNotNull(TEXT("SpawnLocal spawns"), Probe))
		{
			TestTrue(TEXT("materials resolved (import_flame_fx.py statics)"),
			         Probe->AddMaterials.Num() == 7 && Probe->AddMaterials[0] != nullptr
			         && Probe->ModMaterials[6] != nullptr);
			Probe->Destroy();
		}

		AWoTFireballProjectile* FB = World->SpawnActor<AWoTFireballProjectile>(
			AWoTFireballProjectile::StaticClass(), FVector(5000, 0, 300),
			FRotator::ZeroRotator, ProjAlwaysSpawnParams());
		if (TestNotNull(TEXT("fireball for the trail"), FB))
		{
			WoTTest::TickWorld(World, 30);   // past SprayerDelay 0.3 at 1/60 steps
			TestTrue(TEXT("the moving fireball grew its head/cone/smoke"),
			         IsValid(FB->Head) && IsValid(FB->Cone) && IsValid(FB->Smoke));
			AWoTParticleSprayer* TrailCone = FB->Cone;
			FB->Destroy();
			TestTrue(TEXT("destroy fades the cone (bOn off, 5s linger)"),
			         IsValid(TrailCone) && !TrailCone->bOn
			         && TrailCone->GetLifeSpan() == 5.0f);
		}

		AWoTTracerProjectile* Trc = World->SpawnActor<AWoTTracerProjectile>(
			AWoTTracerProjectile::StaticClass(), FVector(6000, 0, 300),
			FRotator::ZeroRotator, ProjAlwaysSpawnParams());
		if (TestNotNull(TEXT("tracer for the trail"), Trc))
		{
			TestTrue(TEXT("the tracer dust trail follows (VisRadius override 0)"),
			         IsValid(Trc->Trail) && Trc->Trail->VisibilityRadius == 0.0f);
		}
	}

	// --- 6a-iii part B: the impact bursts (Explosion frame series) + drip decal. ---
	{
		const FWoTExplosionRow* FX3 = WoTWeaveFX::FindExplosion(TEXT("FireballExplode3"));
		if (TestNotNull(TEXT("FireballExplode3 row"), FX3))
		{
			TestEqual(TEXT("17 frames"), FX3->FrameCount, 17);
			TestEqual(TEXT("DrawScale 2.5"), FX3->DrawScale, 2.5f);
		}
		TestNotNull(TEXT("GreenExplode row"), WoTWeaveFX::FindExplosion(TEXT("GreenExplode")));
		TestNotNull(TEXT("DecayExplosion row"),
		            WoTWeaveFX::FindExplosion(TEXT("DecayExplosion")));
		TestNotNull(TEXT("DecaySplatter decal row (the decay drip)"),
		            WoTDecalFX::Find(TEXT("DecaySplatter")));

		AWoTExplosionEffect* Burst = WoTWeaveFX::SpawnExplosion(
			*World, TEXT("FireballExplode3"), FVector(7000, 0, 100));
		if (TestNotNull(TEXT("burst spawns"), Burst))
		{
			TestEqual(TEXT("burst burns for the authored 1s"), Burst->GetLifeSpan(), 1.0f);
			TestTrue(TEXT("the burst light carries the recipe (25x(12+1))"),
			         Burst->Light && Burst->Light->AttenuationRadius == 325.0f);
			TestTrue(TEXT("play-once mode armed over LifeSpan"),
			         Burst->Sprite && Burst->Sprite->PlayOnceSeconds == 1.0f);
		}
		// The class defaults carry the burst names into every Explode.
		TestEqual(TEXT("Fireball explodes as FireballExplode3"),
		          GetDefault<AWoTFireballProjectile>()->ExplosionEffectName,
		          FName(TEXT("FireballExplode3")));
		TestEqual(TEXT("Seeker explodes as GreenExplode"),
		          GetDefault<AWoTSeekerProjectile>()->ExplosionEffectName,
		          FName(TEXT("GreenExplode")));
	}

	// --- The DT_Sprite weaves (part 2): the billboard quads bind and cycle. ---
	AWoTEarthTremorProjectile* ET = World->SpawnActor<AWoTEarthTremorProjectile>(
		AWoTEarthTremorProjectile::StaticClass(), FVector(3000, 0, 100),
		FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	UWoTSpriteBillboardComponent* ETSprite =
		ET ? ET->FindComponentByClass<UWoTSpriteBillboardComponent>() : nullptr;
	if (TestNotNull(TEXT("EarthTremor billboard"), ETSprite))
	{
		TestEqual(TEXT("EarthTremor DrawScale 0.8 (authored)"), ETSprite->DrawScale, 0.8f);
		TestTrue(TEXT("the ETL_A series resolved (import_weave_sprites.py ran)"),
		         ETSprite->HasResolvedFrames());
	}
	AWoTSoulBarbProjectile* SB = World->SpawnActor<AWoTSoulBarbProjectile>(
		AWoTSoulBarbProjectile::StaticClass(), FVector(3500, 0, 100),
		FRotator::ZeroRotator, ProjAlwaysSpawnParams());
	if (TestNotNull(TEXT("SoulBarb spawned"), SB))
	{
		TestNotNull(TEXT("SoulBarb flare quad"), SB->Flare.Get());
		TestNotNull(TEXT("SoulBarb hole quad"), SB->Hole.Get());
		TestTrue(TEXT("the hole disk is modulate-only (STY_Modulated)"),
		         SB->Hole->bModulateOnly);
	}

	// --- 6a-iv: the beams. BalefireLine sprite segments + LightningStreak02
	// mesh segments (pooled, follow-actor driven). ---
	{
		AWoTBeamEffect* Line = World->SpawnActor<AWoTBeamEffect>(
			AWoTBeamEffect::StaticClass(), FVector(8000, 0, 100), FRotator::ZeroRotator,
			ProjAlwaysSpawnParams());
		if (TestNotNull(TEXT("balefire line spawns"), Line))
		{
			Line->Config = FName(TEXT("BalefireLine"));
			Line->Start = FVector(8000, 0, 100);
			Line->End = FVector(8000, 360, 100);   // 360uu / 18 = 20 segments
			WoTTest::TickWorld(World, 2);
			int32 Quads = 0;
			for (UActorComponent* C : Line->GetComponents())
			{
				if (auto* ISM = Cast<UInstancedStaticMeshComponent>(C))
				{
					Quads = FMath::Max(Quads, ISM->GetInstanceCount());
				}
			}
			TestTrue(FString::Printf(TEXT("sparks laid every 18uu (got %d)"), Quads),
			         Quads >= 19 && Quads <= 21);
		}

		AWoTCharacter* From = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), FVector(9000, 0, 100), FRotator::ZeroRotator,
			ProjAlwaysSpawnParams());
		AWoTCharacter* To = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), FVector(9000, 640, 100), FRotator::ZeroRotator,
			ProjAlwaysSpawnParams());
		AWoTBeamEffect* Bolt = World->SpawnActor<AWoTBeamEffect>(
			AWoTBeamEffect::StaticClass(), FVector(9000, 0, 100), FRotator::ZeroRotator,
			ProjAlwaysSpawnParams());
		if (TestNotNull(TEXT("lightning bolt spawns"), Bolt) && From && To)
		{
			Bolt->Config = FName(TEXT("LightningStreak02"));
			Bolt->StartActor = From;
			Bolt->EndActor = To;
			WoTTest::TickWorld(World, 2);
			int32 Bolts = 0;
			for (UActorComponent* C : Bolt->GetComponents())
			{
				auto* Seg = Cast<UWoTVertexAnimComponent>(C);
				if (Seg && Seg->IsVisible() && Seg->GetSkeletalMeshAsset())
				{
					TestEqual(TEXT("segment mesh is LSbolt"),
					          Seg->GetSkeletalMeshAsset()->GetName(),
					          FString(TEXT("LSbolt")));
					++Bolts;
				}
			}
			TestTrue(FString::Printf(TEXT("bolt segments every 64uu (got %d)"), Bolts),
			         Bolts >= 9 && Bolts <= 11);
		}
		TestNotNull(TEXT("BalefireDecal row (the end scorch)"),
		            WoTDecalFX::Find(TEXT("BalefireDecal")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
