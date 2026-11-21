#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

// The owner play-tests the editor build in game mode on the editor-imported
// maps (UnrealEditor <proj> /Game/WoT/Maps/Arena_06 -game). These specs load
// that same asset through the engine's own LoadMap into a standalone game
// world, exactly as -game does, and check what the play-test found broken.
#if WITH_AUTOMATION_TESTS && WITH_EDITOR && !WOT_GUARDED_BOOTSTRAP

#include "../WoTCharacter.h"
#include "../WoTDecoration.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTGameInstance.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTTeleporter.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace WoTEditorPlaytest
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const TCHAR* Arena06 = TEXT("/Game/WoT/Maps/Arena_06");

class FLogCapture final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type,
		const FName&) override
	{
		Text += Message;
		Text += TEXT("\n");
	}
	FString Text;
};

// The editor-imported map loaded into its own game world context by
// UEngine::LoadMap, the path -game takes for a map on the command line.
struct FPlayWorld
{
	UGameInstance* GameInstance = nullptr;
	UWorld* World = nullptr;
	// LoadMap makes the loaded world GWorld; the editor's comes back after.
	UWorld* PriorGWorld = GWorld;
	FLogCapture Log;

	bool Load(FAutomationTestBase& Test, const TCHAR* Map)
	{
		if (!FPackageName::DoesPackageExist(Map))
		{
			Test.AddWarning(FString::Printf(
				TEXT("%s is not imported in this content tree; spec skipped"), Map));
			return false;
		}
		GameInstance = NewObject<UGameInstance>(GEngine, UWoTGameInstance::StaticClass());
		GameInstance->AddToRoot();
		GameInstance->InitializeStandalone();
		FWorldContext* Context = GameInstance->GetWorldContext();
		if (!Test.TestNotNull(TEXT("standalone game context"), Context))
		{
			return false;
		}
		GLog->AddOutputDevice(&Log);
		FString Error;
		const bool bLoaded = GEngine->LoadMap(
			*Context, FURL(nullptr, Map, TRAVEL_Absolute), nullptr, Error);
		GLog->FlushThreadedLogs();
		GLog->RemoveOutputDevice(&Log);
		World = Context->World();
		if (!Test.TestTrue(FString::Printf(TEXT("LoadMap %s (%s)"), Map, *Error), bLoaded)
			|| !Test.TestNotNull(TEXT("loaded game world"), World))
		{
			return false;
		}
		return Test.TestTrue(TEXT("the loaded world has begun play"), World->HasBegunPlay());
	}

	~FPlayWorld()
	{
		if (!GameInstance)
		{
			return;
		}
		if (World && World->HasBegunPlay())
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
		}
		GameInstance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		GameInstance->RemoveFromRoot();
		GWorld = PriorGWorld;
		// The map package is released, so the next spec loads it afresh.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
};

FActorSpawnParameters AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AWoTTeleporter* FindTeleporter(UWorld& World, const TCHAR* Url, const TCHAR* Tag)
{
	for (TActorIterator<AWoTTeleporter> It(&World); It; ++It)
	{
		if (It->URL == Url && It->TeleTag == FName(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

AWoTTeleporter* FindTeleporterByTag(UWorld& World, const TCHAR* Tag)
{
	for (TActorIterator<AWoTTeleporter> It(&World); It; ++It)
	{
		if (It->TeleTag == FName(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

FString TexName(UMaterialInterface* Material)
{
	UTexture* Texture = nullptr;
	if (Material && Material->GetTextureParameterValue(
			FHashedMaterialParameterInfo(FName(TEXT("Tex"))), Texture) && Texture)
	{
		return Texture->GetName();
	}
	return FString(TEXT("<none>"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEditorPlaytestArena06TeleporterSpec,
	"WoT.EditorPlaytest.Arena06.TeleporterReachesTele2", WoTEditorPlaytest::Flags)

bool FWoTEditorPlaytestArena06TeleporterSpec::RunTest(const FString&)
{
	using namespace WoTEditorPlaytest;
	FPlayWorld Play;
	if (!Play.Load(*this, Arena06))
	{
		return !HasAnyErrors();
	}
	UWorld& World = *Play.World;
	// TeleporterWithEffects1: URL tele2, Tag tele1, CollisionRadius and
	// CollisionHeight 80 in the shipped Arena_06 map. Its only destination is
	// TeleporterWithEffects2, Tag tele2.
	AWoTTeleporter* Source = FindTeleporter(World, TEXT("tele2"), TEXT("tele1"));
	AWoTTeleporter* Dest = FindTeleporterByTag(World, TEXT("tele2"));
	if (!TestNotNull(TEXT("TeleporterWithEffects1 (URL tele2, Tag tele1)"), Source)
		|| !TestNotNull(TEXT("the tele2 teleporter"), Dest))
	{
		return false;
	}
	TestEqual(TEXT("the pad touches at its authored 80 uu radius"),
		Source->UE1CollisionRadius, 80.0f);
	TestEqual(TEXT("the pad touches at its authored 80 uu height"),
		Source->UE1CollisionHalfHeight, 80.0f);
	const FVector Pad = Source->GetActorLocation();

	// The level floor under the pad (world geometry only).
	FHitResult FloorHit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(WoTEditorPlaytestFloor), false, Source);
	const bool bFloor = World.LineTraceSingleByObjectType(FloorHit, Pad,
		Pad - FVector(0, 0, 1000), FCollisionObjectQueryParams(ECC_WorldStatic), Query);
	if (!TestTrue(TEXT("the pad stands over the level floor"), bFloor))
	{
		return false;
	}

	// Approach from the open side of the alcove: the longest clear horizontal
	// ray from the pad at standing height.
	const FVector Floor = FloorHit.ImpactPoint;
	float BestDistance = -1.0f;
	FVector BestDirection = FVector::ForwardVector;
	for (int32 Step = 0; Step < 16; ++Step)
	{
		const FVector Direction = FRotator(0.0f, Step * 22.5f, 0.0f).Vector();
		const FVector Start = Floor + FVector(0, 0, 60);
		FHitResult Hit;
		FCollisionQueryParams WallQuery(SCENE_QUERY_STAT(WoTEditorPlaytestWall), false);
		WallQuery.AddIgnoredActor(Source);
		const bool bHit = World.LineTraceSingleByChannel(Hit, Start,
			Start + Direction * 600.0f, ECC_WorldStatic, WallQuery);
		const float Distance = bHit ? Hit.Distance : 600.0f;
		if (Distance > BestDistance)
		{
			BestDistance = Distance;
			BestDirection = Direction;
		}
	}
	AddInfo(FString::Printf(TEXT("open side %s (%.0f clear)"),
		*BestDirection.ToCompactString(), BestDistance));

	AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>(AlwaysSpawn());
	const FVector StartLocation = Floor + BestDirection * 260.0f + FVector(0, 0, 60);
	AWoTCharacter* Body = World.SpawnActor<AWoTCharacter>(
		StartLocation, BestDirection.Rotation() + FRotator(0, 180, 0), AlwaysSpawn());
	if (!TestNotNull(TEXT("player controller"), Controller)
		|| !TestNotNull(TEXT("player pawn"), Body))
	{
		return false;
	}
	Controller->InitPlayerState();
	if (AWoTGameMode* Game = World.GetAuthGameMode<AWoTGameMode>())
	{
		Game->ChangeTeam(Controller, 255);
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Body);
	UCharacterMovementComponent* Move = Body->GetCharacterMovement();
	Move->SetMovementMode(MOVE_Falling);
	constexpr float Step = 1.0f / 60.0f;
	WoTTest::TickWorld(&World, 1.0f, Step);
	AddInfo(FString::Printf(TEXT("player stands at %s (falling=%d)"),
		*Body->GetActorLocation().ToCompactString(), Move->IsFalling()));

	// Walk straight at the pad for up to four seconds.
	float Closest = TNumericLimits<float>::Max();
	bool bArrived = false;
	for (int32 Tick = 0; Tick < 240 && !bArrived; ++Tick)
	{
		const FVector ToPad = (Pad - Body->GetActorLocation()).GetSafeNormal2D();
		Body->AddMovementInput(ToPad, 1.0f, true);
		WoTTest::TickWorld(&World, 1, Step);
		Closest = FMath::Min(Closest, static_cast<float>(
			FVector::Dist2D(Body->GetActorLocation(), Pad)));
		bArrived = FVector::Dist2D(Body->GetActorLocation(), Dest->GetActorLocation()) < 100.0f;
	}
	AddInfo(FString::Printf(TEXT("closest approach to the pad %.1f; pawn now at %s"),
		Closest, *Body->GetActorLocation().ToCompactString()));
	TestTrue(FString::Printf(TEXT("the player walked into TeleporterWithEffects1 and arrived at tele2 %s (now %s)"),
		*Dest->GetActorLocation().ToCompactString(),
		*Body->GetActorLocation().ToCompactString()), bArrived);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEditorPlaytestArena06AnimatedTexturesSpec,
	"WoT.EditorPlaytest.Arena06.AnimatedTexturesLive", WoTEditorPlaytest::Flags)

bool FWoTEditorPlaytestArena06AnimatedTexturesSpec::RunTest(const FString&)
{
	using namespace WoTEditorPlaytest;
	FPlayWorld Play;
	if (!Play.Load(*this, Arena06))
	{
		return !HasAnyErrors();
	}
	UWorld& World = *Play.World;
	UWoTFractalSubsystem* Fractals = World.GetSubsystem<UWoTFractalSubsystem>();
	if (!TestNotNull(TEXT("fractal subsystem"), Fractals))
	{
		return false;
	}
	int32 Reported = INDEX_NONE;
	{
		const int32 At = Play.Log.Text.Find(TEXT("WOTFRACTAL: "));
		if (At != INDEX_NONE)
		{
			Reported = FCString::Atoi(*Play.Log.Text.Mid(At + 12, 8));
		}
		AddInfo(Play.Log.Text.Mid(At == INDEX_NONE ? 0 : At, 80));
	}
	TestTrue(FString::Printf(TEXT("WOTFRACTAL reports live sims (%d)"), Reported),
		Reported > 0);
	TestTrue(TEXT("the subsystem holds live simulations"),
		Fractals->GetActiveTextureCount() > 0);

	// Arena_06's procedural BSP surfaces: the fountain portal's
	// ScottT.S_ForFountain001, imported as modulated unlit sections
	// (MI_W_ScottT__S_ForFountain001_Mod_U and _Mod_2S_U).
	int32 Surfaces = 0;
	for (TActorIterator<AStaticMeshActor> It(&World); It; ++It)
	{
		UStaticMeshComponent* Component = It->GetStaticMeshComponent();
		const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		if (!Mesh || !Mesh->GetName().Contains(TEXT("_Fractal_")))
		{
			continue;
		}
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			++Surfaces;
			UMaterialInterface* Material = Component->GetMaterial(Slot);
			const FString Bound = TexName(Material);
			TestTrue(FString::Printf(TEXT("%s slot %d samples a live animated texture (%s via %s)"),
				*Mesh->GetName(), Slot, *Bound,
				Material ? *Material->GetName() : TEXT("-")),
				Bound.StartsWith(TEXT("WoTLive_")));
		}
	}
	TestTrue(TEXT("Arena_06 has procedural BSP surfaces"), Surfaces > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEditorPlaytestArena06ShimmerQuadsSpec,
	"WoT.EditorPlaytest.Arena06.TeleporterShimmerQuadsLive", WoTEditorPlaytest::Flags)

bool FWoTEditorPlaytestArena06ShimmerQuadsSpec::RunTest(const FString&)
{
	using namespace WoTEditorPlaytest;
	FPlayWorld Play;
	if (!Play.Load(*this, Arena06))
	{
		return !HasAnyErrors();
	}
	UWorld& World = *Play.World;
	// twotriangles0-3 (Skin=WetTexture'ScottT.S_Tele_effect', STY_Translucent,
	// bCollideActors=False in the shipped Arena_06 map) front the teleporter
	// alcoves.
	// The ripple rewrites every pixel, so the quad never shows a still frame,
	// and the player walks through it onto the pad.
	int32 Quads = 0;
	for (TActorIterator<AWoTDecoration> It(&World); It; ++It)
	{
		if (!It->GetActorLabel().StartsWith(TEXT("twotriangles")))
		{
			continue;
		}
		++Quads;
		UMaterialInterface* Material = It->Mesh->GetMaterial(0);
		const FString Bound = TexName(Material);
		TestTrue(FString::Printf(TEXT("%s samples the live S_Tele_effect ripple (%s via %s)"),
			*It->GetActorLabel(), *Bound, Material ? *Material->GetName() : TEXT("-")),
			Bound.StartsWith(TEXT("WoTLive_ScottT_S_Tele_effect")));
		TestTrue(FString::Printf(TEXT("%s neither touches nor blocks"), *It->GetActorLabel()),
			It->Collision->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	}
	TestEqual(TEXT("Arena_06 places the four teleporter shimmer quads"), Quads, 4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEditorPlaytestProceduralCensusSpec,
	"WoT.EditorPlaytest.ProceduralSlotCensus", WoTEditorPlaytest::Flags)

bool FWoTEditorPlaytestProceduralCensusSpec::RunTest(const FString&)
{
	// Every material slot of the editor-imported MP maps that names a fractal
	// record (the importers' MI_W_<Pkg>__<Tex><type> world sections, MI_<Tex>
	// and the mesh/particle identity instances) must resolve to it, or the
	// editor play-test shows the surface's still pre-activation texture.
	const TCHAR* CatalogPath =
		TEXT("/Game/WoT/Data/DA_WoTFractalCatalog.DA_WoTFractalCatalog");
	if (!FPackageName::DoesPackageExist(TEXT("/Game/WoT/Data/DA_WoTFractalCatalog")))
	{
		AddWarning(TEXT("the fractal catalog is not imported in this content tree; spec skipped"));
		return true;
	}
	UWorld* SpecWorld = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Fractals = SpecWorld
		? SpecWorld->GetSubsystem<UWoTFractalSubsystem>() : nullptr;
	UWoTFractalCatalog* Catalog = LoadObject<UWoTFractalCatalog>(nullptr, CatalogPath);
	if (!TestNotNull(TEXT("fractal subsystem"), Fractals)
		|| !TestNotNull(TEXT("catalog"), Catalog))
	{
		WoTTest::DestroySpecWorld(SpecWorld);
		return false;
	}
	static const TCHAR* Suffixes[] = {TEXT("_Mod"), TEXT("_Sky"), TEXT("_SL"),
		TEXT("_2S"), TEXT("_M"), TEXT("_T"), TEXT("_U")};
	auto OnlySuffixes = [](FString Rest)
	{
		while (!Rest.IsEmpty())
		{
			bool bPeeled = false;
			for (const TCHAR* Suffix : Suffixes)
			{
				if (Rest.StartsWith(Suffix, ESearchCase::CaseSensitive))
				{
					Rest.RightChopInline(FCString::Strlen(Suffix));
					bPeeled = true;
					break;
				}
			}
			if (!bPeeled) return false;
		}
		return true;
	};
	auto Carries = [&](const FString& Name) -> bool
	{
		for (const FWoTFractalTextureRecord& Record : Catalog->Textures)
		{
			const FString Slug = UWoTFractalSubsystem::MakeFractalIdentityKey(
				Record.PackageName, Record.TextureName).ToString();
			for (const FString& Key : {Slug, Record.TextureName.ToString()})
			{
				const int32 At = Name.Find(Key, ESearchCase::IgnoreCase);
				if (At > 0 && Name[At - 1] == TEXT('_')
					&& OnlySuffixes(Name.Mid(At + Key.Len())))
				{
					return true;
				}
			}
		}
		return false;
	};
	const TCHAR* Maps[] = {TEXT("Arena_01"), TEXT("Arena_02"), TEXT("Arena_03"),
		TEXT("Arena_04"), TEXT("Arena_05"), TEXT("Arena_06"), TEXT("Arena_07"),
		TEXT("Arena_08"), TEXT("Arena_09"), TEXT("Arena_11"), TEXT("Arena_12"),
		TEXT("Battle2_01"), TEXT("Battle2_02"), TEXT("Battle2_03"), TEXT("Battle2_04"),
		TEXT("Battle2_05"), TEXT("Battle3_01"), TEXT("Battle3_02"), TEXT("Battle4_01")};
	int32 Candidates = 0;
	int32 Missed = 0;
	for (const TCHAR* Map : Maps)
	{
		const FString Path = FString(TEXT("/Game/WoT/Maps/")) + Map;
		UPackage* Package = LoadPackage(nullptr, *Path, LOAD_None);
		UWorld* MapWorld = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
		if (!MapWorld || !MapWorld->PersistentLevel)
		{
			AddWarning(Path + TEXT(" is not imported"));
			continue;
		}
		TSet<FString> Seen;
		for (AActor* Actor : MapWorld->PersistentLevel->Actors)
		{
			if (!Actor) continue;
			TInlineComponentArray<UPrimitiveComponent*> Components(Actor);
			for (UPrimitiveComponent* Component : Components)
			{
				for (int32 Slot = 0; Component && Slot < Component->GetNumMaterials(); ++Slot)
				{
					UMaterialInterface* Material = Component->GetMaterial(Slot);
					if (!Material) continue;
					bool bCandidate = false;
					for (const UMaterialInterface* Chain = Material; Chain && !bCandidate;)
					{
						bCandidate = Carries(Chain->GetName());
						const UMaterialInstance* Instance = Cast<UMaterialInstance>(Chain);
						Chain = Instance ? ToRawPtr(Instance->Parent) : nullptr;
					}
					if (!bCandidate) continue;
					++Candidates;
					if (!Fractals->FindRecordForMaterialForTesting(Material))
					{
						const FString Key = FString(Map) + TEXT(" ") + Material->GetName();
						if (!Seen.Contains(Key))
						{
							Seen.Add(Key);
							++Missed;
							AddInfo(FString::Printf(TEXT("UNBOUND %s on %s.%s"), *Key,
								*Actor->GetActorLabel(), *Component->GetName()));
						}
					}
				}
			}
		}
		// Loading a map package in the editor initializes its (inactive) world
		// (UEditorEngine::OnAssetLoaded). Clean it up here, or a later garbage
		// collection purges an initialized world and its subsystems ensure.
		if (MapWorld->bIsWorldInitialized)
		{
			MapWorld->CleanupWorld();
		}
	}
	AddInfo(FString::Printf(TEXT("census: %d procedural slots, %d unbound materials"),
		Candidates, Missed));
	TestEqual(TEXT("every procedural-named slot of the imported MP maps resolves"), Missed, 0);
	WoTTest::DestroySpecWorld(SpecWorld);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return !HasAnyErrors();
}

#endif
