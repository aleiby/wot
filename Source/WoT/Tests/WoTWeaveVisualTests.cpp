// Presentation-parity automation specs.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTDecayProjectile.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTFireworksProjectile.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Projectiles/WoTSoulBarbProjectile.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "../Projectiles/WoTTaintProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTDecal.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTTeleporter.h"
#include "../WoTWeaveFX.h"
#include "../WoTAppearEffect.h"
#include "../WoTZoneVolume.h"
#include "../WoTVertexAnimComponent.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"

namespace
{
	FActorSpawnParameters AlwaysSpawn()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}

	int32 CountSprayers(UWorld* World, FName Config)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
		{
			Count += It->ConfigName == Config ? 1 : 0;
		}
		return Count;
	}

	int32 CountExplosions(UWorld* World, FName Config)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTExplosionEffect> It(World); It; ++It)
		{
			Count += It->ConfigName == Config ? 1 : 0;
		}
		return Count;
	}

	int32 CountMeshEffects(UWorld* World, FName Config)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
		{
			Count += It->ConfigName == Config ? 1 : 0;
		}
		return Count;
	}

	int32 CountBurstMarkers(UWorld* World, FName Config)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTWeaveBurst> It(World); It; ++It)
		{
			Count += It->Config == Config ? 1 : 0;
		}
		return Count;
	}

	AWoTParticleSprayer* FindSprayerAt(UWorld* World, FName Config,
		const FVector& Location)
	{
		for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
		{
			if (It->ConfigName == Config && It->GetActorLocation().Equals(Location, 1.0f))
			{
				return *It;
			}
		}
		return nullptr;
	}

	AWoTExplosionEffect* FindExplosion(UWorld* World, FName Config)
	{
		for (TActorIterator<AWoTExplosionEffect> It(World); It; ++It)
		{
			if (It->ConfigName == Config) { return *It; }
		}
		return nullptr;
	}

	AWoTMeshEffect* FindMeshEffect(UWorld* World, FName Config)
	{
		for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
		{
			if (It->ConfigName == Config) { return *It; }
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNewFXRowsSpec,
	"WoT.WeaveFX.NewBundleRowsResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNewFXRowsSpec::RunTest(const FString&)
{
	const FName BundleNames[] = {
		TEXT("SoulBarbSmoke"), TEXT("BlastBits"), TEXT("AppearSprayerWhite"),
		TEXT("WOTSparks"), TEXT("Firework01"), TEXT("Firework02"),
		TEXT("Firework03"), TEXT("Firework04"), TEXT("Firework05"),
		TEXT("Firework06"), TEXT("Firework07"), TEXT("Firework08"),
		TEXT("Firework09"), TEXT("Firework01NL"), TEXT("Firework02NL"),
		TEXT("Firework03NL"), TEXT("Firework04NL"), TEXT("Firework05NL"),
		TEXT("Firework06NL"), TEXT("Firework07NL"), TEXT("Firework08NL"),
		TEXT("Firework09NL")};
	for (FName Name : BundleNames)
	{
		const FWoTSprayerBundleRow* Row = WoTWeaveFX::Find(Name);
		if (TestNotNull(*FString::Printf(TEXT("%s bundle row"), *Name.ToString()), Row))
		{
			TestTrue(*FString::Printf(TEXT("%s has particle templates"), *Name.ToString()),
				Row->Templates.Num() > 0);
		}
	}
	const FWoTSprayerBundleRow* Blast = WoTWeaveFX::Find(TEXT("BlastBits"));
	if (TestNotNull(TEXT("BlastBits row"), Blast))
	{
		for (const FWoTSprayerTemplateRow& T : Blast->Templates)
		{
			TestTrue(TEXT("BlastBits WetTextures use FB: material slugs"),
				!T.MaterialSlug.IsNone()
				&& T.MaterialSlug.ToString().StartsWith(TEXT("FB:")));
		}
	}
	TestNotNull(TEXT("FireballFizzle explosion row"),
		WoTWeaveFX::FindExplosion(TEXT("FireballFizzle")));
	for (FName Name : {FName(TEXT("SoulBarbSkull")), FName(TEXT("SoulBarbImpact")),
		FName(TEXT("TaintExpDarts")), FName(TEXT("TaintExpTorus")),
		FName(TEXT("TaintExpGeoA")), FName(TEXT("TaintExpGeoB"))})
	{
		TestNotNull(*FString::Printf(TEXT("%s mesh-effect row"), *Name.ToString()),
			WoTMeshFX::Find(Name));
	}
	TestNotNull(TEXT("SoulBarbEffect sprite row"),
		WoTMeshFX::FindSprite(TEXT("SoulBarbEffect")));
	TestNotNull(TEXT("SeekerGlobe sprite row"),
		WoTMeshFX::FindSprite(TEXT("SeekerGlobe")));

	// Firework01..09.uc: one mechanically-resolved template sample per row,
	// including the six former filler/default failures and both visibility families.
	struct FFireworkExpected
	{
		FName Name; float Visibility; int32 TemplateIndex; float Weight;
		float MinDrawScale; float MaxDrawScale; uint8 FadePhase;
	};
	const FFireworkExpected Expected[] = {
		{TEXT("Firework01"),8000,3,10,.25f,.5f,1},
		{TEXT("Firework02"),8000,0,8,.7f,1,1},
		{TEXT("Firework03"),8000,2,1,0,0,5},
		{TEXT("Firework04"),8000,2,1,0,0,5},
		{TEXT("Firework05"),0,2,1,0,0,5},
		{TEXT("Firework06"),8000,3,10,.25f,.5f,1},
		{TEXT("Firework07"),0,1,1,.01f,.1f,2},
		{TEXT("Firework08"),0,0,1,0,0,40},
		{TEXT("Firework09"),0,2,1,0,0,5},
	};
	for (const FFireworkExpected& E : Expected)
	{
		const FWoTSprayerBundleRow* Row = WoTWeaveFX::Find(E.Name);
		if (!TestNotNull(*FString::Printf(TEXT("%s resolved row"), *E.Name.ToString()), Row)
			|| !TestTrue(*FString::Printf(TEXT("%s sampled template exists"), *E.Name.ToString()),
				Row->Templates.IsValidIndex(E.TemplateIndex)))
		{
			continue;
		}
		const FWoTParticleTemplate& T = Row->Templates[E.TemplateIndex].Template;
		TestEqual(*FString::Printf(TEXT("%s visibility radius"), *E.Name.ToString()), Row->VisibilityRadius, E.Visibility);
		TestEqual(*FString::Printf(TEXT("%s visibility height"), *E.Name.ToString()), Row->VisibilityHeight, E.Visibility);
		TestEqual(*FString::Printf(TEXT("%s template weight"), *E.Name.ToString()), T.Weight, E.Weight);
		TestEqual(*FString::Printf(TEXT("%s template min scale"), *E.Name.ToString()), T.MinDrawScale, E.MinDrawScale);
		TestEqual(*FString::Printf(TEXT("%s template max scale"), *E.Name.ToString()), T.MaxDrawScale, E.MaxDrawScale);
		TestEqual(*FString::Printf(TEXT("%s template fade phase"), *E.Name.ToString()), T.FadePhase, E.FadePhase);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecayVisualSpec,
	"WoT.Decay.DripAndFollowExplosion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDecayVisualSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDecayProjectile* Decay = World->SpawnActor<AWoTDecayProjectile>(
		FVector(0, 0, 300), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("Decay projectile"), Decay))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 125);
	TestEqual(TEXT("flight timer emitted DecaySplatter"), Decay->LastDripConfig,
		FName(TEXT("DecaySplatter")));
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("Decay victim"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Exercise the production projectile wiring, not the registry helper.
	Decay->SetDestination(Victim);
	Decay->Explode(Victim->GetActorLocation(), FVector::UpVector);
	AWoTExplosionEffect* Explosion = FindExplosion(World, TEXT("DecayExplosion"));
	if (TestNotNull(TEXT("DecayExplosion spawned"), Explosion))
	{
		Victim->SetActorLocation(FVector(700, 100, 125));
		WoTTest::TickWorld(World, 1);
		TestEqual(TEXT("DecayExplosion follows its victim"),
			Explosion->GetActorLocation(), Victim->GetActorLocation());
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoulBarbVisualSpec,
	"WoT.SoulBarb.TrailAndImpact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSoulBarbVisualSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("SoulBarb victim"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTSoulBarbProjectile* Soul = World->SpawnActor<AWoTSoulBarbProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("SoulBarb projectile"), Soul))
	{
		TestTrue(TEXT("SoulBarbSmoke spawned as the flight trail"),
			IsValid(Soul->SmokeTrail));
		TestTrue(TEXT("SoulBarbSmoke aligns authored gravity to rotation"),
			Soul->SmokeTrail && Soul->SmokeTrail->bAlignGravityToRotation);
		Soul->SetDestination(Victim);
		Soul->Explode(Victim->GetActorLocation(), FVector::UpVector);
		TestEqual(TEXT("production Explode spawned SoulBarbImpact"),
			CountMeshEffects(World, TEXT("SoulBarbImpact")), 1);
	}
	AWoTSoulBarbProjectile* WallSoul = World->SpawnActor<AWoTSoulBarbProjectile>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("wall-hit SoulBarb projectile"), WallSoul))
	{
		WallSoul->Explode(WallSoul->GetActorLocation(), FVector::UpVector);
		TestEqual(TEXT("invalid destination does not over-spawn SoulBarbImpact"),
			CountMeshEffects(World, TEXT("SoulBarbImpact")), 1);
	}
	// These two shipped classes have no retail producer; rows still resolve so a
	// later verified producer can bind without new machinery.
	TestNotNull(TEXT("unused shipped SoulBarbSkull row resolves"),
		WoTMeshFX::Find(TEXT("SoulBarbSkull")));
	TestNotNull(TEXT("unused shipped SoulBarbEffect row resolves"),
		WoTMeshFX::FindSprite(TEXT("SoulBarbEffect")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBlastBitsSpec,
	"WoT.ExpWard.BlastBits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTBlastBitsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("ExpWard"), Ward))
	{
		Ward->RelativeUp = FVector::RightVector;
		const FVector WardAt = Ward->GetActorLocation();
		Ward->Explode(WardAt, FVector::ForwardVector);
		TestEqual(TEXT("ExpWard Explode spawned BlastBits"),
			CountSprayers(World, TEXT("BlastBits")), 1);
		TestEqual(TEXT("ExpWard kept FireballExplode3 beside BlastBits"),
			CountExplosions(World, TEXT("FireballExplode3")), 1);
		if (AWoTParticleSprayer* Bits = FindSprayerAt(World, TEXT("BlastBits"), WardAt))
		{
			TestTrue(TEXT("ExpWard BlastBits uses authored RelativeUp orientation"),
				Bits->GetActorRotation().Equals(FVector::RightVector.Rotation(), .01f));
		}
	}
	AWoTFireballProjectile* Fireball = World->SpawnActor<AWoTFireballProjectile>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("Fireball"), Fireball))
	{
		const FVector FireballAt = Fireball->GetActorLocation();
		const FVector HitNormal = FVector::RightVector;
		Fireball->Explode(FireballAt, HitNormal);
		TestEqual(TEXT("shipped Fireball producer also spawns BlastBits"),
			CountSprayers(World, TEXT("BlastBits")), 2);
		if (AWoTParticleSprayer* Bits = FindSprayerAt(World, TEXT("BlastBits"), FireballAt))
		{
			TestTrue(TEXT("Fireball BlastBits uses HitNormal orientation"),
				Bits->GetActorRotation().Equals(HitNormal.Rotation(), .01f));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFireworksVisualSpec,
	"WoT.Fireworks.SetsAndSounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFireworksVisualSpec::RunTest(const FString&)
{
	const TArray<FWoTFireworkSet>& Sets = AWoTFireworksProjectile::GetFireworkSets();
	TestEqual(TEXT("five retail firework sets"), Sets.Num(), 5);
	const FName ExpectedProj[] = {TEXT("Firework01"), TEXT("Firework06"),
		TEXT("Firework03"), TEXT("Firework04"), TEXT("Firework02")};
	const FName ExpectedExp[] = {TEXT("Firework05"), TEXT("Firework05"),
		TEXT("Firework09"), TEXT("Firework08"), TEXT("Firework07")};
	for (int32 i = 0; i < Sets.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("set %d projectile"), i),
			Sets[i].ProjectileConfig, ExpectedProj[i]);
		TestEqual(*FString::Printf(TEXT("set %d explosion"), i),
			Sets[i].ExplosionConfig, ExpectedExp[i]);
		TestEqual(*FString::Printf(TEXT("set %d launch sound"), i),
			Sets[i].LaunchSoundPath, FName(TEXT("Angreal/Launch1FW")));
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTFireworksProjectile* Fireworks = World->SpawnActor<AWoTFireworksProjectile>(
		FVector(0, 0, 300), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("Fireworks projectile"), Fireworks))
	{
		TestTrue(TEXT("random selected set is in range"),
			Fireworks->SelectedFirework >= 0 && Fireworks->SelectedFirework < 5);
		const FWoTFireworkSet Set = Sets[Fireworks->SelectedFirework];
		TestTrue(TEXT("selected rising trail follows projectile"),
			Fireworks->Trail && Fireworks->Trail->ConfigName == Set.ProjectileConfig
			&& Fireworks->Trail->FollowActor == Fireworks);
		TestEqual(TEXT("selected launch sound observed"),
			Fireworks->LastLaunchSound, Set.LaunchSoundPath);
		AWoTParticleSprayer* Trail = Fireworks->Trail;
		Fireworks->Explode(Fireworks->GetActorLocation(), FVector::UpVector);
		TestTrue(TEXT("rising trail retires for five seconds"),
			Trail && !Trail->bOn && Trail->GetLifeSpan() == 5.0f);
		TestTrue(TEXT("rising trail light is killed immediately at explode"),
			Trail && Trail->Light && !Trail->Light->IsVisible());
		TestEqual(TEXT("selected explosion bundle spawned"),
			CountSprayers(World, Set.ExplosionConfig), 1);
	}
	AWoTParticleSprayer* Fade05 = WoTWeaveFX::SpawnLocal(*World, TEXT("Firework05"),
		FVector(2000, 0, 300), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("Firework05 light-fade fixture"), Fade05))
	{
		const float Full = Fade05->Light->Intensity;
		WoTTest::TickWorld(World, 30);
		TestTrue(TEXT("Firework05 explosion light fades continuously"),
			Fade05->Light->Intensity > 0.0f && Fade05->Light->Intensity < Full);
	}
	AWoTParticleSprayer* Stage07 = WoTWeaveFX::SpawnLocal(*World, TEXT("Firework07"),
		FVector(3000, 0, 300), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("Firework07 staged-light fixture"), Stage07))
	{
		WoTTest::TickWorld(World, 66); // stage 1: light fades out, rotation paused
		const FRotator Paused = Stage07->GetActorRotation();
		WoTTest::TickWorld(World, 20);
		TestTrue(TEXT("Firework07 stage 1 pauses rotation"),
			Stage07->GetActorRotation().Equals(Paused, .01f));
		WoTTest::TickWorld(World, 40); // stage 2: light fades in and rotation resumes
		TestFalse(TEXT("Firework07 stage 2 resumes rotation"),
			Stage07->GetActorRotation().Equals(Paused, .01f));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesVisualSpec,
	"WoT.SwapPlaces.TrailAndFlash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwapPlacesVisualSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Source = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	AWoTCharacter* Dest = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("SwapPlaces source"), Source)
		|| !TestNotNull(TEXT("SwapPlaces destination"), Dest))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTSwapPlacesProjectile* Swap = World->SpawnActor<AWoTSwapPlacesProjectile>(
		FVector(500, 0, 300), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("SwapPlaces projectile"), Swap))
	{
		Swap->InitiatingPawn = Source;
		Swap->SetDestination(Dest);
		WoTTest::TickWorld(World, 1);
		TestTrue(TEXT("Firework04 follow trail spawned"),
			Swap->Trail && Swap->Trail->ConfigName == FName(TEXT("Firework04")));
		TestTrue(TEXT("Swap trail light override is radius 6"),
			Swap->Trail && Swap->Trail->Light
			&& Swap->Trail->Light->AttenuationRadius == 175.0f);
		AWoTParticleSprayer* SwapTrail = Swap->Trail;
		Swap->Explode(Dest->GetActorLocation(), FVector::UpVector);
		TestTrue(TEXT("Swap trail light is killed immediately at explode"),
			SwapTrail && SwapTrail->Light && !SwapTrail->Light->IsVisible());
		TestEqual(TEXT("production swap path flashed both endpoints"),
			Swap->LastSwapFlashCount, 2);
		// SwapPlacesEffect.uc:408-434: both endpoints get an AppearEffect on
		// the PAWN — neither fixture pawn is a player, so the retail non-player arm
		// colors them Blue (source -> CyanCorona) and Green (dest -> GreenCorona).
		int32 SourceEffects = 0, DestEffects = 0;
		for (TActorIterator<AWoTAppearEffect> It(World); It; ++It)
		{
			if (It->AppearTarget == Source
				&& It->TopSprite == FName(TEXT("CyanCorona"))
				&& It->BottomSprite == FName(TEXT("CyanCorona")))
			{
				++SourceEffects;
			}
			if (It->AppearTarget == Dest
				&& It->TopSprite == FName(TEXT("GreenCorona"))
				&& It->BottomSprite == FName(TEXT("GreenCorona")))
			{
				++DestEffects;
			}
		}
		TestEqual(TEXT("source endpoint wears the retail Blue appear rings"),
			SourceEffects, 1);
		TestEqual(TEXT("dest endpoint wears the retail Green appear rings"),
			DestEffects, 1);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTaintVisualSpec,
	"WoT.Taint.AuraAndBurst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTaintVisualSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("Taint victim"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTTaintProjectile* Taint = World->SpawnActor<AWoTTaintProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("Taint projectile"), Taint))
	{
		TestTrue(TEXT("five-part TaintVisual aura spawned and pinned"),
			Taint->Aura && Taint->Aura->FollowActor == Taint
			&& Taint->Aura->GetPartCount() == 5);
		Taint->SetDestination(Victim);
		Taint->Explode(Victim->GetActorLocation(), FVector::UpVector);
		for (FName Name : {FName(TEXT("TaintExpDarts")), FName(TEXT("TaintExpTorus")),
			FName(TEXT("TaintExpGeoA")), FName(TEXT("TaintExpGeoB"))})
		{
			TestEqual(*FString::Printf(TEXT("%s spawned at Explode"), *Name.ToString()),
				CountMeshEffects(World, Name), 1);
		}
		WoTTest::TickWorld(World, 30);
		AWoTMeshEffect* Darts = FindMeshEffect(World, TEXT("TaintExpDarts"));
		AWoTMeshEffect* Torus = FindMeshEffect(World, TEXT("TaintExpTorus"));
		// TaintExpAssets.uc:38-39 fades ScaleGlow (and the light), never
		// AmbientGlow: the unlit darts keep their AmbientGlow 20 term.
		if (TestNotNull(TEXT("Taint mesh-light fade fixture"), Darts))
		{
			FLinearColor Ambient = FLinearColor::White;
			UMaterialInterface* Bound = Darts->Mesh->GetMaterial(0);
			TestTrue(TEXT("bound mesh-light MID exposes vector MLAmbient"), Bound
				&& Bound->GetVectorParameterValue(
					FHashedMaterialParameterInfo(TEXT("MLAmbient")), Ambient));
			TestTrue(TEXT("the darts' ScaleGlow has faded"),
				Darts->ScaleGlow > 0.0f && Darts->ScaleGlow < 1.0f);
			TestEqual(TEXT("the darts keep their AmbientGlow"), Darts->AmbientGlow, 20.0f);
			TestTrue(TEXT("bound mesh-light MID renders the faded GUnlitColor"),
				FMath::IsNearlyEqual(Ambient.R, FMath::Clamp(
					0.5f * Darts->ScaleGlow + 20.0f / 256.0f, 0.0f, 1.0f), 1.0e-4f));
		}
		// The torus is a lit translucent mesh with a procedural fire skin: it
		// fades its lights' Diffuse (ScaleGlow) on the lit translucent master.
		if (TestNotNull(TEXT("Taint lit fade fixture"), Torus))
		{
			float SG = -1.0f;
			UMaterialInterface* Bound = Torus->Mesh->GetMaterial(0);
			TestTrue(TEXT("bound lit MID exposes scalar MLScaleGlow"), Bound
				&& Bound->GetScalarParameterValue(
					FHashedMaterialParameterInfo(TEXT("MLScaleGlow")), SG));
			TestTrue(TEXT("the torus's ScaleGlow has faded"),
				Torus->ScaleGlow > 0.0f && Torus->ScaleGlow < 1.0f);
			TestTrue(TEXT("bound lit MID received the faded ScaleGlow"),
				FMath::IsNearlyEqual(SG, Torus->ScaleGlow, 1.0e-4f));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSeekerGlobeSpec,
	"WoT.Seeker.GlobeChild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSeekerGlobeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTSeekerProjectile* Seeker = World->SpawnActor<AWoTSeekerProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("Seeker"), Seeker))
	{
		AWoTWeaveSpriteEffect* Globe = Seeker->Globe;
		TestTrue(TEXT("SeekerGlobe child is based to the seeker"),
			Globe && Globe->FollowActor == Seeker);
		Seeker->Destroy();
		WoTTest::TickWorld(World, 2);
		TestFalse(TEXT("SeekerGlobe dies with the projectile"), IsValid(Globe));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterFizzleSpec,
	"WoT.Projectile.WaterFizzle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterFizzleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		FVector(500, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("production water zone"), Water))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Water->bWaterZone = true;
	Water->Collision->SetBoxExtent(FVector(120, 120, 120));
	Water->SetRole(ROLE_SimulatedProxy);

	// A real client bNetTemporary copy: role + replicated velocity are installed
	// before BeginPlay, then the projectile flies into the production zone poll.
	const FTransform TM(FRotator::ZeroRotator, FVector(0, 0, 100));
	AWoTFireballProjectile* Fireball =
		World->SpawnActorDeferred<AWoTFireballProjectile>(
			AWoTFireballProjectile::StaticClass(), TM, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("client fireball fixture"), Fireball))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Fireball->SetRole(ROLE_SimulatedProxy);
	FRepMovement Rep = Fireball->GetReplicatedMovement();
	Rep.LinearVelocity = FVector(1000, 0, 0);
	Fireball->SetReplicatedMovement(Rep);
	Fireball->FinishSpawning(TM);
	TestFalse(TEXT("water-fizzle projectile is not authority"), Fireball->HasAuthority());
	TestTrue(TEXT("water-fizzle projectile is the bNetTemporary retail copy"),
		Fireball->bNetTemporary);
	WoTTest::TickWorld(World, 60);
	TestEqual(TEXT("production zone path spawned client-local FireballFizzle"),
		CountExplosions(World, TEXT("FireballFizzle")), 1);
	TestFalse(TEXT("client bNetTemporary copy tore itself down locally"),
		IsValid(Fireball));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTeleporterEffectsSpec,
	"WoT.Teleporter.EffectsFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTeleporterEffectsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTeleporter* Source = World->SpawnActor<AWoTTeleporter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	AWoTTeleporter* Dest = World->SpawnActor<AWoTTeleporter>(
		FVector(1000, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("teleporter source fixture"), Source)
		|| !TestNotNull(TEXT("teleporter destination fixture"), Dest))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Source->URL = TEXT("Exit");
	Dest->TeleTag = TEXT("Exit");
	Source->EnterEffectConfig = TEXT("WOTSparks");
	Source->EnterEffectSoundPath = TEXT("AmbientA/Portal5");
	Dest->ExitEffectConfig = TEXT("WOTSparks");
	Dest->ExitEffectSoundPath = TEXT("AmbientA/Portal5");
	// Spawn OUTSIDE both volumes: spawning inside the source fires a real
	// begin-overlap teleport, and the manual call below would then be a second
	// traversal (4 markers for 2 trips — correct behavior, wrong fixture).
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("teleporter pawn fixture"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// The one traversal is delivered from inside the source's UE1 cylinder
	// (the touch filter rejects a pawn standing 5000 units away).
	WoTTest::PlaceWithoutOverlapEvents(Pawn, Source->GetActorLocation());
	Source->NotifyActorBeginOverlap(Pawn);
	TestEqual(TEXT("departure stamped configured enter effect"),
		Source->LastEnterEffectConfig, FName(TEXT("WOTSparks")));
	TestEqual(TEXT("departure stamped configured enter sound"),
		Source->LastEnterEffectSound, FName(TEXT("AmbientA/Portal5")));
	TestEqual(TEXT("arrival stamped configured exit effect"),
		Dest->LastExitEffectConfig, FName(TEXT("WOTSparks")));
	TestEqual(TEXT("arrival stamped configured exit sound"),
		Dest->LastExitEffectSound, FName(TEXT("AmbientA/Portal5")));
	TestEqual(TEXT("enter and exit WOTSparks markers spawned"),
		CountBurstMarkers(World, TEXT("WOTSparks")), 2);

	// Four retail instances author sounds without effect classes: preserve that
	// independent axis instead of treating an absent class as an absent phase.
	Dest->ExitEffectConfig = NAME_None;
	AWoTCharacter* SoundOnlyPawn = World->SpawnActor<AWoTCharacter>(
		FVector(2000, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("sound-only teleporter pawn fixture"), SoundOnlyPawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("sound-only destination still accepts"), Dest->Accept(SoundOnlyPawn));
	TestTrue(TEXT("sound-only destination did not synthesize an effect"),
		Dest->LastExitEffectConfig.IsNone());
	TestEqual(TEXT("sound-only destination retained its authored sound"),
		Dest->LastExitEffectSound, FName(TEXT("AmbientA/Portal5")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAppearEffectProducerSpec,
	"WoT.AppearEffect.ProducerLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAppearEffectProducerSpec::RunTest(const FString&)
{
	// The corona bundle rows (AppearSprayer.uc template x the corona palette).
	for (const TCHAR* Corona : {TEXT("CyanCorona"), TEXT("PurpleCorona"),
		TEXT("AOrangeCorona"), TEXT("ABlueCorona"), TEXT("APinkCorona"),
		TEXT("APurpleCorona"), TEXT("RedCorona"), TEXT("GreenCorona"),
		TEXT("YellowCorona"), TEXT("ZGreenCorona"), TEXT("AYellowCorona"),
		TEXT("AWhiteCorona")})
	{
		TestNotNull(*FString::Printf(TEXT("AppearSprayer%s row"), Corona),
			WoTWeaveFX::Find(FName(*FString::Printf(
				TEXT("AppearSprayer%s"), Corona))));
	}

	// SetTopColor/SetBottomColor (AppearEffect.uc:77-142) + the port's plain
	// Yellow alias (the color seam hands out "Yellow" for PC_Gold).
	TestEqual(TEXT("PC_Red maps RedCorona"),
		AWoTAppearEffect::CoronaForColor(TEXT("PC_Red")), FName(TEXT("RedCorona")));
	TestEqual(TEXT("Green maps GreenCorona"),
		AWoTAppearEffect::CoronaForColor(TEXT("Green")), FName(TEXT("GreenCorona")));
	TestEqual(TEXT("PC_Purple maps PurpleCorona"),
		AWoTAppearEffect::CoronaForColor(TEXT("PC_Purple")),
		FName(TEXT("PurpleCorona")));
	TestEqual(TEXT("Gold maps YellowCorona"),
		AWoTAppearEffect::CoronaForColor(TEXT("Gold")), FName(TEXT("YellowCorona")));
	TestEqual(TEXT("Yellow maps YellowCorona (seam alias)"),
		AWoTAppearEffect::CoronaForColor(TEXT("Yellow")), FName(TEXT("YellowCorona")));
	TestEqual(TEXT("PC_Blue maps CyanCorona"),
		AWoTAppearEffect::CoronaForColor(TEXT("PC_Blue")), FName(TEXT("CyanCorona")));
	TestEqual(TEXT("unknown maps the retail default CyanCorona"),
		AWoTAppearEffect::CoronaForColor(TEXT("Chartreuse")),
		FName(TEXT("CyanCorona")));
	FName Top, Bottom;
	AWoTAppearEffect::ResolveColorPair(NAME_None, NAME_None, Top, Bottom);
	TestTrue(TEXT("SetColors() defaults Blue/Blue"),
		Top == FName(TEXT("Blue")) && Bottom == FName(TEXT("Blue")));
	AWoTAppearEffect::ResolveColorPair(TEXT("Red"), NAME_None, Top, Bottom);
	TestTrue(TEXT("one-arg SetColors colors both sprites"),
		Top == FName(TEXT("Red")) && Bottom == FName(TEXT("Red")));

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("appear target"), Target))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// SetAppearActor rejects a hidden target (uc:253-260).
	Target->SetActorHiddenInGame(true);
	TestNull(TEXT("a hidden target spawns no effect"),
		AWoTAppearEffect::SpawnWithSprites(*World, Target,
			TEXT("CyanCorona"), TEXT("PurpleCorona")));
	Target->SetActorHiddenInGame(false);
	// AppearEffect.uc:149-176 destroys on a hidden target even before Initialize
	// has produced its first rings; only a client's unresolved Target waits.
	AWoTAppearEffect* HiddenBeforeFirstTick =
		AWoTAppearEffect::SpawnWithSprites(*World, Target,
			TEXT("CyanCorona"), TEXT("PurpleCorona"));
	Target->SetActorHiddenInGame(true);
	WoTTest::TickWorld(World, 1);
	TestFalse(TEXT("hidden-before-first-tick target destroys the effect"),
		IsValid(HiddenBeforeFirstTick));
	Target->SetActorHiddenInGame(false);

	AWoTAppearEffect* Effect = AWoTAppearEffect::SpawnWithSprites(*World, Target,
		TEXT("CyanCorona"), TEXT("PurpleCorona"));
	if (!TestNotNull(TEXT("the effect spawns for a visible target"), Effect))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("the top ring sprayer spawned"),
		CountSprayers(World, TEXT("AppearSprayerCyanCorona")), 1);
	TestEqual(TEXT("the bottom ring sprayer spawned"),
		CountSprayers(World, TEXT("AppearSprayerPurpleCorona")), 1);
	const float HalfHeight = Target->GetSimpleCollisionHalfHeight();
	const float Radius = Target->GetSimpleCollisionRadius();
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (It->ConfigName == FName(TEXT("AppearSprayerCyanCorona")))
		{
			// uc:198-228: the ring orbits at the collision radius, starting
			// +CollisionHeight above the target (one tick of convergence).
			const FVector Delta =
				It->GetActorLocation() - Target->GetActorLocation();
			TestTrue(TEXT("top ring starts a half-height above the target"),
				FMath::Abs(Delta.Z - HalfHeight) < 5.0f);
			TestTrue(TEXT("top ring orbits at the collision radius"),
				FMath::Abs(FVector(Delta.X, Delta.Y, 0).Size() - Radius) < 1.0f);
		}
	}
	// Past the converge phase (1.5s): TurnOffParticles — bOn drops, linger runs.
	WoTTest::TickWorld(World, 95);
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (It->ConfigName == FName(TEXT("AppearSprayerCyanCorona"))
			|| It->ConfigName == FName(TEXT("AppearSprayerPurpleCorona")))
		{
			TestFalse(TEXT("rings stop emitting after the converge phase"), It->bOn);
		}
	}
	// Past LifeSpan + ExtraFadeTime (2.5s): the effect actor is gone.
	WoTTest::TickWorld(World, 70);
	TestFalse(TEXT("the effect self-destroys after the fade phase"),
		IsValid(Effect));

	// A destroyed target tears a live effect down (uc:174-177).
	AWoTCharacter* Doomed = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	AWoTAppearEffect* Orphan = AWoTAppearEffect::SpawnWithSprites(*World, Doomed,
		TEXT("RedCorona"), TEXT("RedCorona"));
	if (Doomed && Orphan)
	{
		WoTTest::TickWorld(World, 1);
		Doomed->Destroy();
		WoTTest::TickWorld(World, 2);
		TestFalse(TEXT("a dead target destroys the effect"), IsValid(Orphan));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAppearEffectPickupSpec,
	"WoT.AppearEffect.PickupRings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAppearEffectPickupSpec::RunTest(const FString&)
{
	// AngrealInventory.BecomePickup/BecomeItem (uc:632-671): rarity rings around
	// visible pickups, torn down on take, regrown on respawn. The headless world
	// is NM_Standalone, so the SP flag drives the same gate the MP arm uses
	// (bMPAppearEffect=True is the shipped base default the netmode branch reads).
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvDart* Dart = World->SpawnActor<AWoTAngrealInvDart>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("dart pickup"), Dart))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	auto CountAppearOn = [World](const AActor* On)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTAppearEffect> It(World); It; ++It)
		{
			Count += It->AppearTarget == On ? 1 : 0;
		}
		return Count;
	};
	// Standalone + bSPAppearEffect=false (base default): no rings.
	TestEqual(TEXT("the SP flag gates the standalone arm off"),
		CountAppearOn(Dart), 0);

	Dart->bSPAppearEffect = true;
	Dart->GotoItemState(EWoTInventoryState::Pickup);
	TestEqual(TEXT("an enabled visible pickup grows one effect"),
		CountAppearOn(Dart), 1);
	for (TActorIterator<AWoTAppearEffect> It(World); It; ++It)
	{
		if (It->AppearTarget == Dart)
		{
			// The Dart is bCommon: CyanCorona top / PurpleCorona bottom
			// (uc:1332-1333).
			TestEqual(TEXT("common rarity top sprite"),
				It->TopSprite, FName(TEXT("CyanCorona")));
			TestEqual(TEXT("common rarity bottom sprite"),
				It->BottomSprite, FName(TEXT("PurpleCorona")));
		}
	}
	// Re-entering Pickup with a LIVE effect reuses it (uc:639 AE == None gate).
	Dart->GotoItemState(EWoTInventoryState::Pickup);
	TestEqual(TEXT("a live effect is not doubled"), CountAppearOn(Dart), 1);

	// Taking the item destroys the rings (uc:667-671).
	Dart->GotoItemState(EWoTInventoryState::Held);
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("BecomeItem destroys the rings"), CountAppearOn(Dart), 0);

	// The hidden respawn wait spawns nothing; the respawn regrows the rings.
	Dart->GotoItemState(EWoTInventoryState::Sleeping);
	TestEqual(TEXT("a sleeping (hidden) pickup grows no rings"),
		CountAppearOn(Dart), 0);
	Dart->GotoItemState(EWoTInventoryState::Pickup);
	TestEqual(TEXT("the pool respawn regrows the rings"), CountAppearOn(Dart), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
