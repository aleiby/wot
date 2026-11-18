// Automation specs for the original engine's sound channel rules, observed in
// real worlds with real shipped classes and sound tables: per-actor slots, the
// shared sixteen-voice budget across one-shot, ambient and interface sounds,
// sounds that follow their source, ambient Doppler, close-range centring, the
// underwater rules of the shipped WOTPlayer.uc ClientHearSound, and random
// ambience chosen by the server (DynamicAmbientSound.uc).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTAmbientSound.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTFragment.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTZoneVolume.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTFireworksProjectile.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "WoTTestAIClasses.h"

#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
	constexpr EAutomationTestFlags WoTSoundParityFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FActorSpawnParameters WoTSoundParitySpawn()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	// A local listener: a player controller possessing a still player pawn.
	AWoTCharacter* SpawnSoundListener(UWorld* World, const FVector& Location,
		AWoTPlayerController** OutController = nullptr)
	{
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), Location, FRotator::ZeroRotator,
			WoTSoundParitySpawn());
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
			AWoTPlayerController::StaticClass(), Location, FRotator::ZeroRotator,
			WoTSoundParitySpawn());
		if (!Pawn || !Controller)
		{
			return nullptr;
		}
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		if (OutController)
		{
			*OutController = Controller;
		}
		return Pawn;
	}

	AWoTNPCCharacter* SpawnSoundCreature(UWorld* World, const FVector& Location)
	{
		AWoTNPCCharacter* Creature = WoTCreatureData::SpawnCreature(*World,
			TEXT("Trolloc"), Location, 1, /*bCombatReady*/ true,
			AWoTTestNPCCharacter::StaticClass());
		if (Creature)
		{
			Creature->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return Creature;
	}

	AActor* SpawnSoundSource(UWorld* World, const FVector& Location)
	{
		AActor* Source = World->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform(Location), WoTSoundParitySpawn());
		USceneComponent* Root = NewObject<USceneComponent>(Source);
		Source->SetRootComponent(Root);
		Root->RegisterComponent();
		Source->SetActorLocation(Location);
		return Source;
	}

	TArray<WoTSound::FChannelSnapshot> SoundChannelsOf(UWorld* World,
		const UObject* Source, WoTSound::ESlot Slot)
	{
		TArray<WoTSound::FChannelSnapshot> Out;
		for (const WoTSound::FChannelSnapshot& Snap :
			WoTSound::GetChannelsForTesting(*World))
		{
			if (Snap.SourceKey == FObjectKey(Source) && Snap.Slot == Slot)
			{
				Out.Add(Snap);
			}
		}
		return Out;
	}

	int32 CountSoundChannels(UWorld* World)
	{
		return WoTSound::GetChannelsForTesting(*World).Num();
	}

	bool SoundNameStartsWith(const WoTSound::FChannelSnapshot& Snap,
		const TCHAR* Prefix)
	{
		return Snap.SoundName.ToString().StartsWith(Prefix);
	}

	// Channels playing a wave whose name starts with Prefix, whoever plays it.
	TArray<WoTSound::FChannelSnapshot> SoundChannelsNamed(UWorld* World,
		const TCHAR* Prefix)
	{
		TArray<WoTSound::FChannelSnapshot> Out;
		for (const WoTSound::FChannelSnapshot& Snap :
			WoTSound::GetChannelsForTesting(*World))
		{
			if (SoundNameStartsWith(Snap, Prefix))
			{
				Out.Add(Snap);
			}
		}
		return Out;
	}

	bool SameSoundChannel(const WoTSound::FChannelSnapshot& A,
		const WoTSound::FChannelSnapshot& B)
	{
		return A.Index == B.Index && A.SoundName == B.SoundName && A.Slot == B.Slot
			&& A.SourceKey == B.SourceKey && A.Location.Equals(B.Location, 0.01f)
			&& FMath::IsNearlyEqual(A.Volume, B.Volume)
			&& FMath::IsNearlyEqual(A.Radius, B.Radius)
			&& FMath::IsNearlyEqual(A.Pitch, B.Pitch);
	}

	// The channel a play just started: present after it and not before.
	TOptional<WoTSound::FChannelSnapshot> NewSoundChannel(
		const TArray<WoTSound::FChannelSnapshot>& Before,
		const TArray<WoTSound::FChannelSnapshot>& After)
	{
		for (const WoTSound::FChannelSnapshot& Snap : After)
		{
			if (!Before.ContainsByPredicate([&Snap](const WoTSound::FChannelSnapshot& Old)
				{
					return SameSoundChannel(Old, Snap);
				}))
			{
				return Snap;
			}
		}
		return {};
	}

	USoundBase* SoundParityWave(const TCHAR* SubPath)
	{
		return WoTSound::LoadBySubPath(FName(SubPath));
	}

	WoTSound::FPlayParams SoundParityPlay(USoundBase* Sound, const AActor* Source,
		WoTSound::ESlot Slot)
	{
		WoTSound::FPlayParams Params;
		Params.Sound = Sound;
		Params.Source = Source;
		Params.Location = Source ? Source->GetActorLocation() : FVector::ZeroVector;
		Params.Slot = Slot;
		return Params;
	}

	void TickSoundParity(UWorld* World, int32 Frames = 1)
	{
		WoTTest::TickWorld(World, Frames, 1.0f / 60.0f);
	}

	// Whether Source holds a sound whose wave name starts with Prefix in Slot.
	bool HoldsSound(UWorld* World, const UObject* Source, WoTSound::ESlot Slot,
		const TCHAR* Prefix)
	{
		for (const WoTSound::FChannelSnapshot& Snap : SoundChannelsOf(World, Source, Slot))
		{
			if (SoundNameStartsWith(Snap, Prefix))
			{
				return true;
			}
		}
		return false;
	}

	// The replicated PlaySound markers in World for a wave subpath.
	TArray<AWoTSoundBurst*> SoundBurstsFor(UWorld* World, FName SubPath)
	{
		TArray<AWoTSoundBurst*> Out;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It) && It->SoundSubPath == SubPath)
			{
				Out.Add(*It);
			}
		}
		return Out;
	}

#if WITH_EDITOR
	// Network specs run on payload-authorized worlds (as the ambience spec).
	struct FSoundParityNetActivation
	{
		bool bOwns = false;
		FSoundParityNetActivation()
		{
			if (!IsWoTPayloadBootstrapActive())
			{
				bOwns = !PublishWoTPayloadBootstrapActivationForTesting().IsError()
					&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
			}
		}
		~FSoundParityNetActivation()
		{
			if (bOwns)
			{
				ResetWoTPayloadActivationForTesting();
			}
		}
	};

	// A connected client's controller and its possessed, motionless player.
	bool SoundParityClientPlayer(UE::Net::FTestWorlds& Worlds, int32 ClientIndex,
		AWoTCharacter*& OutServerPawn, AWoTCharacter*& OutClientPawn)
	{
		const AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
			Worlds.GetServerPlayerControllerOfClient(static_cast<uint32>(ClientIndex)));
		OutServerPawn = ServerPC ? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
		UWorld* ClientWorld = Worlds.Clients[ClientIndex].GetWorld();
		OutClientPawn = nullptr;
		Worlds.TickUntil([&]()
		{
			const APlayerController* ClientPC = ClientWorld->GetFirstPlayerController();
			OutClientPawn = ClientPC ? Cast<AWoTCharacter>(ClientPC->GetPawn()) : nullptr;
			return OutClientPawn != nullptr;
		}, 120);
		if (!OutServerPawn || !OutClientPawn)
		{
			return false;
		}
		// The test map has no floor.
		for (AWoTCharacter* Pawn : {OutServerPawn, OutClientPawn})
		{
			Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Pawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		}
		return true;
	}
#endif
}

// Item 1: a new sound on the same actor and non-None slot replaces the old
// one; bNoOverride makes the newcomer yield; SLOT_None stacks; playing None
// on a slot stops it. Shipped tables put hurt and death cries in SLOT_Talk,
// footsteps in SLOT_Interact, and projectile launch and impact in SLOT_Misc.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParitySlotReplacementSpec,
	"WoT.Sound.Parity.SlotReplacement", WoTSoundParityFlags)
bool FWoTSoundParitySlotReplacementSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("listener"), SpawnSoundListener(World, FVector::ZeroVector)))
	{
		return false;
	}
	AWoTNPCCharacter* Creature = SpawnSoundCreature(World, FVector(300.0f, 0.0f, 0.0f));
	if (!TestNotNull(TEXT("creature with a shipped sound table"), Creature))
	{
		return false;
	}
	using WoTSound::ESlot;

	// The shipped table's hurt and death rows both name SLOT_Talk.
	TestTrue(TEXT("hurt cry plays"), Creature->PlayVoiceSlot(TEXT("HitHard")));
	TArray<WoTSound::FChannelSnapshot> Talk =
		SoundChannelsOf(World, Creature, ESlot::Talk);
	TestEqual(TEXT("the hurt cry holds the creature's talk slot"), Talk.Num(), 1);
	TestTrue(TEXT("death cry plays"), Creature->PlayVoiceSlot(TEXT("DieHard")));
	TestEqual(TEXT("the hurt cry no longer plays under the death cry"),
		SoundChannelsNamed(World, TEXT("Tro_HitHard")).Num(), 0);
	Talk = SoundChannelsOf(World, Creature, ESlot::Talk);
	if (TestEqual(TEXT("the death cry replaced the hurt cry"), Talk.Num(), 1))
	{
		TestTrue(FString::Printf(TEXT("talk slot holds the death cry (%s)"),
			*Talk[0].SoundName.ToString()), SoundNameStartsWith(Talk[0], TEXT("Tro_DieHard")));
	}

	// bNoOverride: the newcomer yields to the occupied slot.
	USoundBase* Hurt = SoundParityWave(TEXT("TrollocA/Tro_HitHard1"));
	if (!TestNotNull(TEXT("hurt wave loads"), Hurt))
	{
		return false;
	}
	WoTSound::FPlayParams Yield = SoundParityPlay(Hurt, Creature, ESlot::Talk);
	Yield.bNoOverride = true;
	WoTSound::PlaySound(World, Yield);
	Talk = SoundChannelsOf(World, Creature, ESlot::Talk);
	if (TestEqual(TEXT("a yielding sound leaves the slot alone"), Talk.Num(), 1))
	{
		TestTrue(TEXT("the death cry still plays"),
			SoundNameStartsWith(Talk[0], TEXT("Tro_DieHard")));
	}

	// A different slot on the same actor plays alongside.
	WoTSound::PlaySound(World, SoundParityPlay(Hurt, Creature, ESlot::Misc));
	TestEqual(TEXT("misc slot plays beside talk"),
		SoundChannelsOf(World, Creature, ESlot::Misc).Num(), 1);
	TestEqual(TEXT("talk slot unaffected by misc"),
		SoundChannelsOf(World, Creature, ESlot::Talk).Num(), 1);

	// SLOT_None stacks independently.
	WoTSound::PlaySound(World, SoundParityPlay(Hurt, Creature, ESlot::None));
	WoTSound::PlaySound(World, SoundParityPlay(Hurt, Creature, ESlot::None));
	TestEqual(TEXT("two SLOT_None sounds stack"),
		SoundChannelsOf(World, Creature, ESlot::None).Num(), 2);

	// PlaySound(None, Slot) stops that slot only.
	WoTSound::PlaySound(World, SoundParityPlay(nullptr, Creature, ESlot::Talk));
	TestEqual(TEXT("playing None stops the talk slot"),
		SoundChannelsOf(World, Creature, ESlot::Talk).Num(), 0);
	TestEqual(TEXT("the misc slot keeps playing"),
		SoundChannelsOf(World, Creature, ESlot::Misc).Num(), 1);

	// Footsteps ride SLOT_Interact (GenericTextureHelper.uc DetermineTextureEffect):
	// a running creature's steps replace each other, never its voice.
	if (AController* AI = Creature->GetController())
	{
		AI->UnPossess();
	}
	int32 MostSteps = 0;
	bool bSawStoneStep = false;
	for (int32 Frame = 0; Frame < 240; ++Frame)
	{
		const float Direction = (Frame / 30) % 2 == 0 ? 1.0f : -1.0f;
		Creature->GetCharacterMovement()->Velocity = FVector(300.0f * Direction, 0.0f, 0.0f);
		TickSoundParity(World);
		const TArray<WoTSound::FChannelSnapshot> Steps =
			SoundChannelsOf(World, Creature, ESlot::Interact);
		MostSteps = FMath::Max(MostSteps, Steps.Num());
		for (const WoTSound::FChannelSnapshot& Step : Steps)
		{
			bSawStoneStep |= SoundNameStartsWith(Step, TEXT("Stone"));
		}
	}
	TestTrue(TEXT("a running creature's steps play in its interact slot"), bSawStoneStep);
	TestEqual(TEXT("footsteps never stack in the interact slot"), MostSteps, 1);
	TestTrue(TEXT("death cry plays again"), Creature->PlayVoiceSlot(TEXT("DieHard")));
	TestEqual(TEXT("voice and steps use separate slots"),
		SoundChannelsOf(World, Creature, ESlot::Talk).Num(), 1);

	// GenericProjectile.uc: launch and impact both use PlaySound's default slot.
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(200.0f, 200.0f, 0.0f),
		FRotator::ZeroRotator, WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("projectile"), Dart))
	{
		return false;
	}
	const FObjectKey DartKey(Dart);
	TArray<WoTSound::FChannelSnapshot> Misc;
	for (const WoTSound::FChannelSnapshot& Snap : WoTSound::GetChannelsForTesting(*World))
	{
		if (Snap.SourceKey == DartKey && Snap.Slot == ESlot::Misc)
		{
			Misc.Add(Snap);
		}
	}
	if (TestEqual(TEXT("launch sound in the projectile's misc slot"), Misc.Num(), 1))
	{
		TestEqual(TEXT("launch wave"), Misc[0].SoundName, FName(TEXT("LaunchDT")));
	}
	Dart->Explode(Dart->GetActorLocation(), FVector::UpVector);
	Misc.Reset();
	for (const WoTSound::FChannelSnapshot& Snap : WoTSound::GetChannelsForTesting(*World))
	{
		if (Snap.SourceKey == DartKey && Snap.Slot == ESlot::Misc)
		{
			Misc.Add(Snap);
		}
	}
	if (TestEqual(TEXT("impact replaced the launch sound"), Misc.Num(), 1))
	{
		TestEqual(TEXT("impact wave"), Misc[0].SoundName, FName(TEXT("HitLevelDT")));
	}
	return !HasAnyErrors();
}

// Item 2: one-shot, ambient and interface sounds share the original's sixteen
// effects channels; a newcomer steals the lowest channel whose priority
// (volume x (1 - distance / radius)) does not exceed its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParitySharedVoiceBudgetSpec,
	"WoT.Sound.Parity.SharedVoiceBudget", WoTSoundParityFlags)
bool FWoTSoundParitySharedVoiceBudgetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Listener = SpawnSoundListener(World, FVector::ZeroVector, &Controller);
	if (!TestNotNull(TEXT("listener"), Listener))
	{
		return false;
	}
	using WoTSound::ESlot;
	USoundBase* Loop = WoTSound::LoadBySubPath(FName(TEXT("Angreal/LoopDT")), true);
	if (!TestNotNull(TEXT("looping wave loads"), Loop))
	{
		return false;
	}

	// Sixteen quiet one-shots from distinct sources: priority 1 x (1 - 800/1000).
	TArray<AActor*> Sources;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		const float Angle = 2.0f * PI * Index / WoTSound::EffectsChannelCount;
		AActor* Source = SpawnSoundSource(World,
			FVector(800.0f * FMath::Cos(Angle), 800.0f * FMath::Sin(Angle), 0.0f));
		Sources.Add(Source);
		WoTSound::FPlayParams Params = SoundParityPlay(Loop, Source, ESlot::Misc);
		Params.Radius = 1000.0f;
		WoTSound::PlaySound(World, Params);
	}
	TestEqual(TEXT("sixteen one-shots fill the effects channels"),
		CountSoundChannels(World), WoTSound::EffectsChannelCount);

	// A seventeenth below every active priority is rejected.
	{
		AActor* Quiet = SpawnSoundSource(World, FVector(0.0f, 800.0f, 0.0f));
		WoTSound::FPlayParams Params = SoundParityPlay(Loop, Quiet, ESlot::Misc);
		Params.Radius = 1000.0f;
		Params.Volume = 0.5f;
		WoTSound::PlaySound(World, Params);
		TestEqual(TEXT("a lower-priority one-shot finds no channel"),
			SoundChannelsOf(World, Quiet, ESlot::Misc).Num(), 0);
	}

	// A placed ambient loop near the listener competes for the same channels.
	AWoTAmbientSound* Emitter = World->SpawnActorDeferred<AWoTAmbientSound>(
		AWoTAmbientSound::StaticClass(), FTransform(FVector(100.0f, 0.0f, 0.0f)));
	Emitter->SoundSubPath = FName(TEXT("Angreal/LoopDT"));
	Emitter->SoundRadius = 64;
	Emitter->SoundVolume = 190;
	Emitter->FinishSpawning(FTransform(FVector(100.0f, 0.0f, 0.0f)));
	TickSoundParity(World);
	TArray<WoTSound::FChannelSnapshot> Ambient =
		SoundChannelsOf(World, Emitter->Ambient, ESlot::Ambient);
	TestEqual(TEXT("the ambient loop stole a channel"), Ambient.Num(), 1);
	TestEqual(TEXT("the budget stays sixteen"), CountSoundChannels(World),
		WoTSound::EffectsChannelCount);

	// Interface sounds (PlayerPawn.ClientPlaySound: four slots at volume 16)
	// enter the same budget and steal the four quietest channels.
	Controller->ClientPlayUISound(FName(TEXT("WOT/DefaultExpiring")), 1.0f);
	int32 InterfaceVoices = 0;
	for (const WoTSound::FChannelSnapshot& Snap : WoTSound::GetChannelsForTesting(*World))
	{
		InterfaceVoices += Snap.SoundName == FName(TEXT("DefaultExpiring")) ? 1 : 0;
	}
	TestEqual(TEXT("four interface voices hold channels"), InterfaceVoices,
		WoTSound::ClientUISoundVoiceCount);
	TestEqual(TEXT("the budget is still sixteen"), CountSoundChannels(World),
		WoTSound::EffectsChannelCount);
	TestEqual(TEXT("the louder ambient loop kept its channel"),
		SoundChannelsOf(World, Emitter->Ambient, ESlot::Ambient).Num(), 1);

	// Ambient loops stop outside their radius and restart on re-entry.
	Listener->SetActorLocation(FVector(5000.0f, 0.0f, 0.0f));
	TickSoundParity(World);
	TestEqual(TEXT("an out-of-range ambient loop gives its channel back"),
		SoundChannelsOf(World, Emitter->Ambient, ESlot::Ambient).Num(), 0);
	Listener->SetActorLocation(FVector::ZeroVector);
	TickSoundParity(World);
	TestEqual(TEXT("the ambient loop restarts in range"),
		SoundChannelsOf(World, Emitter->Ambient, ESlot::Ambient).Num(), 1);
	return !HasAnyErrors();
}

// Item 3: an actor-bound sound follows its actor every frame; when the actor
// is destroyed an ordinary sound finishes from the last location.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityMovingSourceSpec,
	"WoT.Sound.Parity.MovingSource", WoTSoundParityFlags)
bool FWoTSoundParityMovingSourceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("listener"), SpawnSoundListener(World, FVector::ZeroVector)))
	{
		return false;
	}
	AWoTNPCCharacter* Creature = SpawnSoundCreature(World, FVector(300.0f, 0.0f, 0.0f));
	if (!TestNotNull(TEXT("creature"), Creature))
	{
		return false;
	}
	using WoTSound::ESlot;
	TestTrue(TEXT("death cry plays"), Creature->PlayVoiceSlot(TEXT("DieHard")));
	const FVector Moved(500.0f, 250.0f, 40.0f);
	Creature->SetActorLocation(Moved);
	TickSoundParity(World);
	TArray<WoTSound::FChannelSnapshot> Cry = SoundChannelsNamed(World, TEXT("Tro_DieHard"));
	if (TestEqual(TEXT("the cry still plays"), Cry.Num(), 1))
	{
		TestTrue(FString::Printf(TEXT("the cry follows its creature (%s)"),
			*Cry[0].Location.ToString()),
			Cry[0].Location.Equals(Creature->GetActorLocation(), 1.0f));
		TestTrue(TEXT("the cry belongs to its creature's talk slot"),
			Cry[0].SourceKey == FObjectKey(Creature) && Cry[0].Slot == ESlot::Talk);
	}
	const FVector LastLocation = Creature->GetActorLocation();
	Creature->Destroy();
	TickSoundParity(World);
	TArray<WoTSound::FChannelSnapshot> Tail = SoundChannelsNamed(World, TEXT("Tro_DieHard"));
	if (TestEqual(TEXT("the tail outlives its creature"), Tail.Num(), 1))
	{
		TestTrue(FString::Printf(TEXT("the tail stays at the last location (%s)"),
			*Tail[0].Location.ToString()), Tail[0].Location.Equals(LastLocation, 1.0f));
	}

	// A looping sound follows its actor too.
	AActor* Mover = SpawnSoundSource(World, FVector(200.0f, 0.0f, 0.0f));
	USoundBase* Loop = WoTSound::LoadBySubPath(FName(TEXT("Angreal/LoopDT")), true);
	if (!TestNotNull(TEXT("loop loads"), Loop))
	{
		return false;
	}
	WoTSound::PlaySound(World, SoundParityPlay(Loop, Mover, ESlot::Misc));
	Mover->SetActorLocation(FVector(-300.0f, 100.0f, 0.0f));
	TickSoundParity(World);
	TArray<WoTSound::FChannelSnapshot> Moving = SoundChannelsNamed(World, TEXT("LoopDT"));
	if (TestEqual(TEXT("moving loop plays"), Moving.Num(), 1))
	{
		TestTrue(FString::Printf(TEXT("the moving loop follows its actor (%s)"),
			*Moving[0].Location.ToString()),
			Moving[0].Location.Equals(Mover->GetActorLocation(), 1.0f));
	}
	return !HasAnyErrors();
}

// Item 4: ambient loops take source-only Doppler, pitch x clamp(1 - radial
// speed / 6500, 0.5, 2); the listener's own motion does not count, and
// one-shots never shift.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityAmbientDopplerSpec,
	"WoT.Sound.Parity.AmbientDoppler", WoTSoundParityFlags)
bool FWoTSoundParityAmbientDopplerSpec::RunTest(const FString&)
{
	const FVector Listener = FVector::ZeroVector;
	const FVector Source(1000.0f, 0.0f, 0.0f);
	TestEqual(TEXT("approach at half the Doppler speed raises pitch 1.5x"),
		WoTSound::DopplerFactor(FVector(-3250.0f, 0.0f, 0.0f), Source, Listener),
		1.5f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("retreat at half the Doppler speed lowers pitch 0.5x"),
		WoTSound::DopplerFactor(FVector(3250.0f, 0.0f, 0.0f), Source, Listener),
		0.5f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("fast approach clamps at 2x"),
		WoTSound::DopplerFactor(FVector(-20000.0f, 0.0f, 0.0f), Source, Listener),
		2.0f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("fast retreat clamps at 0.5x"),
		WoTSound::DopplerFactor(FVector(20000.0f, 0.0f, 0.0f), Source, Listener),
		0.5f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("crossing motion has no radial component"),
		WoTSound::DopplerFactor(FVector(0.0f, 3000.0f, 0.0f), Source, Listener),
		1.0f, KINDA_SMALL_NUMBER);

	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTCharacter* Pawn = SpawnSoundListener(World, FVector::ZeroVector);
	if (!TestNotNull(TEXT("listener"), Pawn))
	{
		return false;
	}
	// A shipped projectile flying straight at the listener.
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(2000.0f, 0.0f, 0.0f),
		FRotator(0.0f, 180.0f, 0.0f), WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("projectile"), Dart)
		|| !TestNotNull(TEXT("projectile flight loop"), Dart->AmbientHum.Get()))
	{
		return false;
	}
	const float AuthoredPitch = static_cast<float>(Dart->SoundPitch) / 64.0f;
	TickSoundParity(World);
	const float Expected = AuthoredPitch * WoTSound::DopplerFactor(
		Dart->GetVelocity(), Dart->GetActorLocation(), Pawn->GetActorLocation());
	TestTrue(TEXT("the approaching projectile is pitched up"), Expected > AuthoredPitch);
	TestEqual(TEXT("flight loop pitch carries the source-only Doppler"),
		Dart->AmbientHum->PitchMultiplier, Expected, 0.01f);

	// The launch one-shot is not shifted.
	for (const WoTSound::FChannelSnapshot& Snap : WoTSound::GetChannelsForTesting(*World))
	{
		if (Snap.SoundName == FName(TEXT("LaunchDT")))
		{
			TestEqual(TEXT("one-shot pitch unchanged by motion"), Snap.Pitch,
				Dart->SpawnSoundPitch, KINDA_SMALL_NUMBER);
		}
	}
	return !HasAnyErrors();
}

// Item 5: the original progressively centred a sound inside 10% of its
// radius. UE's non-spatialized distance blend reproduces that transition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityCloseRangeCentringSpec,
	"WoT.Sound.Parity.CloseRangeCentring", WoTSoundParityFlags)
bool FWoTSoundParityCloseRangeCentringSpec::RunTest(const FString&)
{
	// UE's blend toward 2D (its distance spatialization): fully 3D at the start
	// distance, fully centred at the end distance, linear between.
	auto CentredAmount = [](const FSoundAttenuationSettings& S, float Distance)
	{
		if (S.NonSpatializedRadiusStart <= 0.0f || Distance <= 0.0f)
		{
			return S.NonSpatializedRadiusStart > 0.0f ? 1.0f : 0.0f;
		}
		const float End = FMath::Min(S.NonSpatializedRadiusStart, S.NonSpatializedRadiusEnd);
		if (End <= 0.0f || Distance >= S.NonSpatializedRadiusStart)
		{
			return 0.0f;
		}
		const float Range = FMath::Max(S.NonSpatializedRadiusStart - End, 1.0f);
		return FMath::Clamp((S.NonSpatializedRadiusStart - Distance) / Range, 0.0f, 1.0f);
	};
	for (const float Volume : {1.0f, 1.4f})
	{
		const USoundAttenuation* Att = WoTSound::LinearAttenuation(2000.0f, Volume);
		if (!TestNotNull(TEXT("attenuation"), Att))
		{
			return false;
		}
		const FSoundAttenuationSettings& S = Att->Attenuation;
		TestEqual(TEXT("centring begins at a tenth of the radius"),
			S.NonSpatializedRadiusStart, 200.0f, KINDA_SMALL_NUMBER);
		TestTrue(TEXT("centring ends at the listener"),
			S.NonSpatializedRadiusEnd > 0.0f && S.NonSpatializedRadiusEnd <= 1.0f);
		TestTrue(TEXT("centred sounds use the 2D speaker map"),
			S.NonSpatializedRadiusMode == ENonSpatializedRadiusSpeakerMapMode::Direct2D);
		TestEqual(TEXT("fully spatial at a tenth of the radius"),
			CentredAmount(S, 200.0f), 0.0f, KINDA_SMALL_NUMBER);
		TestEqual(TEXT("half centred at a twentieth"), CentredAmount(S, 100.0f), 0.5f, 0.01f);
		TestTrue(TEXT("nearly centred at the listener"), CentredAmount(S, 1.0f) > 0.99f);
		TestEqual(TEXT("unchanged beyond a tenth"), CentredAmount(S, 900.0f), 0.0f,
			KINDA_SMALL_NUMBER);
	}

	// Ambient loops centre against their own (possibly muffled) radius.
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTAmbientSound* Emitter = World->SpawnActorDeferred<AWoTAmbientSound>(
		AWoTAmbientSound::StaticClass(), FTransform::Identity);
	Emitter->SoundSubPath = FName(TEXT("Angreal/LoopDT"));
	Emitter->SoundRadius = 64;
	Emitter->FinishSpawning(FTransform::Identity);
	if (TestNotNull(TEXT("ambient attenuation"), Emitter->Ambient->AttenuationSettings.Get()))
	{
		TestEqual(TEXT("ambient centring at a tenth of 25 x (64 + 1)"),
			Emitter->Ambient->AttenuationSettings->Attenuation.NonSpatializedRadiusStart,
			162.5f, 0.01f);
	}
	return !HasAnyErrors();
}

// Item 6: the shipped WOTPlayer.uc ClientHearSound underwater rules.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityUnderwaterSpec,
	"WoT.Sound.Parity.Underwater", WoTSoundParityFlags)
bool FWoTSoundParityUnderwaterSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	using WoTSound::ESlot;
	// Water fills z in [-600, 0]; the listener stands dry above it.
	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		AWoTZoneVolume::StaticClass(), FVector(0.0f, 0.0f, -300.0f),
		FRotator::ZeroRotator, WoTSoundParitySpawn());
	Water->Collision->SetBoxExtent(FVector(3000.0f, 3000.0f, 300.0f));
	Water->bWaterZone = true;
	AWoTCharacter* Listener = SpawnSoundListener(World, FVector(0.0f, 0.0f, 300.0f));
	AWoTNPCCharacter* Creature = SpawnSoundCreature(World, FVector(400.0f, 0.0f, 0.0f));
	if (!TestNotNull(TEXT("listener"), Listener) || !TestNotNull(TEXT("creature"), Creature))
	{
		return false;
	}
	TickSoundParity(World);
	const float Eye = Creature->BaseEyeHeight;
	if (!TestTrue(TEXT("creature has an eye height"), Eye > 10.0f))
	{
		return false;
	}
	USoundBase* Hurt = SoundParityWave(TEXT("TrollocA/Tro_HitHard1"));
	if (!TestNotNull(TEXT("hurt wave"), Hurt))
	{
		return false;
	}
	auto PlayFrom = [&](USoundBase* Sound, const AActor* Source, ESlot Slot,
		float Radius) -> TOptional<WoTSound::FChannelSnapshot>
	{
		const TArray<WoTSound::FChannelSnapshot> Before =
			WoTSound::GetChannelsForTesting(*World);
		WoTSound::FPlayParams Params = SoundParityPlay(Sound, Source, Slot);
		Params.Radius = Radius;
		WoTSound::PlaySound(World, Params);
		return NewSoundChannel(Before, WoTSound::GetChannelsForTesting(*World));
	};

	// The source's HEAD decides: waist-deep (origin under, head above) is dry.
	Creature->SetActorLocation(FVector(400.0f, 0.0f, -0.5f * Eye));
	if (TOptional<WoTSound::FChannelSnapshot> Snap =
		PlayFrom(Hurt, Creature, ESlot::Misc, 1500.0f))
	{
		TestEqual(TEXT("waist-deep speaker is not muffled"), Snap->Pitch, 1.0f,
			KINDA_SMALL_NUMBER);
	}
	else
	{
		AddError(TEXT("waist-deep speaker's sound did not play"));
	}
	// Head under: volume 0.90, radius 0.80, pitch 0.85.
	Creature->SetActorLocation(FVector(400.0f, 0.0f, -2.0f * Eye));
	if (TOptional<WoTSound::FChannelSnapshot> Snap =
		PlayFrom(Hurt, Creature, ESlot::Misc, 1500.0f))
	{
		TestEqual(TEXT("submerged speaker volume x0.90"), Snap->Volume, 0.90f, KINDA_SMALL_NUMBER);
		TestEqual(TEXT("submerged speaker radius x0.80"), Snap->Radius, 1200.0f, 0.01f);
		TestEqual(TEXT("submerged speaker pitch x0.85"), Snap->Pitch, 0.85f, KINDA_SMALL_NUMBER);
	}
	else
	{
		AddError(TEXT("submerged speaker's sound did not play"));
	}
	// A raw radius of 0 stays 0 through x0.80 and only then becomes 1600.
	if (TOptional<WoTSound::FChannelSnapshot> Snap =
		PlayFrom(Hurt, Creature, ESlot::Talk, 0.0f))
	{
		TestEqual(TEXT("default radius applies after the underwater scale"),
			Snap->Radius, WoTSound::DefaultPlaySoundRadius, 0.01f);
		TestEqual(TEXT("talk slot is muffled"), Snap->Pitch, 0.85f, KINDA_SMALL_NUMBER);
	}
	else
	{
		AddError(TEXT("default-radius sound did not play"));
	}
	// SLOT_Interact and SLOT_Interface are exempt.
	for (const ESlot Exempt : {ESlot::Interact, ESlot::Interface})
	{
		if (TOptional<WoTSound::FChannelSnapshot> Snap =
			PlayFrom(Hurt, Creature, Exempt, 1500.0f))
		{
			TestEqual(TEXT("exempt slot keeps its pitch"), Snap->Pitch, 1.0f, KINDA_SMALL_NUMBER);
			TestEqual(TEXT("exempt slot keeps its radius"), Snap->Radius, 1500.0f, 0.01f);
		}
		else
		{
			AddError(TEXT("exempt-slot sound did not play"));
		}
	}
	// Drowning and gasping sounds are exempt by name.
	if (USoundBase* Drown = SoundParityWave(TEXT("AesSedaiA/Aes_Drown1")))
	{
		if (TOptional<WoTSound::FChannelSnapshot> Snap =
			PlayFrom(Drown, Creature, ESlot::Pain, 1500.0f))
		{
			TestEqual(TEXT("drowning sound keeps its pitch"), Snap->Pitch, 1.0f,
				KINDA_SMALL_NUMBER);
		}
		else
		{
			AddError(TEXT("drowning sound did not play"));
		}
	}

	// Sound-table radius multipliers scale the raw radius: an unset row radius
	// stays 0 and plays at 1600, not 1600 x the multiplier.
	Creature->SetActorLocation(FVector(400.0f, 0.0f, 300.0f));
	Creature->SoundRadiusMultiplier = 2.0f;
	const TArray<WoTSound::FChannelSnapshot> BeforeCry =
		WoTSound::GetChannelsForTesting(*World);
	TestTrue(TEXT("death cry plays"), Creature->PlayVoiceSlot(TEXT("DieHard")));
	if (TOptional<WoTSound::FChannelSnapshot> Cry =
		NewSoundChannel(BeforeCry, WoTSound::GetChannelsForTesting(*World)))
	{
		TestTrue(TEXT("the death cry plays"), SoundNameStartsWith(*Cry, TEXT("Tro_DieHard")));
		TestEqual(TEXT("unset row radius ignores the multiplier"), Cry->Radius,
			WoTSound::DefaultPlaySoundRadius, 0.01f);
		TestEqual(TEXT("a dry speaker is not muffled"), Cry->Pitch, 1.0f, KINDA_SMALL_NUMBER);
	}
	else
	{
		AddError(TEXT("the death cry did not play"));
	}
	return !HasAnyErrors();
}

// Item 7: DynamicAmbientSound.uc picks and plays on the server, so every
// player hears the same choice through the replicated PlaySound.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityServerChosenAmbienceSpec,
	"WoT.Sound.Parity.ServerChosenAmbience", WoTSoundParityFlags)
bool FWoTSoundParityServerChosenAmbienceSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	bool bOwnsPayloadActivation = false;
	if (!IsWoTPayloadBootstrapActive())
	{
		bOwnsPayloadActivation =
			!PublishWoTPayloadBootstrapActivationForTesting().IsError()
			&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
	}
	ON_SCOPE_EXIT
	{
		if (bOwnsPayloadActivation) ResetWoTPayloadActivationForTesting();
	};
	if (!TestTrue(TEXT("network worlds have authorized payload gameplay"),
		IsWoTBootstrapWorldReady())) return false;

	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("server-chosen random ambience"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	const APawn* ServerPawn = Worlds.GetServerPlayerControllerOfClient(0)->GetPawn();
	const FVector At = ServerPawn ? ServerPawn->GetActorLocation() : FVector::ZeroVector;
	const TArray<FName> Choices = {FName(TEXT("Angreal/LaunchDT")),
		FName(TEXT("Angreal/HitLevelDT")), FName(TEXT("Angreal/HitPawnDT"))};
	auto SpawnKeypoint = [&](UWorld& World)
	{
		AWoTDynamicAmbientSound* Keypoint =
			World.SpawnActorDeferred<AWoTDynamicAmbientSound>(
				AWoTDynamicAmbientSound::StaticClass(), FTransform(At));
		Keypoint->SoundSubPaths = Choices;
		Keypoint->bInitiallyOn = true;
		Keypoint->SoundRadius = 64;
		Keypoint->FinishSpawning(FTransform(At));
		return Keypoint;
	};
	// The keypoint is placed in the map, so it exists on every machine.
	AWoTDynamicAmbientSound* ServerKeypoint = SpawnKeypoint(*ServerWorld);
	AWoTDynamicAmbientSound* ClientKeypoint = SpawnKeypoint(*ClientWorld);
	if (!TestTrue(TEXT("the server picked a sound"),
		Choices.IsValidIndex(ServerKeypoint->LastSound)))
	{
		return false;
	}
	const FName Picked = Choices[ServerKeypoint->LastSound];
	Worlds.TickAll(30);
	TestEqual(TEXT("the client does not roll its own pick"), ClientKeypoint->LastSound, -1);
	bool bClientHeard = false;
	for (TActorIterator<AWoTSoundBurst> It(ClientWorld); It; ++It)
	{
		bClientHeard |= It->SoundSubPath == Picked;
	}
	TestTrue(FString::Printf(TEXT("the client receives the server's pick %s"),
		*Picked.ToString()), bClientHeard);
#endif
	return !HasAnyErrors();
}

// On a network client the original kept the sounds the server sent apart from
// the ones the client played itself (each machine named a slot by its own
// object index). A server-sent cast hum in the player's misc slot survives the
// player's interface sounds (a ClientPlayUISound, an artifact selection click),
// and a server-sent taunt in the talk slot survives a death message's notify.
// This machine's copy of a server play still shares the server's slot; a
// PlaySound in simulated code does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityClientOwnSlotsSpec,
	"WoT.Sound.Parity.ClientOwnSlots", WoTSoundParityFlags)
bool FWoTSoundParityClientOwnSlotsSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	FSoundParityNetActivation Activation;
	if (!TestTrue(TEXT("network worlds have authorized payload gameplay"),
		IsWoTBootstrapWorldReady())) return false;

	using namespace UE::Net;
	using WoTSound::ESlot;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("client-own sound slots"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* ServerPawn = nullptr;
	AWoTCharacter* ClientPawn = nullptr;
	if (!TestNotNull(TEXT("server controller"), ServerPC)
		|| !TestTrue(TEXT("the client possesses its player"),
			SoundParityClientPlayer(Worlds, 0, ServerPawn, ClientPawn)))
	{
		return false;
	}
	TestEqual(TEXT("the listener is a network client"),
		static_cast<int32>(ClientWorld->GetNetMode()), static_cast<int32>(NM_Client));
	auto ClientHolds = [&](ESlot Slot, const TCHAR* Prefix)
	{
		return HoldsSound(ClientWorld, ClientPawn, Slot, Prefix);
	};
	auto ClientCopies = [&](const TCHAR* Prefix)
	{
		return SoundChannelsNamed(ClientWorld, Prefix);
	};

	// A Lightning cast's hum: the server's PlaySound in the player's misc slot,
	// started afresh before each interface sound below. Each start waits for
	// its own marker, so an earlier one cannot land after the next event.
	const FName HumPath(TEXT("Angreal/LaunchLS"));
	TSet<FObjectKey> EarlierHums;
	auto StartHum = [&]()
	{
		for (const AWoTSoundBurst* Earlier : SoundBurstsFor(ClientWorld, HumPath))
		{
			EarlierHums.Add(FObjectKey(Earlier));
		}
		WoTSound::PlayReplicatedFrom(*ServerPawn, HumPath);
		return Worlds.TickUntil([&]()
		{
			for (const AWoTSoundBurst* Marker : SoundBurstsFor(ClientWorld, HumPath))
			{
				if (!EarlierHums.Contains(FObjectKey(Marker)))
				{
					return ClientHolds(ESlot::Misc, TEXT("LaunchLS"));
				}
			}
			return false;
		}, 120);
	};
	if (!TestTrue(TEXT("the client plays the server's hum in its player's misc slot"),
		StartHum()))
	{
		return false;
	}
	for (const WoTSound::FChannelSnapshot& Hum : ClientCopies(TEXT("LaunchLS")))
	{
		TestFalse(TEXT("a server-sent sound takes the server's slot"), Hum.bClientOwnSlot);
	}

	// ClientPlayUISound: four copies on the player, in None, Interface, Misc
	// and Talk.
	ServerPC->ClientPlayUISound(FName(TEXT("WOT/DefaultExpiring")), 1.0f);
	TestTrue(TEXT("the interface sound plays its four copies on the client"),
		Worlds.TickUntil([&]()
		{
			return ClientCopies(TEXT("DefaultExpiring")).Num()
				== WoTSound::ClientUISoundVoiceCount;
		}, 120));
	TestTrue(TEXT("the server's hum survives a ClientPlayUISound"),
		ClientHolds(ESlot::Misc, TEXT("LaunchLS")));
	TestTrue(TEXT("the interface sound's misc copy takes the client's own slot"),
		ClientCopies(TEXT("DefaultExpiring")).ContainsByPredicate(
			[&](const WoTSound::FChannelSnapshot& Snap)
			{
				return Snap.Slot == ESlot::Misc && Snap.bClientOwnSlot
					&& Snap.SourceKey == FObjectKey(ClientPawn);
			}));

	// An artifact selection click: WOTPlayer.PerformSelection's ClientPlaySound.
	AWoTAngrealInvFireball* Fireball = ServerWorld->SpawnActor<AWoTAngrealInvFireball>(
		WoTSoundParitySpawn());
	AWoTAngrealInvSeeker* Seeker = ServerWorld->SpawnActor<AWoTAngrealInvSeeker>(
		WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("first artifact"), Fireball)
		|| !TestNotNull(TEXT("second artifact"), Seeker))
	{
		return false;
	}
	Fireball->GiveTo(ServerPawn);
	Seeker->GiveTo(ServerPawn);
	const UWoTInventoryComponent* ServerInventory = ServerPawn->InventoryComponent;
	UWoTInventoryComponent* ClientInventory = ClientPawn->InventoryComponent;
	if (!TestTrue(TEXT("the client holds the artifacts with one selected"),
		Worlds.TickUntil([&]()
		{
			return ClientInventory && ServerInventory
				&& ClientInventory->Items.Num() == ServerInventory->Items.Num()
				&& IsValid(ClientInventory->SelectedItem);
		}, 180)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the hum plays before the selection click"), StartHum()))
	{
		return false;
	}
	ClientPawn->SelectHand(1);
	const int32 Clicks = ClientCopies(TEXT("SelectHand")).Num()
		+ ClientCopies(TEXT("SelectArtifact")).Num();
	TestTrue(FString::Printf(TEXT("the selection click plays on the client (%d copies)"),
		Clicks), Clicks > 0);
	TestTrue(TEXT("the server's hum survives an artifact selection click"),
		ClientHolds(ESlot::Misc, TEXT("LaunchLS")));

	// A taunt: the server's PlaySound in the player's talk slot.
	WoTSound::PlayReplicatedFrom(*ServerPawn, FName(TEXT("AesSedaiA/Aes_Taunt1")),
		ESlot::Talk);
	if (!TestTrue(TEXT("the client plays the server's taunt in its player's talk slot"),
		Worlds.TickUntil([&]() { return ClientHolds(ESlot::Talk, TEXT("Aes_Taunt1")); },
			120))
		|| !TestTrue(TEXT("the hum plays before the death message"), StartHum()))
	{
		return false;
	}
	// A kill elsewhere: every player's HUD posts the death message with its
	// notify sound (BaseHUD's RightNotify through ClientPlaySound).
	ServerPC->ClientTypedMessage(TEXT("Someone was killed."), FName(TEXT("WOTDeathMessage")));
	TestTrue(TEXT("the death message plays its notify on the client"),
		Worlds.TickUntil([&]() { return ClientCopies(TEXT("RightNotify")).Num() > 0; }, 120));
	TestTrue(TEXT("the server's taunt survives a death message"),
		ClientHolds(ESlot::Talk, TEXT("Aes_Taunt1")));
	TestTrue(TEXT("the server's hum survives a death message"),
		ClientHolds(ESlot::Misc, TEXT("LaunchLS")));

	// A PlaySound in simulated code is the client's own; this machine's copy of
	// a server play (an impact, say) shares the server's slot and replaces.
	USoundBase* Hurt = SoundParityWave(TEXT("AesSedaiA/Aes_HitHard1"));
	USoundBase* Hit = SoundParityWave(TEXT("AesSedaiA/Aes_HitSoft1"));
	if (!TestNotNull(TEXT("simulated wave"), Hurt) || !TestNotNull(TEXT("copy wave"), Hit)
		|| !TestTrue(TEXT("the hum plays before the client's own plays"), StartHum()))
	{
		return false;
	}
	WoTSound::PlaySimulatedFrom(*ClientPawn, Hurt, ESlot::Misc);
	TestTrue(TEXT("a simulated play leaves the server's hum alone"),
		ClientHolds(ESlot::Misc, TEXT("LaunchLS")));
	WoTSound::PlayFrom(*ClientPawn, Hit, ESlot::Misc);
	TestFalse(TEXT("a copy of a server play replaces the server's hum"),
		ClientHolds(ESlot::Misc, TEXT("LaunchLS")));
	TestTrue(TEXT("the copy holds the server's misc slot"),
		ClientHolds(ESlot::Misc, TEXT("Aes_HitSoft1")));
#endif
	return !HasAnyErrors();
}

// A listen host and a standalone game are one machine with one set of slots,
// as in the original: a same-slot interface or simulated sound still replaces
// the player's server-played sound there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityHostSharedSlotsSpec,
	"WoT.Sound.Parity.HostSharedSlots", WoTSoundParityFlags)
bool FWoTSoundParityHostSharedSlotsSpec::RunTest(const FString&)
{
	using WoTSound::ESlot;
	auto CheckSharedSlots = [this](UWorld* World, const TCHAR* Host)
	{
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* Pawn = SpawnSoundListener(World, FVector::ZeroVector, &Controller);
		if (!TestNotNull(*FString::Printf(TEXT("%s player"), Host), Pawn))
		{
			return;
		}
		WoTSound::PlayReplicatedFrom(*Pawn, FName(TEXT("Angreal/LaunchLS")));
		TestTrue(*FString::Printf(TEXT("%s: the hum holds the player's misc slot"), Host),
			HoldsSound(World, Pawn, ESlot::Misc, TEXT("LaunchLS")));
		// PlayerPawn.ClientPlaySound on this machine (the body of ClientPlayUISound).
		WoTSound::PlayClientSound(*Controller, SoundParityWave(TEXT("WOT/DefaultExpiring")));
		TestFalse(*FString::Printf(TEXT("%s: the interface copy replaces the hum"), Host),
			HoldsSound(World, Pawn, ESlot::Misc, TEXT("LaunchLS")));
		TestTrue(*FString::Printf(TEXT("%s: the misc slot holds the interface copy"), Host),
			HoldsSound(World, Pawn, ESlot::Misc, TEXT("DefaultExpiring")));

		WoTSound::PlayReplicatedFrom(*Pawn, FName(TEXT("AesSedaiA/Aes_Taunt1")), ESlot::Talk);
		TestTrue(*FString::Printf(TEXT("%s: the taunt holds the player's talk slot"), Host),
			HoldsSound(World, Pawn, ESlot::Talk, TEXT("Aes_Taunt1")));
		WoTSound::PlaySimulatedFrom(*Pawn,
			SoundParityWave(TEXT("AesSedaiA/Aes_HitHard1")), ESlot::Talk);
		TestFalse(*FString::Printf(TEXT("%s: a simulated play replaces the taunt"), Host),
			HoldsSound(World, Pawn, ESlot::Talk, TEXT("Aes_Taunt1")));
	};

	{
		UWorld* World = WoTTest::CreateSpecWorld();
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		TestEqual(TEXT("standalone net mode"),
			static_cast<int32>(World->GetNetMode()), static_cast<int32>(NM_Standalone));
		CheckSharedSlots(World, TEXT("standalone"));
	}
#if WITH_EDITOR
	{
		WoTTest::FSpecWorldOptions Options;
		Options.BeforeInitializeActorsForPlay = [](UWorld* World)
		{
			World->WorldType = EWorldType::PIE;
			World->SetPlayInEditorInitialNetMode(NM_ListenServer);
		};
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		TestEqual(TEXT("listen host net mode"),
			static_cast<int32>(World->GetNetMode()), static_cast<int32>(NM_ListenServer));
		CheckSharedSlots(World, TEXT("listen host"));
	}
#endif
	return !HasAnyErrors();
}

// WOTCarcass.uc ChunkUp: SourceActor.PlaySound(None, SLOT_Talk) on the server,
// so a corpse gibbed while its pawn's death cry still plays cuts the cry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityCarcassGibCutsCrySpec,
	"WoT.Sound.Parity.CarcassGibCutsCry", WoTSoundParityFlags)
bool FWoTSoundParityCarcassGibCutsCrySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("listener"), SpawnSoundListener(World, FVector::ZeroVector)))
	{
		return false;
	}
	AWoTNPCCharacter* Creature = SpawnSoundCreature(World, FVector(300.0f, 0.0f, 0.0f));
	USoundBase* Loop = WoTSound::LoadBySubPath(FName(TEXT("Angreal/LoopDT")), true);
	if (!TestNotNull(TEXT("creature"), Creature) || !TestNotNull(TEXT("loop"), Loop))
	{
		return false;
	}
	using WoTSound::ESlot;
	TestTrue(TEXT("death cry plays"), Creature->PlayVoiceSlot(TEXT("DieHard")));
	TestTrue(TEXT("the cry holds the creature's talk slot"),
		HoldsSound(World, Creature, ESlot::Talk, TEXT("Tro_DieHard")));
	WoTSound::PlaySound(World, SoundParityPlay(Loop, Creature, ESlot::Misc));

	// The swap: the corpse links to its pawn, which hides and lingers.
	AWoTCarcass* Corpse = World->SpawnActor<AWoTCarcass>(Creature->GetActorLocation(),
		FRotator::ZeroRotator, WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("corpse"), Corpse))
	{
		return false;
	}
	Corpse->InitFor(*Creature);
	Creature->SetActorHiddenInGame(true);
	TestTrue(TEXT("the cry plays on over the fresh corpse"),
		HoldsSound(World, Creature, ESlot::Talk, TEXT("Tro_DieHard")));

	// Enough damage gibs it.
	FWoTDamageEvent Hit;
	Hit.Damage = 500;
	Hit.WoTDamageType = FName(TEXT("Exploded"));
	Hit.HitInfo.ImpactPoint = Corpse->GetActorLocation();
	Corpse->TakeDamage(500.0f, Hit, nullptr, nullptr);
	TestTrue(TEXT("the corpse gibbed"), !IsValid(Corpse) || Corpse->WasChunkedUp());
	TestFalse(TEXT("the gib cut the death cry"),
		HoldsSound(World, Creature, ESlot::Talk, TEXT("Tro_DieHard")));
	TestTrue(TEXT("only the talk slot stops"),
		HoldsSound(World, Creature, ESlot::Misc, TEXT("LoopDT")));
	bool bServerStop = false;
	for (const AWoTSoundBurst* Burst : SoundBurstsFor(World, NAME_None))
	{
		bServerStop |= Burst->LocalSource.Get() == Creature
			&& Burst->Slot == static_cast<uint8>(ESlot::Talk);
	}
	TestTrue(TEXT("the stop is a server PlaySound heard by every player in range"),
		bServerStop);
	return !HasAnyErrors();
}

// WOTPlayer.uc NotifyAutoSwitch: Fire on an empty selection picks an item and
// the server plays WOT.Effect.SelectNewItem from the player, so every player in
// range hears it, not only the one who pressed Fire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityAutoSwitchHeardByAllSpec,
	"WoT.Sound.Parity.AutoSwitchHeardByAll", WoTSoundParityFlags)
bool FWoTSoundParityAutoSwitchHeardByAllSpec::RunTest(const FString&)
{
	const FName SelectNewItem(TEXT("WOT/SelectNewItem"));
	// The Fire path on a standalone player.
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* Pawn = SpawnSoundListener(World, FVector::ZeroVector, &Controller);
		AWoTAngrealInvFireball* Emptied = World->SpawnActor<AWoTAngrealInvFireball>(
			WoTSoundParitySpawn());
		AWoTAngrealInvSeeker* Next = World->SpawnActor<AWoTAngrealInvSeeker>(
			WoTSoundParitySpawn());
		if (!TestNotNull(TEXT("player"), Pawn) || !TestNotNull(TEXT("artifact"), Emptied)
			|| !TestNotNull(TEXT("next artifact"), Next))
		{
			return false;
		}
		Emptied->GiveTo(Pawn);
		Next->GiveTo(Pawn);
		Pawn->InventoryComponent->SelectItem(Emptied);
		Pawn->ReconcileHandSet();
		Pawn->InventoryComponent->DeleteInventory(Emptied);
		Emptied->Destroy();
		Pawn->UseAngreal();
		TestTrue(TEXT("Fire on the emptied selection selects the next artifact"),
			Pawn->InventoryComponent->SelectedItem == Next);
		const TArray<AWoTSoundBurst*> Plays = SoundBurstsFor(World, SelectNewItem);
		if (TestEqual(TEXT("the auto-switch sound is one server PlaySound"), Plays.Num(), 1))
		{
			TestTrue(TEXT("the player plays it"), Plays[0]->LocalSource.Get() == Pawn);
			TestEqual(TEXT("in its default slot"), static_cast<int32>(Plays[0]->Slot),
				static_cast<int32>(WoTSound::ESlot::Misc));
			TestEqual(TEXT("at the default radius"), Plays[0]->Radius, 0.0f);
		}
	}
#if WITH_EDITOR
	// A client's Fire: the other player beside it hears the sound too.
	FSoundParityNetActivation Activation;
	if (!TestTrue(TEXT("network worlds have authorized payload gameplay"),
		IsWoTBootstrapWorldReady())) return false;
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("auto-switch heard by every player in range"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	// Two starts 300 units apart, well inside the sound's hearing radius.
	for (const float Y : {-150.0f, 150.0f})
	{
		Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(FVector(0.0f, Y, 100.0f),
			FRotator::ZeroRotator, WoTSoundParitySpawn());
	}
	if (!TestTrue(TEXT("selecting player connects"), Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("listening player connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	UWorld* ListeningWorld = Worlds.Clients[1].GetWorld();
	AWoTCharacter* ServerPawn = nullptr;
	AWoTCharacter* ClientPawn = nullptr;
	AWoTCharacter* ServerListener = nullptr;
	AWoTCharacter* ClientListener = nullptr;
	if (!TestTrue(TEXT("the selecting player is in play"),
			SoundParityClientPlayer(Worlds, 0, ServerPawn, ClientPawn))
		|| !TestTrue(TEXT("the listening player is in play"),
			SoundParityClientPlayer(Worlds, 1, ServerListener, ClientListener)))
	{
		return false;
	}
	AWoTAngrealInvFireball* Emptied = ServerWorld->SpawnActor<AWoTAngrealInvFireball>(
		WoTSoundParitySpawn());
	AWoTAngrealInvSeeker* Next = ServerWorld->SpawnActor<AWoTAngrealInvSeeker>(
		WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("artifact"), Emptied) || !TestNotNull(TEXT("next artifact"), Next))
	{
		return false;
	}
	Emptied->GiveTo(ServerPawn);
	Next->GiveTo(ServerPawn);
	UWoTInventoryComponent* ServerInventory = ServerPawn->InventoryComponent;
	ServerInventory->SelectItem(Emptied);
	UWoTInventoryComponent* ClientInventory = ClientPawn->InventoryComponent;
	if (!TestTrue(TEXT("the selecting client holds the selected artifact"),
		Worlds.TickUntil([&]()
		{
			return ClientInventory->Items.Num() == ServerInventory->Items.Num()
				&& Cast<AWoTAngrealInvFireball>(ClientInventory->SelectedItem.Get());
		}, 180)))
	{
		return false;
	}
	ClientPawn->ReconcileHandSet();
	ServerInventory->DeleteInventory(Emptied);
	Emptied->Destroy();
	if (!TestTrue(TEXT("the selection empties on the client"),
		Worlds.TickUntil([&]()
		{
			return ClientInventory->Items.Num() == ServerInventory->Items.Num()
				&& !IsValid(ClientInventory->SelectedItem);
		}, 180)))
	{
		return false;
	}
	AWoTCharacter* SelectorSeenByListener =
		Worlds.FindReplicatedObjectOnClient(ServerPawn, 1);
	if (!TestNotNull(TEXT("the listener sees the selecting player"), SelectorSeenByListener))
	{
		return false;
	}
	ClientPawn->UseAngreal();
	TestTrue(TEXT("the other player hears the auto-switch from the selecting player"),
		Worlds.TickUntil([&]()
		{
			for (const AWoTSoundBurst* Play : SoundBurstsFor(ListeningWorld, SelectNewItem))
			{
				if (Play->Source.Get() == SelectorSeenByListener)
				{
					return true;
				}
			}
			return false;
		}, 120));
#endif
	return !HasAnyErrors();
}

// Engine Fragment's Flying.ZoneChange (the shipped bytecode): entering water
// slows the fragment to a fifth and plays the zone's EntrySound in
// SLOT_Interact at the splash size 0.0005 * (250 - 0.5 * Velocity.Z).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityFragmentSplashSpec,
	"WoT.Sound.Parity.FragmentSplashVolume", WoTSoundParityFlags)
bool FWoTSoundParityFragmentSplashSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	// A dry listener above the pool (the water spans 1000 units each way).
	if (!TestNotNull(TEXT("listener"),
		SpawnSoundListener(World, FVector(-1300.0f, 0.0f, 1200.0f))))
	{
		return false;
	}
	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>();
	Water->Collision->SetBoxExtent(FVector(1000.0f));
	Water->bWaterZone = true;
	Water->EntrySoundSubPath = FName(TEXT("AmbientA/DIVE"));
	AWoTFragment* Fragment = World->SpawnActor<AWoTFragment>(AWoTFragment::StaticClass(),
		FVector(-1100.0f, 0.0f, 200.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("fragment"), Fragment))
	{
		return false;
	}
	Fragment->Init(EWoTFragmentType::Wall, 200.0f, FVector::ZeroVector, 1.0f, NAME_None);
	Fragment->SetFragmentVelocity(FVector(600.0f, 0.0f, -400.0f));
	for (int32 Frame = 0; Frame < 120 && !Fragment->WasWaterDamped(); ++Frame)
	{
		TickSoundParity(World);
	}
	if (!TestTrue(TEXT("the fragment entered the water"), Fragment->WasWaterDamped()))
	{
		return false;
	}
	const double SlowedZ = Fragment->GetFragmentVelocity().Z;
	const float SplashSize = 0.0005f * (250.0f - 0.5f * static_cast<float>(SlowedZ));
	TestTrue(FString::Printf(TEXT("a falling fragment's splash is about 0.1-0.2 (%.4f)"),
		SplashSize), SplashSize > 0.1f && SplashSize < 0.2f);
	const TArray<AWoTSoundBurst*> Splashes = SoundBurstsFor(World, Water->EntrySoundSubPath);
	if (TestEqual(TEXT("the entry sound plays once"), Splashes.Num(), 1))
	{
		TestEqual(TEXT("at the splash size"), Splashes[0]->Volume, SplashSize, 1.0e-4f);
		TestEqual(TEXT("in the interact slot"), static_cast<int32>(Splashes[0]->Slot),
			static_cast<int32>(WoTSound::ESlot::Interact));
	}
	for (const WoTSound::FChannelSnapshot& Snap : SoundChannelsNamed(World, TEXT("DIVE")))
	{
		TestEqual(TEXT("the splash voice plays at the splash size"), Snap.Volume,
			SplashSize, 1.0e-4f);
	}
	return !HasAnyErrors();
}

// AngrealFireworksProjectile.uc plays its launch at volume 100 within 3200 and
// its burst at volume 100 within 12000. Each voice is cut to full scale after
// its distance fade, so both stay at full scale out to 99% of their radius;
// UE's 4x cap on a voice's own gain must not shrink that.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityFireworksReachSpec,
	"WoT.Sound.Parity.FireworksReach", WoTSoundParityFlags)
bool FWoTSoundParityFireworksReachSpec::RunTest(const FString&)
{
	// The heard gain is the voice volume times the curve.
	constexpr float Burst = AWoTFireworksProjectile::ExplosionSoundRadius;
	const USoundAttenuation* Att = WoTSound::LinearAttenuation(Burst, 100.0f);
	if (!TestNotNull(TEXT("burst attenuation"), Att))
	{
		return false;
	}
	for (const float Distance : {0.0f, 6000.0f, 11880.0f, 11940.0f, 12000.0f, 13000.0f})
	{
		const float Heard = WoTSound::VoiceVolume(100.0f) * Att->Attenuation.Evaluate(
			FTransform::Identity, FVector(Distance, 0.0f, 0.0f));
		const float Expected = FMath::Clamp(100.0f * (1.0f - Distance / Burst), 0.0f, 1.0f);
		TestEqual(*FString::Printf(TEXT("volume 100 at %.0f: Clamp(100*(1-d/R),0,1)"), Distance),
			Heard, Expected, 1.0e-3f);
	}
	TestTrue(TEXT("the voice stays within the engine's gain cap"),
		WoTSound::VoiceVolume(100.0f) <= 4.0f);

	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("listener"), SpawnSoundListener(World, FVector::ZeroVector)))
	{
		return false;
	}
	using WoTSound::ESlot;
	AWoTFireworksProjectile* Near = World->SpawnActor<AWoTFireworksProjectile>(
		FVector(1000.0f, 0.0f, 0.0f), FRotator::ZeroRotator, WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("near fireworks"), Near))
	{
		return false;
	}
	const TArray<WoTSound::FChannelSnapshot> Launch = SoundChannelsOf(World, Near, ESlot::Misc);
	if (TestEqual(TEXT("the launch plays in the projectile's slot"), Launch.Num(), 1))
	{
		TestEqual(TEXT("launch volume 100"), Launch[0].Volume, 100.0f, 1.0e-3f);
		TestEqual(TEXT("launch radius 3200"), Launch[0].Radius, 3200.0f, 1.0e-3f);
	}
	// Past the launch's reach but inside the burst's (12000 / sqrt(1.3)).
	AWoTFireworksProjectile* Far = World->SpawnActor<AWoTFireworksProjectile>(
		FVector(9000.0f, 0.0f, 0.0f), FRotator::ZeroRotator, WoTSoundParitySpawn());
	if (!TestNotNull(TEXT("far fireworks"), Far))
	{
		return false;
	}
	TestEqual(TEXT("a launch 9000 away is out of hearing"),
		SoundChannelsOf(World, Far, ESlot::Misc).Num(), 0);
	Far->Explode(Far->GetActorLocation(), FVector::UpVector);
	const TArray<WoTSound::FChannelSnapshot> Bang = SoundChannelsOf(World, Far, ESlot::Misc);
	if (TestEqual(TEXT("the burst is heard 9000 away"), Bang.Num(), 1))
	{
		TestEqual(TEXT("burst volume 100"), Bang[0].Volume, 100.0f, 1.0e-3f);
		TestEqual(TEXT("burst radius 12000"), Bang[0].Radius, 12000.0f, 1.0e-3f);
	}
	return !HasAnyErrors();
}

// A sound whose engine voice never starts holds no channel: a loop the engine
// cannot start would otherwise keep a channel busy for ever, and an ambient
// loop would sit silent on one until its listener left its range.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundParityUnstartedVoiceSpec,
	"WoT.Sound.Parity.UnstartedVoiceFreesChannel", WoTSoundParityFlags)
bool FWoTSoundParityUnstartedVoiceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("listener"), SpawnSoundListener(World, FVector::ZeroVector)))
	{
		return false;
	}
	USoundBase* Loop = WoTSound::LoadBySubPath(FName(TEXT("Angreal/LoopDT")), true);
	if (!TestNotNull(TEXT("looping wave"), Loop))
	{
		return false;
	}
	// The engine starts no voice while the world's audio is off.
	World->bAllowAudioPlayback = false;
	TestNull(TEXT("no voice starts"),
		WoTSound::PlayOneShot(World, Loop, FVector(200.0f, 0.0f, 0.0f), 1000.0f));
	TickSoundParity(World, 30);
	TestEqual(TEXT("an unstarted sourceless loop holds no channel"),
		SoundChannelsNamed(World, TEXT("LoopDT")).Num(), 0);

	AWoTAmbientSound* Emitter = World->SpawnActorDeferred<AWoTAmbientSound>(
		AWoTAmbientSound::StaticClass(), FTransform(FVector(100.0f, 0.0f, 0.0f)));
	Emitter->SoundSubPath = FName(TEXT("Angreal/LoopDT"));
	Emitter->SoundRadius = 64;
	Emitter->FinishSpawning(FTransform(FVector(100.0f, 0.0f, 0.0f)));
	TickSoundParity(World, 5);
	TestEqual(TEXT("an ambient loop whose voice cannot start holds no channel"),
		SoundChannelsOf(World, Emitter->Ambient, WoTSound::ESlot::Ambient).Num(), 0);

	// Once the engine can start it, the loop takes a channel and plays.
	World->bAllowAudioPlayback = true;
	TickSoundParity(World);
	TestEqual(TEXT("the ambient loop takes a channel"),
		SoundChannelsOf(World, Emitter->Ambient, WoTSound::ESlot::Ambient).Num(), 1);
	TestTrue(TEXT("the ambient loop's voice plays"), Emitter->Ambient->IsPlaying());
	return !HasAnyErrors();
}

#endif   // WITH_AUTOMATION_TESTS
