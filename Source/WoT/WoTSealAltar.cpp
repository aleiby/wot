#include "WoTSealAltar.h"

#include "WoTBattleGameMode.h"
#include "WoTBattleInfo.h"
#include "WoTCharacter.h"
#include "WoTEvents.h"
#include "WoTPlayerController.h"
#include "WoTPlayerState.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"
#include "WoTUE1Collision.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTSeal.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

AWoTSealAltar::AWoTSealAltar()
{
	// A placed KeyPoint needs a real world location: without a RootComponent an
	// AActor's GetActorLocation() is always origin (the importer places altars per
	// citadel, and NearestAltarTeam keys citadel ownership off altar position).
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	// KeyPoint default bHidden=True: retail altars are invisible markers, and the
	// trap deploy separation check (AnyActorsInArea) excludes HIDDEN actors — an
	// unhidden altar with the new touch capsule would block citadel deploys
	// around the altar (caught by the citadel suite when the capsule landed).
	SetHidden(true);
	// SealAltar defaults: bCollideActors, CollisionRadius 48 / Height 1 — the UE
	// capsule is only the broad phase; OnAltarOverlap restores the flat cap.
	Touch = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Touch"));
	Touch->SetupAttachment(RootComponent);
	Touch->InitCapsuleSize(48.0f,
		WoTUE1Collision::ContainingCapsuleHalfHeight(48.0f, 1.0f));
	Touch->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Touch->SetCollisionResponseToAllChannels(ECR_Ignore);
	Touch->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	for (int32 i = 0; i < 4; ++i)
	{
		Seals[i] = nullptr;
	}
}

void AWoTSealAltar::SetUE1CollisionSize(float Radius, float HalfHeight)
{
	UE1CollisionRadius = FMath::Max(0.0f, Radius);
	UE1CollisionHalfHeight = FMath::Max(0.0f, HalfHeight);
	if (Touch)
	{
		Touch->SetCapsuleSize(UE1CollisionRadius,
			WoTUE1Collision::ContainingCapsuleHalfHeight(
				UE1CollisionRadius, UE1CollisionHalfHeight), false);
	}
}

void AWoTSealAltar::BeginPlay()
{
	Super::BeginPlay();

	// SealAltar.PreBeginPlay (SealAltar.uc:22-28): assert(SealGoal <= ArrayCount(Seals))
	// — the goal must be reachable with the 4 seal slots. Retail hard-asserts; the port
	// ensures (non-fatal) so bad map data logs instead of killing a headless run.
	ensureMsgf(SealGoal <= 4,
		TEXT("%s: SealGoal %d exceeds the 4 seal slots (SealAltar.uc:26)"),
		*GetName(), SealGoal);

	// SealAltar.PostBeginPlay: locate the placed BattleInfo (AllActors -> break: take the first).
	if (TActorIterator<AWoTBattleInfo> It(GetWorld()); It)
	{
		Battle = *It;
	}
	if (!Battle)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s: unable to locate a BattleInfo in this level."), *GetName());
	}

	Touch->OnComponentBeginOverlap.AddDynamic(this, &AWoTSealAltar::OnAltarOverlap);
}

void AWoTSealAltar::OnAltarOverlap(UPrimitiveComponent*, AActor* Other,
                                   UPrimitiveComponent*, int32, bool,
                                   const FHitResult&)
{
	const auto Inside = [this](const AActor* Candidate)
		{
			return WoTUE1Collision::CylindersOverlap(GetActorLocation(),
				UE1CollisionRadius, UE1CollisionHalfHeight, Candidate);
		};
	if (!Inside(Other))
	{
		// In the containing capsule only (e.g. dropping onto the altar):
		// SealAltar.Touch runs on the frame the flat cylinders meet.
		UE1Touches.DeferOutside(*this, Touch, Other, Inside,
			[this](AActor* Candidate)
			{ OnAltarOverlap(Touch, Candidate, nullptr, 0, false, FHitResult()); });
		return;
	}
	if (APawn* Pawn = Cast<APawn>(Other))
	{
		TouchedByPawn(Pawn);
	}
}

void AWoTSealAltar::TouchedByPawn(APawn* Other)
{
	// Server-only, Active state only (Inactive debounces; GameEnded is dead).
	if (!HasAuthority() || bEnded || bInactive || !Other)
	{
		return;
	}
	// SealAltar.uc:214-217: only players interact — every NPC touch is ignored
	// (Player = WOTPlayer(Other)).
	AController* Controller = Other->GetController();
	if (!Controller || !Controller->IsPlayerController())
	{
		return;
	}

	UWoTInventoryComponent* Inv =
		Other->FindComponentByClass<UWoTInventoryComponent>();
	AWoTSeal* Carried = nullptr;
	if (Inv)
	{
		// Player.FindInventoryType(class'Seal') (SealAltar.uc:221).
		for (AWoTInventory* Item : Inv->Items)
		{
			if (AWoTSeal* S = Cast<AWoTSeal>(Item))
			{
				Carried = S;
				break;
			}
		}
	}

	UWoTEventSubsystem* Events =
		GetWorld() ? GetWorld()->GetSubsystem<UWoTEventSubsystem>() : nullptr;

	if (Carried)
	{
		// SealAltar.uc:224-231: the altar's own Event fires on EVERY carried-seal
		// touch — before the team gate can reject the placement.
		if (!EventName.IsNone() && Events)
		{
			Events->FireEvent(EventName, this, Other);
		}
		PlaceSeal(Carried);
		Carried->NotifyPawnsSealEvent(Carried->OwnershipChangedEvent, nullptr);
		if (CheckForWinner())
		{
			if (!WinEvent.IsNone() && Events)
			{
				Events->FireEvent(WinEvent, this, Other);
			}
			AnnounceWinningTeam();
		}
		else
		{
			// "de-bounce" seal handling behavior (SealAltar.uc:251).
			StartInactive();
		}
	}
	else
	{
		// SealAltar.uc:256-268: no carried seal — hand the last banked one over
		// (the seal's own pickup path; its collision is off on the altar, so the
		// ALTAR is the touch surface). ValidSealTouch folds into TryPickupBy's
		// pickup-query gate: a denied grant leaves the slot banked.
		if (AWoTSeal* Banked = FindSeal())
		{
			RemoveSeal(Banked);
			if (Banked->TryPickupBy(Other))
			{
				StartInactive();
			}
			else
			{
				// Denied (retail gates BEFORE removing; re-slot restores it).
				for (int32 i = 0; i < 4; ++i)
				{
					if (Seals[i] == nullptr)
					{
						Seals[i] = Banked;
						break;
					}
				}
			}
		}
	}
}

void AWoTSealAltar::StartInactive()
{
	bInactive = true;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(InactiveTimer, this,
			&AWoTSealAltar::EndInactive, 1.0f, false);
	}
}

void AWoTSealAltar::EndInactive()
{
	bInactive = false;
}

void AWoTSealAltar::Reset(int32 Count)
{
	// SealAltar.Reset: destroy banked seals, reset this team's count, set the new goal.
	for (int32 i = 0; i < 4; ++i)
	{
		if (Seals[i])
		{
			Seals[i]->Destroy();
			Seals[i] = nullptr;
		}
	}
	if (Battle)
	{
		Battle->ResetTeamSealCount(Team);
	}
	SealGoal = Count;
}

bool AWoTSealAltar::CheckForWinner() const
{
	// SealAltar.CheckForWinner: the team's banked count via BattleInfo, or (off a Battle game)
	// the count of seals physically on the altar.
	int32 Count = 0;
	if (Battle)
	{
		Count = Battle->GetTeamSealCount(Team);
	}
	else
	{
		for (int32 i = 0; i < 4; ++i)
		{
			if (Seals[i])
			{
				++Count;
			}
		}
	}
	return Count >= SealGoal;
}

float AWoTSealAltar::BankedSealOffset(int32 Rank, int32 Count) const
{
	// SealAltar.uc:112-114 spaces slot i at Seal.default.CollisionRadius *
	// (i*2 - (SealGoal-1)) along the altar's yaw: 34 uu apart, a 102 uu row for
	// the default 4-seal goal, filled from one end, so a partly filled altar
	// looks lopsided and on a default 48 uu altar (Battle2_02's 96 uu pedestal)
	// slot 0's centre sits past the edge. Divergence
	// (PORT_NOTES "Banked seals fit and center on the altar"): centre the seals
	// actually banked, and keep retail spacing while that row fits inside the
	// altar's collision radius, else compress it until each seal's mesh (about
	// half its collision radius wide) stays on the altar. Presentation only:
	// banked seals never collide and are handed back by the altar.
	constexpr float SealRadius = 17.0f;
	const float Fit = FMath::Max(0.0f, UE1CollisionRadius - SealRadius / 2);
	const float Spacing = Count > 1
		? FMath::Min(2.0f * SealRadius, 2.0f * Fit / (Count - 1)) : 0.0f;
	return Spacing * (Rank - (Count - 1) * 0.5f);
}

void AWoTSealAltar::LayoutBankedSeals()
{
	int32 Count = 0;
	for (const AWoTSeal* Seal : Seals)
	{
		Count += Seal ? 1 : 0;
	}
	// Seal.default.CollisionRadius = 17, Seal CollisionHeight = 3, altar
	// CollisionHeight = 1: retail rests the seal 1 + 3/2 uu above the altar actor
	// (SealAltar.uc:111-115). Mappers set that actor above the pedestal it marks,
	// so retail seals hover over it (Battle2_02 by ~20 uu). Seat them the same
	// 2.5 uu above the pedestal's top instead when world geometry lies within
	// MaxSeatDrop below the retail spot; otherwise keep the retail spot.
	constexpr float RestHeight = 1.0f + 3.0f / 2.0f;
	constexpr float SeatProbeAbove = 8.0f;
	constexpr float MaxSeatDrop = 32.0f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTSealAltarSeat), false, this);
	for (AWoTSeal* Seal : Seals)
	{
		if (Seal)
		{
			Params.AddIgnoredActor(Seal);
		}
	}
	const FVector Forward = GetActorRotation().Vector();
	UWorld* World = GetWorld();
	int32 Rank = 0;
	for (AWoTSeal* Seal : Seals)
	{
		if (!Seal)
		{
			continue;
		}
		FVector Spot = GetActorLocation() + RestHeight * FVector::UpVector
			+ Forward * BankedSealOffset(Rank++, Count);
		FHitResult Hit;
		if (World && World->LineTraceSingleByObjectType(Hit,
				Spot + FVector(0, 0, SeatProbeAbove), Spot - FVector(0, 0, MaxSeatDrop),
				FCollisionObjectQueryParams(ECC_WorldStatic), Params))
		{
			Spot.Z = Hit.ImpactPoint.Z + RestHeight;
		}
		Seal->SetActorLocation(Spot);
	}
}

void AWoTSealAltar::PlaceSeal(AWoTSeal* Seal, bool bForce)
{
	if (!Seal)
	{
		return;
	}

	// Player = WOTPlayer(Seal.Owner): the banking player is the seal's owning pawn (captured
	// before SetOwner(this) reassigns it).
	APawn* OwnerPawn = Cast<APawn>(Seal->GetOwner());
	AController* Placer = OwnerPawn ? OwnerPawn->GetController() : nullptr;
	AWoTPlayerState* PS = Placer ? Placer->GetPlayerState<AWoTPlayerState>() : nullptr;

	// Only accept seals from the altar's team, unless forced or team-agnostic (Seal.uc:98).
	if (!bForce && Team != 255 && (PS == nullptr || PS->Team != Team))
	{
		return;
	}

	// Fill the first empty slot (the original centers them left to right on the altar).
	for (int32 i = 0; i < 4; ++i)
	{
		if (Seals[i] == nullptr)
		{
			// SealAltar.uc:105-124: pull it from the placer's inventory, center it
			// on the altar, freeze it, and let the ALTAR hand it back later.
			if (Placer && OwnerPawn)
			{
				if (UWoTInventoryComponent* Inv =
					OwnerPawn->FindComponentByClass<UWoTInventoryComponent>())
				{
					Inv->DeleteInventory(Seal);
				}
			}
			// Altar CollisionHeight 1 + Seal CollisionHeight / 2 above the altar
			// (SealAltar.uc:111-115); LayoutBankedSeals sets the final spot below.
			Seal->RestOnAltar(GetActorLocation() + (1.0f + 3.0f / 2.0f) * FVector::UpVector,
				GetActorRotation(), this);

			if (Seal->ControllingTeam == Team)
			{
				// Own team re-banking: the altar plays the place sound (uc:128).
				// SealAltar.uc:128 PlaySound: the server's play, replicated to
				// players in range, volume 1, radius 1600.
				WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WOT/PlaceSeal")),
					GetActorLocation(), WoTSound::DefaultPlaySoundRadius);
			}
			else if (!bForce)
			{
				SendCaptureNotification(Placer);
			}
			Seal->SetControllingTeam(Placer);
			Seals[i] = Seal;
			LayoutBankedSeals();
			break;
		}
	}
}

void AWoTSealAltar::SendCaptureNotification(AController* Placer)
{
	// SealAltar.uc:143-155: every player hears the capture and reads
	// "<TeamDescription> captured a seal." (the placer pawn's live copy).
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FString TeamName;
	if (Placer)
	{
		if (const AWoTCharacter* Ch = Cast<AWoTCharacter>(Placer->GetPawn()))
		{
			TeamName = Ch->TeamDescription;
		}
	}
	const FString Message = TeamName + CaptureNotificationStr;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator();
	     It; ++It)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(It->Get()))
		{
			PC->ClientPlayUISound(FName(TEXT("WOT/CaptureSeal")), 1.0f);
			// CenterMessage(...,,true): the bEcho console copy (SealAltar.uc:152).
			PC->ClientCenterMessage(Message, /*bEcho=*/true);
		}
	}
}

AWoTSeal* AWoTSealAltar::FindSeal() const
{
	// SealAltar.uc:157-172: the LAST banked seal.
	for (int32 i = 3; i >= 0; --i)
	{
		if (Seals[i])
		{
			return Seals[i];
		}
	}
	return nullptr;
}

void AWoTSealAltar::RemoveSeal(AWoTSeal* Seal)
{
	for (int32 i = 3; i >= 0; --i)
	{
		if (Seals[i] == Seal)
		{
			Seals[i] = nullptr;
			LayoutBankedSeals();
			break;
		}
	}
}

void AWoTSealAltar::AnnounceWinningTeam()
{
	// SealAltar.AnnounceWinningTeam: freeze every altar (GameEnded state), record the winner on
	// the Battle mode, then end the game.
	for (TActorIterator<AWoTSealAltar> It(GetWorld()); It; ++It)
	{
		It->bEnded = true;
	}
	if (AWoTBattleGameMode* BattleMode = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr)
	{
		BattleMode->WinningTeam = Team;
	}
	if (AWoTGameMode* Game = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
	{
		Game->EndGame(FName(TEXT("AltarFilled")));
	}
}
