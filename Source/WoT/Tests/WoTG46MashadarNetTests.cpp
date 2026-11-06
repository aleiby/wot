// G46: client-local Mashadar trailer chains and replicated FID reactions.
//
// These follow WoTNetProjectileTests.cpp's single-process network seam: the spec
// world is standalone, so a deferred guide is forced to ROLE_SimulatedProxy before
// BeginPlay and its replicated fields are populated directly. This proves the role
// gates and local-chain mechanism; a parent multiplayer harness remains the end-to-end
// replication proof.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTProjectile.h"
#include "WoTTestAIClasses.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"

namespace
{
	FActorSpawnParameters G46AlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	int32 G46BodyCount(const AWoTMashadarGuide* Guide)
	{
		int32 Count = 0;
		const AWoTMashadarSegment* Head = Guide ? Guide->GetMasterSegment() : nullptr;
		for (const AWoTIKTrailer* Segment = Head ? Head->ChildTrailer : nullptr;
			Segment; Segment = Segment->ChildTrailer)
		{
			++Count;
		}
		return Count;
	}

	AWoTMashadarGuide* G46SpawnClientGuide(
		UWorld* World, const FVector& InitialLocation,
		const FRotator& InitialRotation, const bool bExtending,
		const int32 MaxSegments = 100,
		const bool bReplayHistoricalDestinationBeforeBeginPlay = false,
		const bool bEditingSpeedContext = false)
	{
		const FTransform Transform(InitialRotation, InitialLocation);
		AWoTMashadarGuide* Guide =
			World->SpawnActorDeferred<AWoTMashadarGuide>(
				AWoTMashadarGuide::StaticClass(), Transform, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Guide)
		{
			return nullptr;
		}

		// Mimic the initial actor-channel bunch before BeginPlay. No gameplay mutator is
		// called: all state below is the replicated durable/config surface.
		Guide->SetRole(ROLE_SimulatedProxy);
		Guide->InitialGuideLocation = InitialLocation;
		Guide->InitialGuideRotation = InitialRotation;
		Guide->HidingPlaceLocation = InitialLocation;
		Guide->InitialSpeed = 275.0f;
		Guide->InitialMaxSpeed = 525.0f;
		Guide->InitialAcceleration = 31.0f;
		Guide->HeadDist = 48.0f;
		Guide->BodyDist = 42.0f;
		Guide->MaxNumSegments = MaxSegments;
		Guide->Team = 3;
		Guide->bActive = bExtending;
		Guide->ChainPhase = bExtending
			? EWoTMashadarChainPhase::Extending
			: EWoTMashadarChainPhase::Resting;
		Guide->bUseCitadelEditingSpeed = bEditingSpeedContext;
		if (bExtending)
		{
			Guide->Destination = Guide;
			Guide->ClientSetDestinationCounter = 1;
		}
		else if (bReplayHistoricalDestinationBeforeBeginPlay)
		{
			// Initial actor-channel RepNotify callbacks precede BeginPlay. Replay an old
			// destination edge while the durable current phase says this joiner is resting.
			Guide->ClientSetDestinationCounter = 1;
			Guide->OnRep_ClientSetDestinationCounter();
		}
		Guide->FinishSpawning(Transform);
		return Guide;
	}

	AWoTTestNPCCharacter* G46SpawnNPC(
		UWorld* World, const FVector& Location, const uint8 Team)
	{
		AWoTTestNPCCharacter* NPC =
			World->SpawnActor<AWoTTestNPCCharacter>(
				Location, FRotator::ZeroRotator, G46AlwaysSpawnParams());
		if (NPC)
		{
			NPC->AssignedTeam = Team;
			NPC->Health = 100;
		}
		return NPC;
	}

	AWoTMashadarSegment* G46TrailerAt(
		const AWoTMashadarGuide* Guide, int32 OneBasedIndex)
	{
		AWoTIKTrailer* Segment = Guide ? Guide->GetMasterSegment() : nullptr;
		while (Segment && --OneBasedIndex > 0)
		{
			Segment = Segment->ChildTrailer;
		}
		return Cast<AWoTMashadarSegment>(Segment);
	}
}

#define G46_TEST(ClassName, Path) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

G46_TEST(FWoTG46ClientChainConstructionSpec,
	"WoT.Net.Mashadar.ClientLocalChainConstruction")
bool FWoTG46ClientChainConstructionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FVector InitialLocation(120.0f, -350.0f, 140.0f);
	const FRotator InitialRotation(0.0f, 90.0f, 0.0f);
	AWoTMashadarGuide* Guide = G46SpawnClientGuide(
		World, InitialLocation, InitialRotation, true);
	if (!TestNotNull(TEXT("simulated-proxy guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	TestFalse(TEXT("guide has no authority"), Guide->HasAuthority());
	TestTrue(TEXT("client restored the authored initial rotation"),
		Guide->GetActorRotation().Equals(InitialRotation, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("client applied the authored transform plus head offset"),
		Guide->GetActorLocation().Equals(
			InitialLocation + InitialRotation.Vector() * Guide->HeadDist,
			KINDA_SMALL_NUMBER));
	TestEqual(TEXT("client consumed replicated initial speed"),
		FMath::RoundToInt(Guide->Speed), 275);
	TestEqual(TEXT("client consumed replicated initial max speed"),
		FMath::RoundToInt(Guide->MaxSpeed), 525);
	TestEqual(TEXT("client consumed replicated acceleration"),
		FMath::RoundToInt(Guide->Acceleration), 31);

	AWoTMashadarSegment* Head = Guide->GetMasterSegment();
	if (!TestNotNull(TEXT("client built its local head segment"), Head))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("replicated destination state reveals the local head"),
		!Head->IsHidden());

	// Simulate a later replicated movement update away from the lair. Client Tick
	// must grow the private chain without calling Search, Activate, or SetDestination.
	Guide->SetActorLocation(InitialLocation + FVector(0.0f, 420.0f, 0.0f));
	WoTTest::TickWorld(World, 1);
	const int32 ExtendedBody = G46BodyCount(Guide);
	TestTrue(TEXT("one tick catches up a replicated movement jump"),
		ExtendedBody >= 3);
	TestTrue(TEXT("client-local body segments grow from replicated movement"),
		G46BodyCount(Guide) >= 3);
	TestTrue(TEXT("G42 billboard quads are live on the client-local head"),
		Head->ParticleQuads && Head->ParticleQuads->GetInstanceCount() > 0);

	// Retraction also follows replicated reach in one tick rather than peeling one
	// body per rendered frame.
	++Guide->ClientDeactivateCounter;
	Guide->OnRep_ClientDeactivateCounter();
	Guide->SetActorLocation(InitialLocation + FVector(0.0f, 168.0f, 0.0f));
	WoTTest::TickWorld(World, 1);
	TestTrue(TEXT("one tick trims every segment beyond the current reach"),
		G46BodyCount(Guide) < ExtendedBody);
	Guide->SetActorLocation(InitialLocation);
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("home reach leaves only the head"), G46BodyCount(Guide), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

G46_TEST(FWoTG46InitialPhaseOrderingSpec,
	"WoT.Net.Mashadar.InitialPhaseWinsAfterRepNotifyReplay")
bool FWoTG46InitialPhaseOrderingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* RestingGuide = G46SpawnClientGuide(
		World, FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		false, 100, true);
	AWoTMashadarGuide* EditingGuide = G46SpawnClientGuide(
		World, FVector(500.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		true, 100, false, true);
	if (!TestNotNull(TEXT("resting joiner spawned"), RestingGuide)
		|| !TestNotNull(TEXT("editing joiner spawned"), EditingGuide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	TestEqual(TEXT("historical destination edge replayed before BeginPlay"),
		RestingGuide->GetSetDestinationReactionCountForTesting(), 1u);
	TestEqual(TEXT("durable resting phase remains the join-time truth"),
		RestingGuide->ChainPhase, EWoTMashadarChainPhase::Resting);
	TestFalse(TEXT("durable resting phase overrides edge retraction state"),
		RestingGuide->IsRetractingForTesting());
	TestTrue(TEXT("durable resting phase hides the head after replay"),
		RestingGuide->GetMasterSegment()->IsHidden());
	TestEqual(TEXT("unresolved focus still receives editing speed context"),
		FMath::RoundToInt(EditingGuide->Speed),
		FMath::RoundToInt(EditingGuide->CitadelEditingSpeed));
	TestEqual(TEXT("unresolved focus still receives editing max speed context"),
		FMath::RoundToInt(EditingGuide->MaxSpeed),
		FMath::RoundToInt(EditingGuide->CitadelEditingSpeed));
	const FProperty* PhaseProperty = FindFProperty<FProperty>(
		AWoTMashadarGuide::StaticClass(), TEXT("ChainPhase"));
	const FProperty* SpeedContextProperty = FindFProperty<FProperty>(
		AWoTMashadarGuide::StaticClass(), TEXT("bUseCitadelEditingSpeed"));
	TestTrue(TEXT("durable chain phase is replicated"),
		PhaseProperty && PhaseProperty->HasAnyPropertyFlags(CPF_Net));
	TestTrue(TEXT("durable speed context is replicated"),
		SpeedContextProperty && SpeedContextProperty->HasAnyPropertyFlags(CPF_Net));

	WoTTest::DestroySpecWorld(World);
	return true;
}

G46_TEST(FWoTG46ReactionCounterSpec,
	"WoT.Net.Mashadar.ReactionCountersIdempotent")
bool FWoTG46ReactionCounterSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* Guide = G46SpawnClientGuide(
		World, FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, false);
	if (!TestNotNull(TEXT("client guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Guide->bActive = true;
	const uint32 DeactivateBefore = Guide->GetDeactivateReactionCountForTesting();
	++Guide->ClientDeactivateCounter;
	Guide->OnRep_ClientDeactivateCounter();
	TestEqual(TEXT("deactivate increment invokes once"),
		Guide->GetDeactivateReactionCountForTesting(), DeactivateBefore + 1);
	TestTrue(TEXT("deactivate reaction retracts"), Guide->IsRetractingForTesting());
	TestFalse(TEXT("deactivate reaction disables bites"), Guide->bActive);
	Guide->OnRep_ClientDeactivateCounter();
	TestEqual(TEXT("deactivate duplicate is idempotent"),
		Guide->GetDeactivateReactionCountForTesting(), DeactivateBefore + 1);

	const uint32 WrapBefore = Guide->GetDeactivateReactionCountForTesting();
	Guide->ClientDeactivateCounter = MAX_uint32;
	Guide->OnRep_ClientDeactivateCounter();
	Guide->ClientDeactivateCounter = 0;
	Guide->OnRep_ClientDeactivateCounter();
	TestEqual(TEXT("counter wrap from max to zero consumes the new edge"),
		Guide->GetDeactivateReactionCountForTesting(), WrapBefore + 2);
	Guide->OnRep_ClientDeactivateCounter();
	TestEqual(TEXT("wrapped zero remains idempotent on duplicate notify"),
		Guide->GetDeactivateReactionCountForTesting(), WrapBefore + 2);

	const uint32 UnRetractBefore = Guide->GetUnRetractReactionCountForTesting();
	++Guide->ClientUnRetractCounter;
	Guide->OnRep_ClientUnRetractCounter();
	TestEqual(TEXT("un-retract increment invokes once"),
		Guide->GetUnRetractReactionCountForTesting(), UnRetractBefore + 1);
	TestFalse(TEXT("un-retract reaction resumes extension"),
		Guide->IsRetractingForTesting());
	Guide->OnRep_ClientUnRetractCounter();
	TestEqual(TEXT("un-retract duplicate is idempotent"),
		Guide->GetUnRetractReactionCountForTesting(), UnRetractBefore + 1);

	AWoTTestNPCCharacter* SameBunchDestination = G46SpawnNPC(
		World, FVector(200.0f, 0.0f, 100.0f), 2);
	if (!TestNotNull(TEXT("same-bunch destination payload spawned"), SameBunchDestination))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const uint32 DestinationBefore =
		Guide->GetSetDestinationReactionCountForTesting();
	// Model one replication bunch: deserialize its actor payload first, then the
	// associated edge counter and RepNotify.
	Guide->Destination = SameBunchDestination;
	++Guide->ClientSetDestinationCounter;
	Guide->OnRep_ClientSetDestinationCounter();
	TestEqual(TEXT("set-destination increment invokes once"),
		Guide->GetSetDestinationReactionCountForTesting(), DestinationBefore + 1);
	TestFalse(TEXT("set-destination reveals the billboard head"),
		Guide->GetMasterSegment()->IsHidden());
	TestEqual(TEXT("same-bunch counter reaction observes its payload"),
		Guide->GetLastSetDestinationPayloadForTesting(),
		static_cast<AActor*>(SameBunchDestination));
	Guide->OnRep_ClientSetDestinationCounter();
	TestEqual(TEXT("set-destination duplicate is idempotent"),
		Guide->GetSetDestinationReactionCountForTesting(), DestinationBefore + 1);

	const uint32 FocusABefore = Guide->GetSetFocusPawnAReactionCountForTesting();
	++Guide->ClientSetFocusPawnACounter;
	Guide->OnRep_ClientSetFocusPawnACounter();
	TestEqual(TEXT("focus-A increment invokes once"),
		Guide->GetSetFocusPawnAReactionCountForTesting(), FocusABefore + 1);
	TestEqual(TEXT("focus-A applies editing speed"),
		FMath::RoundToInt(Guide->Speed), FMath::RoundToInt(Guide->CitadelEditingSpeed));
	Guide->OnRep_ClientSetFocusPawnACounter();
	TestEqual(TEXT("focus-A duplicate is idempotent"),
		Guide->GetSetFocusPawnAReactionCountForTesting(), FocusABefore + 1);

	const uint32 FocusBBefore = Guide->GetSetFocusPawnBReactionCountForTesting();
	++Guide->ClientSetFocusPawnBCounter;
	Guide->OnRep_ClientSetFocusPawnBCounter();
	TestEqual(TEXT("focus-B increment invokes once"),
		Guide->GetSetFocusPawnBReactionCountForTesting(), FocusBBefore + 1);
	TestEqual(TEXT("focus-B restores initial speed"),
		FMath::RoundToInt(Guide->Speed), FMath::RoundToInt(Guide->InitialSpeed));
	TestTrue(TEXT("focus-B reaction retracts"), Guide->IsRetractingForTesting());
	Guide->OnRep_ClientSetFocusPawnBCounter();
	TestEqual(TEXT("focus-B duplicate is idempotent"),
		Guide->GetSetFocusPawnBReactionCountForTesting(), FocusBBefore + 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

G46_TEST(FWoTG46ClientBreakOffSpec,
	"WoT.Net.Mashadar.ClientBreakOffAtIndex")
bool FWoTG46ClientBreakOffSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FVector Lair(0.0f, 0.0f, 100.0f);
	AWoTMashadarGuide* Guide = G46SpawnClientGuide(
		World, Lair, FRotator::ZeroRotator, true);
	if (!TestNotNull(TEXT("client guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Guide->SetActorLocation(Lair + FVector(420.0f, 0.0f, 0.0f));
	WoTTest::TickWorld(World, 1);
	const int32 Before = G46BodyCount(Guide);
	if (!TestTrue(TEXT("fixture grew enough body for one-based index 3"), Before >= 4))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Index 1=head, 2=first body, 3=second body. Retail removes the two body
	// segments through the hit and links the head to what followed index 3.
	const uint32 BreakBefore = Guide->GetBreakOffReactionCountForTesting();
	Guide->MulticastBreakOffMashadarAt_Implementation(3);
	TestEqual(TEXT("client chain pruned at the matching one-based index"),
		G46BodyCount(Guide), Before - 2);
	TestTrue(TEXT("client break-off deactivates and fully retracts"),
		Guide->bFullRetract && !Guide->bActive && Guide->IsRetractingForTesting());
	TestEqual(TEXT("multicast invokes the break-off reaction once"),
		Guide->GetBreakOffReactionCountForTesting(), BreakBefore + 1);
	const int32 AfterBreak = G46BodyCount(Guide);
	Guide->MulticastBreakOffMashadarAt_Implementation(3);
	TestEqual(TEXT("duplicate multicast delivery is idempotent"),
		Guide->GetBreakOffReactionCountForTesting(), BreakBefore + 1);
	TestEqual(TEXT("duplicate multicast does not sever the chain twice"),
		G46BodyCount(Guide), AfterBreak);

	// A real client reaches Destroyed from the actor-channel close. The standalone
	// seam must temporarily restore authority so UWorld permits the same teardown.
	Guide->SetRole(ROLE_Authority);
	Guide->Destroy();
	TestNull(TEXT("guide teardown follows the delivered multicast"),
		Guide->GetMasterSegment());

	WoTTest::DestroySpecWorld(World);
	return true;
}

G46_TEST(FWoTG46LethalMulticastBeforeTeardownSpec,
	"WoT.Net.Mashadar.LethalMulticastBeforeGuideTeardown")
bool FWoTG46LethalMulticastBeforeTeardownSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		G46AlwaysSpawnParams());
	if (!TestNotNull(TEXT("authority guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const UFunction* BreakOffRPC = AWoTMashadarGuide::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(AWoTMashadarGuide, MulticastBreakOffMashadarAt));
	TestTrue(TEXT("break-off delivery is a reliable multicast"),
		BreakOffRPC
		&& BreakOffRPC->HasAnyFunctionFlags(FUNC_NetMulticast)
		&& BreakOffRPC->HasAnyFunctionFlags(FUNC_NetReliable));
	Guide->bActive = true;
	Guide->SetActorLocation(Guide->HidingPlaceLocation + FVector(420.0f, 0.0f, 0.0f));
	WoTTest::TickWorld(World, 1);
	AWoTMashadarSegment* HitSegment = G46TrailerAt(Guide, 3);
	if (!TestNotNull(TEXT("one-tick catch-up produced lethal hit index 3"), HitSegment))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Guide->MashadarHealth = 1;
	FWoTDamageEvent Event;
	Event.WoTDamageType = TEXT("hurt");
	HitSegment->TakeDamage(1.0f, Event, nullptr, nullptr);
	TestEqual(TEXT("lethal path delivered multicast before destruction"),
		Guide->GetBreakOffReactionCountForTesting(), 1u);
	TestTrue(TEXT("lethal path entered full retract before destruction"),
		Guide->bFullRetract);
	TestNull(TEXT("lethal path then tore down the guide chain"),
		Guide->GetMasterSegment());

	WoTTest::DestroySpecWorld(World);
	return true;
}

G46_TEST(FWoTG46SegmentAuthorityGuardSpec,
	"WoT.Net.Mashadar.SegmentAuthorityGuards")
bool FWoTG46SegmentAuthorityGuardSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* Guide = G46SpawnClientGuide(
		World,
		FVector(-500.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		true);
	AWoTTestNPCCharacter* Victim = G46SpawnNPC(
		World, FVector(500.0f, 0.0f, 100.0f), 2);
	if (!TestNotNull(TEXT("simulated-proxy guide spawned"), Guide)
		|| !TestNotNull(TEXT("victim spawned"), Victim)
		|| !TestNotNull(TEXT("head segment exists"), Guide ? Guide->GetMasterSegment() : nullptr))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Guide->Team = 1;
	Guide->bActive = true;
	Guide->MashadarHealth = 100;
	AWoTMashadarSegment* Segment = Guide->GetMasterSegment();
	TestFalse(TEXT("client-local guide has no authority"), Guide->HasAuthority());
	TestTrue(TEXT("client-local segment itself reports local authority"),
		Segment->HasAuthority());
	const int32 VictimHealth = Victim->Health;
	Segment->BiteTarget(Victim);
	TestEqual(TEXT("guide-role guard refuses client-local bite damage"),
		Victim->Health, VictimHealth);

	const int32 GuideHealth = Guide->MashadarHealth;
	FWoTDamageEvent Event;
	Event.WoTDamageType = TEXT("hurt");
	TestEqual(TEXT("guide-role guard refuses client-local damage routing"),
		Segment->TakeDamage(25.0f, Event, nullptr, nullptr), 0.0f);
	TestEqual(TEXT("client segment cannot drain guide health"),
		Guide->MashadarHealth, GuideHealth);

	WoTTest::DestroySpecWorld(World);
	return true;
}

G46_TEST(FWoTG46AuthorityOnlySegmentCapSpec,
	"WoT.Net.Mashadar.AuthorityOnlySegmentCap")
bool FWoTG46AuthorityOnlySegmentCapSpec::RunTest(const FString&)
{
	UWorld* AuthorityWorld = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* AuthorityGuide =
		AuthorityWorld->SpawnActor<AWoTMashadarGuide>(
			FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
			G46AlwaysSpawnParams());
	if (!TestNotNull(TEXT("authority guide spawned"), AuthorityGuide))
	{
		WoTTest::DestroySpecWorld(AuthorityWorld);
		return false;
	}
	AuthorityGuide->MaxNumSegments = 1;
	AuthorityGuide->bActive = true;
	AuthorityGuide->SetActorLocation(
		AuthorityGuide->HidingPlaceLocation + FVector(420.0f, 0.0f, 0.0f));
	WoTTest::TickWorld(AuthorityWorld, 1);
	TestEqual(TEXT("authority appends before enforcing MaxNumSegments"),
		G46BodyCount(AuthorityGuide), AuthorityGuide->MaxNumSegments + 1);
	TestTrue(TEXT("authority cap decides full deactivation"),
		AuthorityGuide->bFullRetract && !AuthorityGuide->bActive);
	TestTrue(TEXT("authority cap broadcasts the deactivate reaction"),
		AuthorityGuide->ClientDeactivateCounter > 0);
	WoTTest::DestroySpecWorld(AuthorityWorld);

	UWorld* ClientWorld = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* ClientGuide = G46SpawnClientGuide(
		ClientWorld, FVector(0.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, true, 1);
	if (!TestNotNull(TEXT("client guide spawned"), ClientGuide))
	{
		WoTTest::DestroySpecWorld(ClientWorld);
		return false;
	}
	ClientGuide->SetActorLocation(FVector(420.0f, 0.0f, 100.0f));
	WoTTest::TickWorld(ClientWorld, 1);
	TestTrue(TEXT("client does not enforce the authority segment cap"),
		G46BodyCount(ClientGuide) > ClientGuide->MaxNumSegments);
	TestTrue(TEXT("client cap never self-deactivates"),
		ClientGuide->bActive && !ClientGuide->bFullRetract);
	TestEqual(TEXT("client cap never authors a deactivate reaction"),
		ClientGuide->ClientDeactivateCounter, 0u);
	WoTTest::DestroySpecWorld(ClientWorld);
	return true;
}

G46_TEST(FWoTG46FullRetractHomeResetSpec,
	"WoT.Net.Mashadar.FullRetractHomeResetReenablesAcquisition")
bool FWoTG46FullRetractHomeResetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		G46AlwaysSpawnParams());
	AWoTTestNPCCharacter* Victim = G46SpawnNPC(
		World, FVector(300.0f, 0.0f, 100.0f), 2);
	if (!TestNotNull(TEXT("authority guide spawned"), Guide)
		|| !TestNotNull(TEXT("acquisition target spawned"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Guide->Team = 1;
	Guide->Deactivate();
	Guide->bFullRetract = true;
	Guide->Speed = 999.0f;
	Guide->MaxSpeed = 1001.0f;
	Guide->Acceleration = 77.0f;
	Guide->SetActorLocation(Guide->HidingPlaceLocation);

	Guide->SearchAndChase();
	Guide->NotifySeePawn(Victim);
	TestNull(TEXT("full retract blocks polling acquisition"), Guide->Destination.Get());
	TestFalse(TEXT("full retract blocks sight activation"), Guide->bActive);
	TestEqual(TEXT("home-reset fixture contains only the head"), G46BodyCount(Guide), 0);

	WoTTest::TickWorld(World, 1);
	TestFalse(TEXT("home reset clears full retract"), Guide->bFullRetract);
	TestEqual(TEXT("home reset publishes resting phase"),
		Guide->ChainPhase, EWoTMashadarChainPhase::Resting);
	TestFalse(TEXT("home reset leaves retraction"), Guide->IsRetractingForTesting());
	TestTrue(TEXT("home reset restores exact initial location"),
		Guide->GetActorLocation().Equals(
			Guide->InitialGuideLocation, KINDA_SMALL_NUMBER));
	TestEqual(TEXT("home reset restores speed"),
		Guide->Speed, Guide->InitialSpeed);
	TestEqual(TEXT("home reset restores max speed"),
		Guide->MaxSpeed, Guide->InitialMaxSpeed);
	TestEqual(TEXT("home reset restores acceleration"),
		Guide->Acceleration, Guide->InitialAcceleration);
	TestTrue(TEXT("home reset hides the head"),
		Guide->GetMasterSegment()->IsHidden());

	Guide->NotifySeePawn(Victim);
	TestEqual(TEXT("cleared full retract re-enables sight acquisition"),
		Guide->Destination.Get(), static_cast<AActor*>(Victim));
	TestTrue(TEXT("cleared full retract re-enables activation"), Guide->bActive);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#undef G46_TEST

#endif // WITH_AUTOMATION_TESTS
