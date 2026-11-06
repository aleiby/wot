#include "WoTBubbles.h"
#include "WoTRandom.h"

#include "WoTSpriteBillboard.h"
#include "WoTZoneVolume.h"

#include "Engine/World.h"

AWoTBubbles::AWoTBubbles()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
	SetCanBeDamaged(false);
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AWoTBubbles::BeginPlay()
{
	Super::BeginPlay();
	// MinBubbles=1 + Rand(MaxBubbles-MinBubbles+1) (BubbleSpawner.uc:66-70); the
	// first bubble spawns immediately, the rest at the 0.1s cadence.
	GroupSize = 1 + WoTRandom::RandHelper(8);
	NextBubbleTime = GetWorld()->GetTimeSeconds();
}

void AWoTBubbles::SpawnBubble()
{
	// SpawnBubble (BubbleSpawner.uc:130-151): base + uniform offsets on the
	// yaw-forward / perpendicular / Z axes; DrawScale 0.03-0.11; WOTBubble's own
	// BeginPlay rerolls texture (25% each), +0-50% scale, life 3-7s, buoyancy
	// 3.1-4.1 over mass 3 (WOTBubble.uc:25-55).
	const FVector Fwd = GetActorForwardVector();
	const FVector Right = GetActorRightVector();
	FBubble B;
	B.Position = GetActorLocation() + Fwd * (WoTRandom::FRand() * 5.0f) +
	             Right * WoTRandom::FRandRange(-5.0f, 5.0f) +
	             FVector(0.0f, 0.0f, WoTRandom::FRandRange(-5.0f, 10.0f));
	B.RiseAccel = 950.0f * (WoTRandom::FRandRange(3.1f, 4.1f) / 3.0f - 1.0f);
	B.DieAt = GetWorld()->GetTimeSeconds() + WoTRandom::FRandRange(3.0f, 7.0f);

	const float Scale =
	    (WoTRandom::FRand() * 0.08f + 0.03f) * (1.0f + WoTRandom::FRand() * 0.5f);
	const TCHAR* Textures[] = {TEXT("bubble1"), TEXT("bubble2"), TEXT("bubble3"),
	                           TEXT("bubble4")};
	UWoTSpriteBillboardComponent* Sprite =
	    NewObject<UWoTSpriteBillboardComponent>(this);
	Sprite->SetupAttachment(GetRootComponent());
	Sprite->RegisterComponent();
	Sprite->SetWorldLocation(B.Position);
	// UE1 sprite world size = texture size * DrawScale (32x32 -> ~1-5uu specks).
	TArray<FString> Frames = {Textures[WoTRandom::RandHelper(4)]};
	Sprite->SetSpriteSeries(TEXT("WOT"), Frames, 32.0f * Scale, 32.0f * Scale);
	B.Sprite = Sprite;
	Bubbles.Add(MoveTemp(B));
	++NumSpawned;
}

void AWoTBubbles::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	// The 0.1s spawn cadence (TimeBetweenBubbles) until the group is out.
	if (NumSpawned < GroupSize && Now >= NextBubbleTime)
	{
		SpawnBubble();
		NextBubbleTime = Now + 0.1f;
	}

	// Rise + kill: leaving water destroys a bubble (WOTBubble.ZoneChange), as
	// does its 3-7s life.
	for (int32 I = Bubbles.Num() - 1; I >= 0; --I)
	{
		FBubble& B = Bubbles[I];
		B.RiseSpeed += B.RiseAccel * DeltaSeconds;
		B.Position.Z += B.RiseSpeed * DeltaSeconds;
		if (Now >= B.DieAt || !AWoTZoneVolume::IsPointInWater(World, B.Position))
		{
			if (B.Sprite)
			{
				B.Sprite->DestroyComponent();
			}
			Bubbles.RemoveAtSwap(I);
			continue;
		}
		if (B.Sprite)
		{
			B.Sprite->SetWorldLocation(B.Position);
		}
	}

	// bDestroy=True: the spawner dies with its single group (BubbleSpawner.uc:95-99).
	if (NumSpawned >= GroupSize && Bubbles.Num() == 0)
	{
		Destroy();
	}
}

AWoTBubbleBurst::AWoTBubbleBurst()
{
	bReplicates = true;
	bNetTemporary = true;
	SetReplicatingMovement(false);
	NetPriority = 6.0f;
	SetCanBeDamaged(false);
	SetLifeSpan(3.0f);
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AWoTBubbleBurst::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer || World->bIsTearingDown)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	World->SpawnActor<AWoTBubbles>(AWoTBubbles::StaticClass(), GetActorLocation(),
	                               GetActorRotation(), Params);
}

AWoTBubbleBurst* WoTBubbles::SpawnBurst(UWorld& World, const FVector& Location,
                                        const FRotator& Rotation)
{
	if (World.GetNetMode() == NM_Client || World.bIsTearingDown)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World.SpawnActor<AWoTBubbleBurst>(AWoTBubbleBurst::StaticClass(),
	                                         Location, Rotation, Params);
}
