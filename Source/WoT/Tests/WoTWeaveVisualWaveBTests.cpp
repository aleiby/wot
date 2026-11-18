// Presentation-parity regression specs.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTIgnoreElementReflector.h"
#include "../Inventory/WoTForkRTSReflector.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvIce.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTIceProjectile.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../WoTBalefireFX.h"
#include "../WoTBeamEffect.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTDecoration.h"
#include "../WoTFactionCharacters.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTWeaveFX.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTWall.h"

#include "Components/BoxComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInterface.h"

namespace
{
	template <typename T> int32 CountActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<T> It(World); It; ++It) { ++Count; }
		return Count;
	}

	AWoTBalefireRemovalMarker* FindRemovalFor(UWorld* World, const AActor* Target)
	{
		for (TActorIterator<AWoTBalefireRemovalMarker> It(World); It; ++It)
		{
			if (It->Target == Target) { return *It; }
		}
		return nullptr;
	}

	bool HasFullRemovalPresentation(AActor* Target)
	{
		if (!IsValid(Target) || Target->GetActorEnableCollision()) { return false; }
		TArray<UPrimitiveComponent*> Primitives;
		Target->GetComponents<UPrimitiveComponent>(Primitives);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			if (IsValid(Primitive)
				&& Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			{
				return false;
			}
		}
		TArray<UMeshComponent*> Meshes;
		Target->GetComponents<UMeshComponent>(Meshes);
		for (const UMeshComponent* Mesh : Meshes)
		{
			if (!IsValid(Mesh)) { continue; }
			if (Mesh->IsComponentTickEnabled()) { return false; }
			if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Mesh);
				Skeletal && (!Skeletal->bPauseAnims
					|| !FMath::IsNearlyZero(Skeletal->GlobalAnimRateScale)))
			{
				return false;
			}
			for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
			{
				const UMaterialInterface* Material = Mesh->GetMaterial(Slot);
				// The per-channel mesh master is BLEND_Translucent; accept
				// the whole translucent family, not one enum value.
				if (Material && !IsTranslucentBlendMode(Material->GetBlendMode()))
				{
					return false;
				}
			}
		}
		return true;
	}
}

#define WAVE_B_TEST(ClassName, Path) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

WAVE_B_TEST(FWoTAirBurstSprayerBlowerSpec, "WoT.AirBurst.SprayerBlower")
bool FWoTAirBurstSprayerBlowerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTParticleSprayer* Sprayer = World->SpawnActor<AWoTParticleSprayer>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	const FVector Before = Sprayer->Gravity;
	AWoTSprayerBlower* Blower = WoTWeaveFX::BlowSprayers(*World,
		FVector::ZeroVector, FRotator::ZeroRotator, 250, 1, 60, 60);
	TestNotNull(TEXT("production blower spawned"), Blower);
	TestEqual(TEXT("wind added immediately"), Sprayer->Gravity, Before + FVector(250, 0, 0));
	WoTTest::TickWorld(World, 65);
	TestTrue(TEXT("wind subtracts back to authored gravity"), Sprayer->Gravity.Equals(Before, 0.1f));
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTIceFrozenPresentationAndGateSpec,
	"WoT.Ice.FrozenPresentationAndGate")
bool FWoTIceFrozenPresentationAndGateSpec::RunTest(const FString&)
{
	const AWoTIceProjectile* Ice = GetDefault<AWoTIceProjectile>();
	TestEqual(TEXT("retail S trail rate"), Ice->SRate, 5.0f);
	TestEqual(TEXT("retail S trail radius"), Ice->SRadius, 20.0f);
	const FWoTMeshEffectRow* Frozen = WoTMeshFX::Find(TEXT("FrozenPresentation"));
	if (TestNotNull(TEXT("frozen shell row"), Frozen))
	{
		TestEqual(TEXT("frozen shell lifetime matches leech"), Frozen->LifeSpan, 7.0f);
		TestTrue(TEXT("frozen shell uses environment map"), Frozen->bEnvironmentMap);
		TestEqual(TEXT("melt window"), Frozen->MeltTime, 2.0f);
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	// AlwaysSpawn + guards: the projectile/inventory spawn on top of the caster
	// and a collision-rejected SpawnActor returns NULL (the recorded
	// spec-spawn trap — this exact line crashed a full-suite run).
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	AWoTCharacter* Live = World->SpawnActor<AWoTCharacter>(FVector(100, 0, 0), FRotator::ZeroRotator, SpawnParams);
	AWoTCharacter* Dead = World->SpawnActor<AWoTCharacter>(FVector(200, 0, 0), FRotator::ZeroRotator, SpawnParams);
	AWoTCharacter* Blocked = World->SpawnActor<AWoTCharacter>(FVector(300, 0, 0), FRotator::ZeroRotator, SpawnParams);
	AWoTAngrealInvIce* IceInv = World->SpawnActor<AWoTAngrealInvIce>(
		FVector(0, 0, 200), FRotator::ZeroRotator, SpawnParams);
	AWoTIceProjectile* Shot = World->SpawnActor<AWoTIceProjectile>(
		FVector(0, 0, 400), FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("caster"), Caster) || !TestNotNull(TEXT("live victim"), Live)
		|| !TestNotNull(TEXT("dead victim"), Dead)
		|| !TestNotNull(TEXT("blocked victim"), Blocked)
		|| !TestNotNull(TEXT("ice inventory"), IceInv)
		|| !TestNotNull(TEXT("ice projectile"), Shot))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Dead->Health = 0; Dead->bDead = true;
	IceInv->SetOwner(Caster);
	Shot->SetSourceAngreal(IceInv);
	UWoTIgnoreWaterElementReflector* BlockingShield =
		NewObject<UWoTIgnoreWaterElementReflector>(Blocked);
	BlockingShield->Install(Blocked);
	Shot->ProcessDamage(Live, 0, nullptr, Live->GetActorLocation(), FVector::ZeroVector);
	Shot->ProcessDamage(Dead, 0, nullptr, Dead->GetActorLocation(), FVector::ZeroVector);
	Shot->ProcessDamage(Blocked, 0, nullptr, Blocked->GetActorLocation(), FVector::ZeroVector);
	UWoTIceLeech* Accepted = Live->FindComponentByClass<UWoTIceLeech>();
	AWoTMeshEffect* FirstShell = nullptr;
	if (TestNotNull(TEXT("real hit installed ice leech"), Accepted))
	{
		TestNotNull(TEXT("accepted leech owns frozen shell"), Accepted->FrozenShell.Get());
		FirstShell = Accepted->FrozenShell;
	}
	TestNull(TEXT("dead-pawn production hit was rejected"), Dead->FindComponentByClass<UWoTIceLeech>());
	TestNull(TEXT("blocked freeze installed no leech"),
		Blocked->FindComponentByClass<UWoTIceLeech>());
	Shot->ProcessDamage(Live, 0, nullptr, Live->GetActorLocation(), FVector::ZeroVector);
	UWoTIceLeech* Replacement = Live->FindComponentByClass<UWoTIceLeech>();
	TestTrue(TEXT("refreeze replaced the accepted leech instance"),
		IsValid(Replacement) && Replacement != Accepted);
	TestTrue(TEXT("refreeze retired the previous shell"), !IsValid(FirstShell));
	int32 FrozenShells = 0;
	for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
	{
		if (It->ConfigName == FName(TEXT("FrozenPresentation"))) { ++FrozenShells; }
	}
	TestEqual(TEXT("blocked freeze emitted no shell"), FrozenShells, 1);
	UWoTIgnoreWaterElementReflector* Cleanse =
		NewObject<UWoTIgnoreWaterElementReflector>(Live);
	Cleanse->Install(Live);
	TestNull(TEXT("water cleanse removed accepted ice leech"),
		Live->FindComponentByClass<UWoTIceLeech>());
	TestTrue(TEXT("cleanse retired leech-owned frozen shell"),
		!IsValid(Replacement) || !IsValid(Replacement->FrozenShell));

	// IceLeech.uc:209-212 size exceptions on row-folded creatures: the retail
	// script-IsA check must resolve through CreatureRowName (native ActorIsA never
	// matches AWoTNPCCharacter, which left the override dead), and
	// TrollocStrong inherits Trolloc's 4.0 via its own spelled-out entry.
	struct FFrozenScaleCase { const TCHAR* Row; float Scale; FVector At; };
	const FFrozenScaleCase ScaleCases[] = {
		{TEXT("TrollocStrong"), 4.0f, FVector(600, 0, 0)},
		{TEXT("Legion"), 12.0f, FVector(900, 0, 0)},
	};
	for (const FFrozenScaleCase& Case : ScaleCases)
	{
		AWoTNPCCharacter* Victim = WoTCreatureData::SpawnCreature(
			*World, Case.Row, Case.At, /*Team*/ 1, /*bCombatReady*/ true);
		if (!TestNotNull(*FString::Printf(TEXT("%s victim spawned"), Case.Row), Victim))
		{
			continue;
		}
		Victim->Health = 100;
		Shot->ProcessDamage(Victim, 0, nullptr, Victim->GetActorLocation(),
			FVector::ZeroVector);
		UWoTIceLeech* NPCLeech = Victim->FindComponentByClass<UWoTIceLeech>();
		if (TestNotNull(*FString::Printf(TEXT("%s froze"), Case.Row), NPCLeech)
			&& TestNotNull(*FString::Printf(TEXT("%s frozen shell"), Case.Row),
				NPCLeech->FrozenShell.Get()))
		{
			TestEqual(*FString::Printf(TEXT("%s frozen shell scale"), Case.Row),
				static_cast<float>(
					NPCLeech->FrozenShell->Mesh->GetRelativeScale3D().X),
				Case.Scale);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTDartSpinCoronaImpactSpec, "WoT.Dart.SpinCoronaImpact")
bool FWoTDartSpinCoronaImpactSpec::RunTest(const FString&)
{
	const AWoTDartProjectile* Dart = GetDefault<AWoTDartProjectile>();
	TestEqual(TEXT("retail roll speed"), Dart->SpinRate, 200000);
	TestFalse(TEXT("dart tick exclusively owns velocity-facing plus roll"),
		Dart->ProjectileMovement->bRotationFollowsVelocity);
	for (const TCHAR* Color : {TEXT("blue"), TEXT("red"), TEXT("green"),
		TEXT("yellow"), TEXT("purple")})
	{
		TestNotNull(*FString::Printf(TEXT("%s corona row"), Color),
			WoTMeshFX::FindSprite(FName(*FString::Printf(TEXT("DartCorona%s"), Color))));
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDartProjectile* LiveDart = World->SpawnActor<AWoTDartProjectile>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 1);
	TestNotNull(TEXT("real dart BeginPlay spawned corona"), LiveDart->CoronaLight.Get());
	const FRotator Spun = LiveDart->GetActorRotation();
	TestTrue(TEXT("real projectile tick accumulated roll"), !FMath::IsNearlyZero(Spun.Roll));
	struct FDartFactionCase
	{
		TSubclassOf<APawn> PawnClass;
		FName Expected;
	};
	const FDartFactionCase FactionCases[] = {
		{AWoTAesSedai::StaticClass(), TEXT("Blue")},
		{AWoTForsaken::StaticClass(), TEXT("Red")},
		{AWoTWhitecloak::StaticClass(), TEXT("Yellow")},
		{AWoTHound::StaticClass(), TEXT("Purple")},
		{AWoTNPCCharacter::StaticClass(), TEXT("Green")},
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(FactionCases); ++Index)
	{
		APawn* NPC = World->SpawnActor<APawn>(FactionCases[Index].PawnClass,
			FVector(0, 200.0f + Index * 100.0f, 0), FRotator::ZeroRotator);
		FActorSpawnParameters Params;
		Params.Instigator = NPC;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTDartProjectile* NPCDart = World->SpawnActor<AWoTDartProjectile>(
			FVector(100, 200.0f + Index * 100.0f, 0), FRotator::ZeroRotator, Params);
		if (TestNotNull(TEXT("NPC-owned production dart spawned"), NPCDart))
		{
			TestEqual(*FString::Printf(TEXT("NPC dart faction color %d"), Index),
				NPCDart->VariantColor, FactionCases[Index].Expected);
		}
	}
	LiveDart->Explode(FVector(20, 0, 0), FVector::UpVector);
	TestEqual(TEXT("real explode path spawned DartExplosion"),
		CountActors<AWoTDartExplosionEffect>(World), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTReflectFeedbackPathsSpec, "WoT.Reflect.FeedbackPaths")
bool FWoTReflectFeedbackPathsSpec::RunTest(const FString&)
{
	const UWoTReturnToSenderReflector* RTS = GetDefault<UWoTReturnToSenderReflector>();
	const UWoTForkRTSReflector* Fork = GetDefault<UWoTForkRTSReflector>();
	TestEqual(TEXT("RTS production sound"), RTS->ReflectSoundPath(), FName(TEXT("WOT/Reflected")));
	TestEqual(TEXT("Fork production sound"), Fork->ReflectSoundPath(), FName(TEXT("Angreal/ForkFK")));
	TestTrue(TEXT("Fork requests mottled overlay"), Fork->UsesMottledFeedback());
	for (const TCHAR* Config : {TEXT("ReflectGreen"), TEXT("ReflectGreenMottled"),
		TEXT("FireShieldVisual"), TEXT("WaterShieldVisual"), TEXT("AirShieldVisual"),
		TEXT("EarthShieldVisual"), TEXT("SpiritShieldVisual")})
	{
		const bool bReflect = FString(Config).StartsWith(TEXT("Reflect"));
		TestTrue(*FString::Printf(TEXT("%s feedback row"), Config),
			bReflect ? WoTMeshFX::FindPawnOverlay(FName(Config)) != nullptr
			         : WoTMeshFX::Find(FName(Config)) != nullptr);
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(FVector(100, 0, 0), FRotator::ZeroRotator);
	AWoTCharacter* Sender = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
	UWoTReturnToSenderReflector* Installed = NewObject<UWoTReturnToSenderReflector>(Host);
	Installed->Install(Host);
	FActorSpawnParameters Params; Params.Instigator = Sender;
	AWoTDartProjectile* Incoming = World->SpawnActor<AWoTDartProjectile>(
		FVector(50, 0, 0), FRotator::ZeroRotator, Params);
	Incoming->ProcessTouch(Host, Host->GetActorLocation());
	TestEqual(TEXT("real reflector dispatch emitted overlay marker"),
		CountActors<AWoTPawnOverlayBurst>(World), 1);
	TestTrue(TEXT("real reflector reassigned projectile"), Incoming->GetInstigator() == Host);
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTBalefireCastFireVictimSpec, "WoT.Balefire.CastFireVictim")
bool FWoTBalefireCastFireVictimSpec::RunTest(const FString&)
{
	TestNotNull(TEXT("gather corona class"), AWoTBalefireCorona::StaticClass());
	TestNotNull(TEXT("victim removal marker class"), AWoTBalefireRemovalMarker::StaticClass());
	const FWoTExplosionRow* Explode = WoTWeaveFX::FindExplosion(TEXT("BFExplodeA"));
	if (TestNotNull(TEXT("BFExplodeA row"), Explode))
	{
		TestTrue(TEXT("impact uses grow/sustain/shrink envelope"), Explode->bGrowSustainShrink);
		TestEqual(TEXT("impact frame count"), Explode->FrameCount, 21);
	}
	TestNotNull(TEXT("delayed BFExpSprayer row"), WoTWeaveFX::Find(TEXT("BFExpSprayer")));
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
	AWoTCharacter* Survivor = World->SpawnActor<AWoTCharacter>(FVector(200, 0, 0), FRotator::ZeroRotator);
	Survivor->MaxHealth = 1000;
	Survivor->Health = 1000;
	Survivor->Tags.Add(TEXT("PriorTag"));
	AWoTCharacter* Killed = World->SpawnActor<AWoTCharacter>(FVector(300, 0, 0), FRotator::ZeroRotator);
	Killed->GetMesh()->SetSkeletalMeshAsset(AWoTCarcass::ResolveMeshByName(TEXT("Trolloc")));
	AWoTAngrealInvBalefire* Inv = World->SpawnActor<AWoTAngrealInvBalefire>();
	Inv->SetOwner(Caster); Inv->PrimeTime = 0.1f;
	Inv->CastArtifact();
	TestNotNull(TEXT("real Cast spawned corona"), Inv->GatherEffect.Get());
	WoTTest::TickWorld(World, 12);
	Inv->UnCastArtifact();
	TestTrue(TEXT("real fire damaged surviving pawn"), Survivor->Health < Survivor->MaxHealth);
	TestTrue(TEXT("surviving pawn kept living"), Survivor->IsAlive());
	TestEqual(TEXT("surviving pawn tag array restored"), Survivor->Tags.Num(), 1);
	if (!Survivor->Tags.IsEmpty())
	{
		TestEqual(TEXT("surviving pawn prior tag restored"), Survivor->Tags[0],
			FName(TEXT("PriorTag")));
	}
	TestFalse(TEXT("second pawn was killed by production fire"), Killed->IsAlive());
	TestTrue(TEXT("killed pawn retained Balefired death marker"),
		Killed->ActorHasTag(TEXT("Balefired")));
	TestEqual(TEXT("real fire spawned line"), CountActors<AWoTBeamEffect>(World), 1);
	TestEqual(TEXT("the corpse emitted during the sustained beam is swept once"),
		CountActors<AWoTBalefireRemovalMarker>(World), 1);
	AWoTBeamEffect* Line = nullptr;
	if (TActorIterator<AWoTBeamEffect> It(World); It) { Line = *It; }
	if (TestNotNull(TEXT("production Balefire line actor"), Line))
	{
		const FVector LineStart(Line->Start);
		const FVector LineEnd(Line->End);
		TestTrue(TEXT("beam actor spawned at relevancy midpoint"),
			Line->GetActorLocation().Equals((LineStart + LineEnd) * 0.5f,
				0.1f));
		WoTTest::TickWorld(World, 1);
		const int32 Needed = FMath::CeilToInt(
			FVector::Distance(LineStart, LineEnd) / 18.0f);
		TestEqual(TEXT("sprite segment allocation covers complete beam extent"),
			Line->GetVisibleSegmentCount(), Needed);
	}

	// Force the standard early-destroy death seam; AWoTCharacter::Destroyed calls
	// the real SpawnCarcassNow path, whose InitFor must carry the Balefired tag.
	Killed->Destroy();
	AWoTCarcass* Carcass = nullptr;
	if (TActorIterator<AWoTCarcass> It(World); It) { Carcass = *It; }
	if (TestNotNull(TEXT("killed pawn production path spawned carcass"), Carcass))
	{
		TestTrue(TEXT("carcass inherited Balefired tag"),
			Carcass->ActorHasTag(TEXT("Balefired")));
		WoTTest::TickWorld(World, 1);
		TestNotNull(TEXT("tagged carcass sweep spawned removal presentation"),
			FindRemovalFor(World, Carcass));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTBalefireWhitelistAndRemovalStyleSpec,
	"WoT.Balefire.WhitelistAndRemovalStyle")
bool FWoTBalefireWhitelistAndRemovalStyleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("caster"), Caster))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// The beam fires from EYE height and the cylinder test measures the
	// target's own bounds around its origin — bare spec actors have ~zero
	// extents, so place every target ON the beam axis (placed map actors
	// carry real authored collision; the fixture supplies the alignment).
	// Freeze the caster too: 12 pre-fire ticks of gravity would sink the
	// beam ~20uu below the 1uu-extent targets.
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	AWoTDecoration* Decoration = World->SpawnActor<AWoTDecoration>(
		FVector(150, 0, EyeZ), FRotator::ZeroRotator);
	Decoration->Mesh->SetSkeletalMeshAsset(AWoTCarcass::ResolveMeshByName(TEXT("Trolloc")));
	AWoTWallSlab* Wall = World->SpawnActor<AWoTWallSlab>(
		FVector(250, 0, EyeZ), FRotator::ZeroRotator);
	AWoTPortcullisMover* Gate = World->SpawnActor<AWoTPortcullisMover>(
		FVector(350, 0, EyeZ), FRotator::ZeroRotator);
	Gate->Activate();
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		FVector(450, 0, EyeZ), FRotator::ZeroRotator);
	AWoTMashadarSegment* Segment = World->SpawnActor<AWoTMashadarSegment>(
		FVector(550, 0, EyeZ), FRotator::ZeroRotator);
	AWoTAngrealInvBalefire* Inv = World->SpawnActor<AWoTAngrealInvBalefire>();
	Inv->SetOwner(Caster);
	Inv->PrimeTime = 0.1f;
	Inv->CastArtifact();
	WoTTest::TickWorld(World, 12);
	AWoTDartProjectile* NonLegionProjectile = World->SpawnActor<AWoTDartProjectile>(
		FVector(650, 0, EyeZ), FRotator::ZeroRotator);
	AWoTCarcass* UntaggedCarcass = World->SpawnActor<AWoTCarcass>(
		FVector(750, 0, EyeZ), FRotator::ZeroRotator);
	Inv->UnCastArtifact();

	for (AActor* Removed : {static_cast<AActor*>(Decoration),
		static_cast<AActor*>(Wall), static_cast<AActor*>(Gate),
		static_cast<AActor*>(Segment)})
	{
		const FString Label = IsValid(Removed)
			? Removed->GetClass()->GetName() : TEXT("<destroyed>");
		TestNotNull(*FString::Printf(
			TEXT("%s received removal marker"), *Label),
			FindRemovalFor(World, Removed));
		TestTrue(*FString::Printf(
			TEXT("%s received full component-generic styling"), *Label),
			HasFullRemovalPresentation(Removed));
	}
	TestNull(TEXT("MashadarGuide is not MashadarTrailer and is never removed"),
		FindRemovalFor(World, Guide));
	TestNull(TEXT("generic AWoTProjectile is not the absent LegionProjectile"),
		FindRemovalFor(World, NonLegionProjectile));
	TestNotNull(TEXT("retail Decoration fold includes an untagged carcass"),
		FindRemovalFor(World, UntaggedCarcass));
	WoTTest::TickWorld(World, 40);
	TestTrue(TEXT("decoration removed after presentation window"), !IsValid(Decoration));
	TestTrue(TEXT("wall slab removed after presentation window"), !IsValid(Wall));
	TestTrue(TEXT("portcullis removed after presentation window"), !IsValid(Gate));
	TestTrue(TEXT("Mashadar body segment removed after presentation window"),
		!IsValid(Segment));
	TestTrue(TEXT("untagged carcass removed after presentation window"),
		!IsValid(UntaggedCarcass));
	TestTrue(TEXT("MashadarGuide survives Balefire whitelist sweep"), IsValid(Guide));
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTAMAMinePresentationSpec, "WoT.AMA.MinePresentation")
bool FWoTAMAMinePresentationSpec::RunTest(const FString&)
{
	const AWoTAngrealAMAProjectile* Mine = GetDefault<AWoTAngrealAMAProjectile>();
	TestNotNull(TEXT("anti-magic pickup display component"), Mine->DisplayMesh.Get());
	TestFalse(TEXT("new mine is not already fading"), Mine->IsFading());
	UWorld* World = WoTTest::CreateSpecWorld();
	AActor* Floor = World->SpawnActor<AActor>(
		FVector(0, 0, -20), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("AMA landing floor spawned"), Floor))
	{
		UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor, TEXT("AMAFloor"));
		if (TestNotNull(TEXT("AMA landing collision component created"), FloorBox))
		{
			Floor->SetRootComponent(FloorBox);
			Floor->AddInstanceComponent(FloorBox);
			FloorBox->SetBoxExtent(FVector(500, 500, 20));
			FloorBox->SetCollisionObjectType(ECC_WorldStatic);
			FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
			FloorBox->RegisterComponent();
		}
	}
	AWoTAngrealAMAProjectile* LiveMine = World->SpawnActor<AWoTAngrealAMAProjectile>(
		FVector(0, 0, 200), FRotator(-90, 0, 0));
	for (int32 Tick = 0; Tick < 300 && LiveMine && !LiveMine->IsPlaced(); ++Tick)
	{
		WoTTest::TickWorld(World, 1);
	}
	TestTrue(TEXT("real falling collision drove production Place"),
		LiveMine && LiveMine->IsPlaced());
	AWoTCharacter* Trigger = World->SpawnActor<AWoTCharacter>(
		FVector(10, 0, 0), FRotator::ZeroRotator);
	if (LiveMine) { LiveMine->Touch(Trigger); }   // the retail global Touch
	TestTrue(TEXT("real placed/tripped mine stays present for the pearl's first poll"),
		LiveMine && !LiveMine->IsFading());
	TestEqual(TEXT("real trip planted one pearl"), CountActors<AWoTAMAPearl>(World), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

WAVE_B_TEST(FWoTLightningBeltAndEndpointsSpec, "WoT.Lightning.BeltAndEndpoints")
bool FWoTLightningBeltAndEndpointsSpec::RunTest(const FString&)
{
	TestNotNull(TEXT("caster crawling-skin belt"),
		WoTMeshFX::FindPawnOverlay(TEXT("LightningSkinEffect")));
	TestNotNull(TEXT("victim LSImpact row"), WoTMeshFX::Find(TEXT("LSImpact")));
	const FWoTSprayerBundleRow* Sparks = WoTWeaveFX::Find(TEXT("LightningSparks"));
	if (TestNotNull(TEXT("victim endpoint sparks"), Sparks))
	{
		TestTrue(TEXT("two retail spark templates"), Sparks->Templates.Num() == 2);
		TestEqual(TEXT("endpoint gravity"), static_cast<float>(Sparks->Gravity.Z), -400.0f);
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
	World->SpawnActor<AWoTCharacter>(FVector(100, 0, 0), FRotator::ZeroRotator);
	AWoTAngrealInvLightning* Inv = World->SpawnActor<AWoTAngrealInvLightning>();
	Inv->SetOwner(Caster);
	Inv->CastArtifact();
	Inv->AttemptLatch();
	WoTTest::TickWorld(World, 2);
	TestNotNull(TEXT("real cast spawned caster belt"), Inv->PlayerEffect.Get());
	TestEqual(TEXT("real latch spawned one bolt"), Inv->Bolts.Num(), 1);
	bool bFoundLSImpact = false;
	for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
	{
		bFoundLSImpact |= It->ConfigName == FName(TEXT("LSImpact"));
	}
	TestTrue(TEXT("real bolt spawned specifically configured LSImpact mesh"),
		bFoundLSImpact);
	TestTrue(TEXT("real bolt spawned endpoint sparks"), CountActors<AWoTParticleSprayer>(World) >= 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#undef WAVE_B_TEST
#endif // WITH_AUTOMATION_TESTS
