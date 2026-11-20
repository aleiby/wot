#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestSoundDispatcher.h"
#include "WoTTestBattleClasses.h"
#include "../WoTEvents.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "UObject/UnrealType.h"
#include "Engine/World.h"

namespace WoTShootTriggerSpec
{
constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FFixture
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestSoundDispatcherEventSink* Sink = nullptr;
	AWoTTrigger* Target = nullptr;
	APawn* Instigator = nullptr;
	APlayerController* Controller = nullptr;
	FFixture()
	{
		Sink = World->SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
		Sink->TagName = TEXT("ShotEvent");
		Sink->FinishSpawning(FTransform::Identity);
		Target = World->SpawnActorDeferred<AWoTTrigger>(AWoTTrigger::StaticClass(), FTransform(FVector(300, 0, 1000)));
		Target->TriggerType = EWoTTriggerType::Shoot;
		Target->EventName = Sink->TagName;
		Target->SetUE1CollisionSize(48, 150);
		if (FFloatProperty* P = FindFProperty<FFloatProperty>(Target->GetClass(), TEXT("DamageThreshold")))
			P->SetPropertyValue_InContainer(Target, 10.0f);
		Target->FinishSpawning(Target->GetActorTransform());
		Instigator = World->SpawnActor<APawn>();
		Controller = World->SpawnActor<AWoTTestMessageCaptureController>();
		Controller->Possess(Instigator);
	}
	~FFixture() { WoTTest::DestroySpecWorld(World); }
	AWoTDartProjectile* Dart(float Damage = 10)
	{
		const FTransform At(FVector(0, 0, 1000));
		AWoTDartProjectile* Shot = World->SpawnActorDeferred<AWoTDartProjectile>(
			AWoTDartProjectile::StaticClass(), At, nullptr, Instigator,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Shot->Damage = Damage;
		Shot->FinishSpawning(At);
		return Shot;
	}
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTShootTriggerImpactSpec,
	"WoT.Events.ShootTrigger", WoTShootTriggerSpec::Flags)

void FWoTShootTriggerImpactSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : {TEXT("ImpactExactlyOnce"), TEXT("TouchThenSplash"), TEXT("TargetNotifiedFirst"), TEXT("PersistentReentry"), TEXT("RepeatWhileTouching"), TEXT("SplashOnly"), TEXT("Delay"), TEXT("Inactive"), TEXT("UnpossessedInstigator"), TEXT("Message"),
		TEXT("SweptTargetFirst"), TEXT("SweptFlatCap"), TEXT("SweptReusableSplash"), TEXT("SweptDeferred")})
	{
		Names.Add(Name);
		Commands.Add(Name);
	}
}

bool FWoTShootTriggerImpactSpec::RunTest(const FString& Case)
{
	WoTShootTriggerSpec::FFixture F;
	if (Case == TEXT("SweptDeferred"))
	{
		AWoTDartProjectile* Shot = F.Dart();
		const FVector Center = F.Target->GetActorLocation();
		WoTTest::PlaceWithoutOverlapEvents(Shot, Center + FVector(53, -42, 159));
		Shot->ProjectileMovement->Velocity = FVector(0, 1980, -120);
		WoTTest::TickWorld(F.World, 1, 1.0f / 60.0f);
		TestTrue(TEXT("first frame only enters containing capsule"), IsValid(Shot)
			&& Shot->GetActorLocation().Equals(Center + FVector(53, -9, 157), 0.05));
		TestEqual(TEXT("above-cap capsule entry fires nothing"), F.Sink->TriggerCount, 0);
		WoTTest::TickWorld(F.World, 1, 1.0f / 60.0f);
		TestFalse(TEXT("deferred swept crossing consumes dart"), IsValid(Shot));
		TestEqual(TEXT("deferred swept crossing fires once"), F.Sink->TriggerCount, 1);
		TestTrue(TEXT("deferred hit stops at the first flat-cap intersection"),
			Shot->GetActorLocation().Equals(Center + FVector(53, 7.5, 156), 0.05));
	}
	else if (Case == TEXT("SweptTargetFirst") || Case == TEXT("SweptFlatCap"))
	{
		AWoTDartProjectile* Shot = F.Dart();
		const FVector Center = F.Target->GetActorLocation();
		const bool bCap = Case == TEXT("SweptFlatCap");
		FHitResult Sweep;
		Sweep.TraceStart = Center + (bCap ? FVector(0, 0, 200) : FVector(-2000.0 / 120.0, 53, 0));
		Sweep.TraceEnd = Center + (bCap ? FVector(0, 0, -200) : FVector(2000.0 / 120.0, 53, 0));
		WoTTest::PlaceWithoutOverlapEvents(Shot, Sweep.TraceEnd);
		F.Target->OnOverlap(nullptr, Shot, Shot->GetActiveSweptCollision(), 0, true, Sweep);
		TestEqual(TEXT("target-first swept hit fires once"), F.Sink->TriggerCount, 1);
		TestFalse(TEXT("target-first swept hit consumes dart"), IsValid(Shot));
		TestTrue(TEXT("target-first hit consumes at first side or flat-cap contact"), Shot->GetActorLocation().Equals(
			Center + (bCap ? FVector(0, 0, 156) : FVector(-10.344080, 53, 0)), 0.05));
	}
	else if (Case == TEXT("SweptReusableSplash"))
	{
		AWoTFireballProjectile* Shot = F.World->SpawnActorDeferred<AWoTFireballProjectile>(
			AWoTFireballProjectile::StaticClass(), FTransform(FVector(0, 0, 1000)), nullptr, F.Instigator);
		Shot->FinishSpawning(Shot->GetActorTransform());
		const FVector Center = F.Target->GetActorLocation();
		WoTTest::PlaceWithoutOverlapEvents(Shot, Center + FVector(-1000.0 / 120.0, 53.75, 0));
		Shot->ProjectileMovement->Velocity = FVector(1000, 0, 0);
		WoTTest::TickWorld(F.World, 1, 1.0f / 60.0f);
		TestFalse(TEXT("swept fireball is consumed"), IsValid(Shot));
		TestEqual(TEXT("swept touch plus splash fires reusable target exactly once"), F.Sink->TriggerCount, 1);
		TestTrue(TEXT("reusable target remains live"), F.Target->IsRetailProjectileTarget());
		TestTrue(TEXT("swept explosion is at cylinder contact"), Shot->GetActorLocation().Equals(
			Center + FVector(-5.190135, 53.75, 0), 0.05));
	}
	else if (Case == TEXT("RepeatWhileTouching"))
	{
		F.Target->RepeatTriggerTime = 0.1f;
		AWoTDartProjectile* Shot = F.Dart();
		Shot->bExplode = false;
		Shot->ProjectileMovement->StopMovementImmediately();
		Shot->SetActorLocation(F.Target->GetActorLocation());
		TestEqual(TEXT("stationary projectile touches once"), F.Sink->TriggerCount, 1);
		WoTTest::TickWorld(F.World, 0.35f);
		TestTrue(TEXT("authored timer repeats while touching"), F.Sink->TriggerCount >= 3);
		Shot->SetActorLocation(FVector(0, 0, 1000));
		const int32 AtExit = F.Sink->TriggerCount;
		WoTTest::TickWorld(F.World, 0.3f);
		TestEqual(TEXT("timer stops after projectile leaves"), F.Sink->TriggerCount, AtExit);
	}
	else if (Case == TEXT("PersistentReentry"))
	{
		AWoTDartProjectile* Shot = F.Dart();
		Shot->bExplode = false;
		WoTTest::TickWorld(F.World, 24);
		TestEqual(TEXT("persistent shot touches once"), F.Sink->TriggerCount, 1);
		TestEqual(TEXT("persistent shot releases on exit"), F.Sink->UnTriggerCount, 1);
		WoTTest::PlaceWithoutOverlapEvents(Shot, FVector(0, 0, 1000));
		WoTTest::TickWorld(F.World, 24);
		TestEqual(TEXT("a distinct returning contact fires again"), F.Sink->TriggerCount, 2);
		TestEqual(TEXT("returning shot releases on exit"), F.Sink->UnTriggerCount, 2);
	}
	else if (Case == TEXT("TouchThenSplash"))
	{
		AWoTDartProjectile* Shot = F.Dart(40);
		Shot->bExplode = false;
		Shot->DamageRadius = 300;
		WoTTest::PlaceWithoutOverlapEvents(Shot, F.Target->GetActorLocation());
		F.Target->OnOverlap(nullptr, Shot, nullptr, 0, false, FHitResult());
		TestEqual(TEXT("touch dispatches before explosion"), F.Sink->TriggerCount, 1);
		Shot->Explode(Shot->GetActorLocation(), FVector::UpVector);
		TestEqual(TEXT("same shot splash does not dispatch again"), F.Sink->TriggerCount, 1);
	}
	else if (Case == TEXT("TargetNotifiedFirst"))
	{
		F.Target->bTriggerOnceOnly = true;
		AWoTDartProjectile* Shot = F.Dart();
		WoTTest::PlaceWithoutOverlapEvents(Shot, F.Target->GetActorLocation());
		F.Target->OnOverlap(nullptr, Shot, nullptr, 0, false, FHitResult());
		TestEqual(TEXT("target-first notification fires once"), F.Sink->TriggerCount, 1);
		TestFalse(TEXT("target-first notification consumes retiring hit"), IsValid(Shot));
	}
	else if (Case == TEXT("Message"))
	{
		FStrProperty* Message = FindFProperty<FStrProperty>(F.Target->GetClass(), TEXT("Message"));
		if (TestNotNull(TEXT("message is configurable"), Message)) Message->SetPropertyValue_InContainer(F.Target, TEXT("Target hit"));
		auto* Controller = CastChecked<AWoTTestMessageCaptureController>(F.Controller);
		F.Dart();
		WoTTest::TickWorld(F.World, 12);
		TestEqual(TEXT("touch sends one message"), Controller->CapturedMessages.Num(), 1);
		FWoTDamageEvent Damage;
		F.Target->TakeDamage(10, Damage, F.Controller, F.Instigator);
		TestEqual(TEXT("direct damage sends one message"), Controller->CapturedMessages.Num(), 2);
		for (const FString& Text : Controller->CapturedMessages) TestEqual(TEXT("authored message"), Text, FString(TEXT("Target hit")));
	}
	else if (Case == TEXT("Inactive"))
	{
		FBoolProperty* Active = FindFProperty<FBoolProperty>(F.Target->GetClass(), TEXT("bInitiallyActive"));
		if (TestNotNull(TEXT("initial activity is configurable"), Active)) Active->SetPropertyValue_InContainer(F.Target, false);
		F.Dart();
		WoTTest::TickWorld(F.World, 12);
		FWoTDamageEvent Damage;
		F.Target->TakeDamage(10, Damage, F.Controller, F.Instigator);
		TestEqual(TEXT("inactive target ignores touches and damage"), F.Sink->TriggerCount, 0);
		if (Active) Active->SetPropertyValue_InContainer(F.Target, true);
		F.Target->TakeDamage(10, Damage, F.Controller, F.Instigator);
		TestEqual(TEXT("active target accepts damage"), F.Sink->TriggerCount, 1);
	}
	else if (Case == TEXT("SplashOnly"))
	{
		AWoTFireballProjectile* Shot = F.World->SpawnActorDeferred<AWoTFireballProjectile>(
			AWoTFireballProjectile::StaticClass(), FTransform(FVector(200, 0, 1000)), nullptr, F.Instigator);
		Shot->Damage = 40;
		Shot->DamageRadius = 300;
		Shot->FinishSpawning(Shot->GetActorTransform());
		Shot->Explode(Shot->GetActorLocation(), FVector::UpVector);
		TestEqual(TEXT("splash without a touch fires once"), F.Sink->TriggerCount, 1);
		TestEqual(TEXT("splash uses instigator as Other"), F.Sink->LastOther.Get(), static_cast<AActor*>(F.Instigator));
	}
	else if (Case == TEXT("UnpossessedInstigator"))
	{
		F.Controller->UnPossess();
		AWoTDartProjectile* Shot = F.Dart();
		Shot->ProcessDamage(F.Target, 10, F.Instigator, F.Target->GetActorLocation(), FVector::ZeroVector);
		TestEqual(TEXT("projectile damage retains an unpossessed instigator"), F.Sink->TriggerCount, 1);
		TestEqual(TEXT("unpossessed damage dispatch"), F.Sink->LastInstigator.Get(), F.Instigator);
	}
	else if (Case == TEXT("Delay"))
	{
		F.Target->ReTriggerDelay = 0.5f;
		WoTTest::TickWorld(F.World, 0.6f);
		F.Dart();
		WoTTest::TickWorld(F.World, 12);
		TestEqual(TEXT("first shot fires"), F.Sink->TriggerCount, 1);
		F.Dart();
		WoTTest::TickWorld(F.World, 12);
		TestEqual(TEXT("second shot inside delay is suppressed"), F.Sink->TriggerCount, 1);
		WoTTest::TickWorld(F.World, 0.6f);
		F.Dart();
		WoTTest::TickWorld(F.World, 12);
		TestEqual(TEXT("later shot fires"), F.Sink->TriggerCount, 2);
	}
	else
	{
		// Zero delay and reusable target expose double delivery hidden by a
		// once-only target. Exercise the real flying impact, then two distinct shots.
		AWoTDartProjectile* First = F.Dart();
		First->DamageRadius = 300;
		WoTTest::TickWorld(F.World, 12);
		TestFalse(TEXT("impact consumes dart"), IsValid(First));
		TestEqual(TEXT("touch and damage dispatch once"), F.Sink->TriggerCount, 1);
		F.Dart();
		F.Dart();
		WoTTest::TickWorld(F.World, 12);
		TestEqual(TEXT("distinct simultaneous shots each dispatch"), F.Sink->TriggerCount, 3);
	}
	return !HasAnyErrors();
}

#endif
