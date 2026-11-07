// Gameplay-AI parity closure specs.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTSight.h"
#include "../Inventory/WoTAngrealInvEarthTremor.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Inventory/WoTAbsorbReflector.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTDecayProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBeamEffect.h"
#include "../WoTBalefireFX.h"
#include "../WoTBudgetInfo.h"
#include "../WoTGameMode.h"
#include "../WoTMeshFX.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTDecal.h"
#include "../WoTCreatureChunk.h"
#include "../WoTFragment.h"
#include "../WoTRainCyd.h"
#include "../WoTAmbientSound.h"
#include "../WoTParticleSprayer.h"
#include "../WoTActorRotator.h"
#include "../WoTWeaveFX.h"
#include "../WoTVertexAnimComponent.h"
#include "../Mesh/WoTRuntimeVertexMesh.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTPlayerStart.h"
#include "../WoTRandom.h"
#include "../WoTSealAltar.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTSoundTable.h"
#include "../WoTWorldFX.h"
#include "../WoTTeleporter.h"
#include "../WoTUE1Collision.h"
#include "../WoTWorldFX.h"
#include "../WoTZoneVolume.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTWorldProjector.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Traps/WoTFireWall.h"
#include "WoTTestAIClasses.h"
#include "WoTRetailFamiliesRestoreScope.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#if WITH_EDITOR
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Tests/NetTestHelpers.h"
#endif
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UnrealType.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
	class FRC33RetailProjectionWriter final : public IWoTObjectWriter
	{
	public:
		virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId,
			FStringView Schema, TConstArrayView<uint8> Bytes, const FWoTSha256&,
			uint64 MaximumBytes) override
		{
			if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::SizeLimit,
					TEXT("RC33 retail projection writer overflow")));
			}
			if (Schema == TEXT("wot-world-v3")
				|| Schema == TEXT("wot-class-default-v3"))
			{
				TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
				Copy.Append(Bytes.GetData(), Bytes.Num());
			}
			return FWoTBootstrapStatus::Success();
		}

		TMap<FString, TArray<uint8>> Objects;
	};

	class FRC33RetailProjectionProgress final : public IWoTProgressSink
	{
	public:
		virtual FWoTBootstrapStatus ReportProgress(
			FStringView, uint64, uint64) override
		{
			return FWoTBootstrapStatus::Success();
		}
		virtual bool IsCancellationRequested() const override { return false; }
	};

	bool LoadRC33Battle203WindProjectionSource(
		FWoTWorldProjectionSource& OutSource,
		TArray<FWoTRuntimeWorldActor>& OutRecords,
		TUniquePtr<FWoTRuntimeClassDefaults>& OutDefaults, FString& OutError)
	{
		if (!WoTReferenceTreeExists())
		{
			OutError = WoTReferenceMissingWarning(TEXT("original"));
			return false;
		}
		TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
			WoTLoadProductionRetailContract();
		if (LoadedContract.IsError())
		{
			OutError = LoadedContract.GetError().ToString();
			return false;
		}
		const FString RetailRoot = WoTReferencePath(TEXT("original"));
		FWoTRetailContract WorldContract;
		FWoTRetailContract DefaultsContract;
		TArray<FWoTWorldPackageBytesForTesting> WorldSources;
		TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
		TArray<FString> DefaultLogicalIds;
		for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
		{
			const bool bMultiplayerWorldFact =
				(Fact.Domain == EWoTFactDomain::World
					|| Fact.Domain == EWoTFactDomain::BSP)
				&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"),
					ESearchCase::IgnoreCase);
			if (bMultiplayerWorldFact)
			{
				WorldContract.Facts.Add(Fact);
				if (Fact.Domain == EWoTFactDomain::World)
				{
					FWoTWorldPackageBytesForTesting& Source =
						WorldSources.AddDefaulted_GetRef();
					Source.SourcePath = Fact.SourcePath;
					if (!FFileHelper::LoadFileToArray(Source.Bytes,
						*(RetailRoot / Fact.SourcePath)))
					{
						OutError = Fact.SourcePath + TEXT(" could not be loaded");
						return false;
					}
				}
			}
			if (Fact.Domain != EWoTFactDomain::ClassDefault) continue;
			DefaultsContract.Facts.Add(Fact);
			FWoTRemainingSourceBytesForTesting& Source =
				DefaultSources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!FFileHelper::LoadFileToArray(Source.Bytes,
				*(RetailRoot / Fact.SourcePath)))
			{
				OutError = Fact.SourcePath + TEXT(" could not be loaded");
				return false;
			}
			if (Fact.Outputs.Num() == 1)
				DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
		}
		if (WorldSources.Num() != 19 || DefaultSources.Num() != 20
			|| DefaultLogicalIds.Num() != 20)
		{
			OutError = TEXT("Battle2_03 authenticated projection closure is incomplete");
			return false;
		}

		FRC33RetailProjectionWriter Writer;
		FRC33RetailProjectionProgress Progress;
		TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorld =
			ImportRequiredWorldSetPackageBytesForTesting(
				WorldSources, WorldContract, Writer, Progress);
		if (ImportedWorld.IsError()
			|| ImportedWorld.GetValue().Worlds.Num() != 19)
		{
			OutError = ImportedWorld.IsError()
				? ImportedWorld.GetError().ToString()
				: TEXT("authenticated importer did not return nineteen worlds");
			return false;
		}
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
			ImportRegistryLocalizationDefaultsSourcesForTesting(
				DefaultSources, DefaultsContract, Writer, Progress);
		if (ImportedDefaults.IsError())
		{
			OutError = ImportedDefaults.GetError().ToString();
			return false;
		}

		TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
		for (const FString& LogicalId : DefaultLogicalIds)
		{
			TArray<uint8>* Bytes = Writer.Objects.Find(LogicalId);
			if (!Bytes)
			{
				OutError = LogicalId + TEXT(" was not imported");
				return false;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*Bytes),
					TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
			if (Decoded.IsError())
			{
				OutError = Decoded.GetError().ToString();
				return false;
			}
			DefaultObjects.Add(MoveTemp(Decoded).GetValue());
		}
		OutDefaults = MakeUnique<FWoTRuntimeClassDefaults>();
		if (!OutDefaults->LoadObjectsForTesting(
			MoveTemp(DefaultObjects), OutError)) return false;

		const FWoTWorldDescriptor* Descriptor =
			ImportedWorld.GetValue().Worlds.FindByPredicate(
				[](const FWoTWorldDescriptor& Candidate)
				{
					return Candidate.MapIdentity == TEXT("Battle2_03");
				});
		if (!Descriptor)
		{
			OutError = TEXT("authenticated world set has no Battle2_03 descriptor");
			return false;
		}
		TArray<uint8>* WorldBytes = Writer.Objects.Find(Descriptor->WorldLogicalId);
		if (!WorldBytes)
		{
			OutError = TEXT("authenticated Battle2_03 world object is absent");
			return false;
		}
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (DecodedWorld.IsError())
		{
			OutError = DecodedWorld.GetError().ToString();
			return false;
		}
		FWoTRuntimeWorldResource Resource;
		FWoTBootstrapStatus Loaded =
			Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue());
		if (Loaded.IsError())
		{
			OutError = Loaded.GetError().ToString();
			return false;
		}
		const FWoTRuntimeWorldActor* Wind0 = Resource.Actors().FindByPredicate(
			[](const FWoTRuntimeWorldActor& Record)
			{
				return Record.Identity.Equals(TEXT("Battle2_03.Wind0"),
					ESearchCase::IgnoreCase);
			});
		if (!Wind0)
		{
			OutError = TEXT("authenticated Battle2_03 source has no Wind0");
			return false;
		}
		OutSource = FWoTWorldProjectionSource();
		OutSource.MapIdentity = Resource.MapIdentity();
		OutRecords.Reset();
		OutRecords.Add(*Wind0);
		OutSource.Records = OutRecords;
		OutError.Empty();
		return true;
	}

	FActorSpawnParameters RC33AlwaysSpawn()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AWoTTestNPCCharacter* SpawnRC33NPC(UWorld* World, const FVector& Location,
		uint8 Team)
	{
		AWoTTestNPCCharacter* NPC = World->SpawnActor<AWoTTestNPCCharacter>(
			Location, FRotator::ZeroRotator, RC33AlwaysSpawn());
		if (NPC)
		{
			NPC->AssignedTeam = Team;
			NPC->Health = 100;
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return NPC;
	}

	AWoTCharacter* SpawnRC33Player(UWorld* World, const FVector& Location,
		uint8 Team, AWoTPlayerController*& OutController)
	{
		OutController = World->SpawnActor<AWoTPlayerController>(RC33AlwaysSpawn());
		OutController->InitPlayerState();
		if (AWoTPlayerState* PS = OutController->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = Team;
		}
		AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator, RC33AlwaysSpawn());
		OutController->Possess(Player);
		return Player;
	}

	AStaticMeshActor* SpawnRC33Wall(UWorld* World, const FVector& Location,
		const FVector& Extent)
	{
		AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(
			Location, FRotator::ZeroRotator, RC33AlwaysSpawn());
		UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
		Box->SetupAttachment(Wall->GetRootComponent());
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		return Wall;
	}

	AWoTDecal* FindRC33BounceDecal(UWorld* World)
	{
		for (TActorIterator<AWoTDecal> It(World); It; ++It)
		{
			if (It->DecalType == FName(TEXT("BloodDecalRed")) && It->IsPlaced())
			{
				return *It;
			}
		}
		return nullptr;
	}

	void SetRC33LocalView(UWorld* World, APlayerController* Controller,
		const FVector& Location, const FVector& Target)
	{
		if (Controller->PlayerCameraManager)
		{
			FMinimalViewInfo View;
			View.Location = Location;
			View.Rotation = (Target - Location).Rotation();
			Controller->PlayerCameraManager->SetCameraCachePOV(View);
		}
	}

	int32 CountRC33SoundMarkers(UWorld* World, FStringView Needle)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			Count += IsValid(*It) && !It->IsActorBeingDestroyed()
				&& It->SoundSubPath.ToString().Contains(
					Needle, ESearchCase::IgnoreCase) ? 1 : 0;
		}
		return Count;
	}

	AWoTNPCCharacter* FindRC33Creature(UWorld* World, FName Row)
	{
		for (TActorIterator<AWoTNPCCharacter> It(World); It; ++It)
		{
			if (It->CreatureRowName == Row) return *It;
		}
		return nullptr;
	}

	bool ReadRC33BodyGlow(const AWoTCharacter* Character, float& OutGlow)
	{
		if (!Character || !Character->GetMesh()) return false;
		for (int32 Slot = 0; Slot < Character->GetMesh()->GetNumMaterials(); ++Slot)
		{
			if (const UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(
				Character->GetMesh()->GetMaterial(Slot)))
			{
				if (MID->GetScalarParameterValue(TEXT("MLScaleGlow"), OutGlow))
					return true;
			}
		}
		return false;
	}

#if WITH_EDITOR
	UE::Net::FTestWorldInstance CreateRC33ListenServer()
	{
		using namespace UE::Net;
		FTestWorldInstance Instance(/*bDelayedInit*/ true);
		UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
		GameInstance->AddToRoot();
		FGameInstancePIEParameters Parameters;
		Parameters.bSimulateInEditor = false;
		Parameters.bAnyBlueprintErrors = false;
		Parameters.bStartInSpectatorMode = false;
		Parameters.bRunAsDedicated = false;
		Parameters.bIsPrimaryPIEClient = false;
		Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
		Parameters.EditorPlaySettings = nullptr;
		Parameters.NetMode = PIE_ListenServer;
		Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
		int32 PIEInstance = INDEX_NONE;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
		}
		++PIEInstance;
		{
			FScopedNetTestPIERestoration Restoration;
			UE::SetPlayInEditorID(-1);
			const FGameInstancePIEResult Initialized =
				GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
			if (!Initialized.IsSuccess())
			{
				GameInstance->Shutdown();
				GameInstance->RemoveFromRoot();
				return Instance;
			}
		}
		Instance.GameInstance = GameInstance;
		{
			FScopedNetTestPIERestoration Restoration;
			UE::SetPlayInEditorID(-1);
			FURL URL(nullptr,
				TEXT("/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTGameMode"),
				TRAVEL_Absolute);
			FString Error;
			Instance.bIsLoaded = GEngine->Browse(
				*GameInstance->GetWorldContext(), URL, Error)
				== EBrowseReturnVal::Success;
		}
		return Instance;
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33RegisteredDecalBounceViewsSpec,
	"WoT.RC33.PresentationA.P2.RegisteredDecalBounceViews",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33RegisteredDecalBounceViewsSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	auto SpawnBounce = [this](FTestWorlds& Worlds) -> AWoTCreatureChunk*
	{
		UWorld* ServerWorld = Worlds.Server.GetWorld();
		SpawnRC33Wall(ServerWorld, FVector(0, 0, -50), FVector(1000, 1000, 50));
		// Keep the producer clear of the fixture wall until its initial bunch has
		// reached both clients; the bounce itself is injected below.
		AWoTCreatureChunk* Chunk = ServerWorld->SpawnActor<AWoTCreatureChunk>(
			FVector(0, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
		Chunk->ConfigureBounceDecalForTest(false);
		Chunk->SetVelocityForTest(FVector(0, 0, -300));
		Chunk->ForceNetUpdate();
		const bool bAllClientsHaveProducer = Worlds.TickUntil([&Worlds]()
		{
			for (const FTestWorldInstance& Client : Worlds.Clients)
			{
				const bool bFound = !!TActorIterator<AWoTCreatureChunk>(
					Client.GetWorld());
				if (!bFound) return false;
			}
			return true;
		}, 180);
		TestTrue(TEXT("bounce producer replicates before its event"),
			bAllClientsHaveProducer);
		Chunk->SetActorLocation(FVector(0, 0, 2), false, nullptr,
			ETeleportType::TeleportPhysics);
		Chunk->HitWallForTest(FVector::UpVector);
		return Chunk;
	};

	{
		FTestWorlds Dedicated(TEXT("/Engine/Maps/Entry"),
			TEXT("/Script/WoT.WoTGameMode"));
		Dedicated.SetDescription(TEXT("registered bounce decal dedicated views"));
		if (!TestTrue(TEXT("dedicated server loads"), Dedicated.Server.IsLoaded())
			|| !TestNotNull(TEXT("dedicated bounce spare start one"),
				Dedicated.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, -1000, 5000), FRotator::ZeroRotator,
					RC33AlwaysSpawn()))
			|| !TestNotNull(TEXT("dedicated bounce spare start two"),
				Dedicated.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, 1000, 5000), FRotator::ZeroRotator,
					RC33AlwaysSpawn()))
			|| !TestTrue(TEXT("first rendering client connects"),
				Dedicated.CreateAndConnectClient())
			|| !TestTrue(TEXT("second rendering client connects"),
				Dedicated.CreateAndConnectClient()))
		{
			return false;
		}
		SpawnBounce(Dedicated);
		AWoTDecal* First = nullptr;
		AWoTDecal* Second = nullptr;
		TestTrue(TEXT("both dedicated clients receive local bounce effects"),
			Dedicated.TickUntil([&Dedicated, &First, &Second]()
			{
				First = FindRC33BounceDecal(Dedicated.Clients[0].GetWorld());
				Second = FindRC33BounceDecal(Dedicated.Clients[1].GetWorld());
				return First && Second;
			}, 180));
		TestNull(TEXT("dedicated server never allocates the registered decal"),
			FindRC33BounceDecal(Dedicated.Server.GetWorld()));
		if (First && Second)
		{
			TestFalse(TEXT("first client decal is non-replicated"),
				First->GetIsReplicated());
			TestFalse(TEXT("second client decal is non-replicated"),
				Second->GetIsReplicated());
			const FVector DecalLocation = First->GetActorLocation();
			APlayerController* FirstPC = Dedicated.Clients[0].GetWorld()
				->GetFirstPlayerController();
			APlayerController* SecondPC = Dedicated.Clients[1].GetWorld()
				->GetFirstPlayerController();
			SetRC33LocalView(Dedicated.Clients[0].GetWorld(), FirstPC,
				DecalLocation + FVector(0, -100, 100), DecalLocation);
			SetRC33LocalView(Dedicated.Clients[1].GetWorld(), SecondPC,
				DecalLocation + FVector(0, -100, 100),
				DecalLocation + FVector(0, -200, 200));
			First->ExpireManagerForTest();
			Second->ExpireManagerForTest();
			TestFalse(TEXT("visible first-client decal survives local expiry"),
				First->IsActorBeingDestroyed());
			TestTrue(TEXT("hidden second-client decal expires from its own view"),
				Second->IsActorBeingDestroyed());
		}
	}

	{
		FTestWorlds Listen;
		Listen.Server = CreateRC33ListenServer();
		Listen.SetDescription(TEXT("registered bounce decal listen host view"));
		if (!TestTrue(TEXT("listen server loads"), Listen.Server.IsLoaded()))
		{
			return false;
		}
		UWorld* HostWorld = Listen.Server.GetWorld();
		if (!TestNotNull(TEXT("listen bounce spare start"),
			HostWorld->SpawnActor<AWoTPlayerStart>(
				FVector(-10000, 0, 5000), FRotator::ZeroRotator,
				RC33AlwaysSpawn())))
		{
			return false;
		}
		AWoTPlayerController* HostPC = HostWorld->SpawnActor<AWoTPlayerController>(
			RC33AlwaysSpawn());
		HostPC->SetAsLocalPlayerController();
		if (!TestTrue(TEXT("listen remote client connects"),
			Listen.CreateAndConnectClient()))
		{
			return false;
		}
		SpawnBounce(Listen);
		AWoTDecal* HostDecal = nullptr;
		AWoTDecal* RemoteDecal = nullptr;
		TestTrue(TEXT("listen host and remote receive their local bounce effects"),
			Listen.TickUntil([&Listen, HostWorld, &HostDecal, &RemoteDecal]()
			{
				HostDecal = FindRC33BounceDecal(HostWorld);
				RemoteDecal = FindRC33BounceDecal(Listen.Clients[0].GetWorld());
				return HostDecal && RemoteDecal;
			}, 180));
		if (HostDecal && RemoteDecal)
		{
			TestFalse(TEXT("listen-host decal is non-replicated"),
				HostDecal->GetIsReplicated());
			TestFalse(TEXT("listen-remote decal is non-replicated"),
				RemoteDecal->GetIsReplicated());
			const FVector HostLocation = HostDecal->GetActorLocation();
			SetRC33LocalView(HostWorld, HostPC,
				HostLocation + FVector(0, -100, 100),
				HostLocation + FVector(0, -200, 200));
			APlayerController* RemotePC = Listen.Clients[0].GetWorld()
				->GetFirstPlayerController();
			const FVector RemoteLocation = RemoteDecal->GetActorLocation();
			SetRC33LocalView(Listen.Clients[0].GetWorld(), RemotePC,
				RemoteLocation + FVector(0, -100, 100), RemoteLocation);
			HostDecal->ExpireManagerForTest();
			RemoteDecal->ExpireManagerForTest();
			TestTrue(TEXT("hidden listen-host decal expires from the host view"),
				HostDecal->IsActorBeingDestroyed());
			TestFalse(TEXT("visible listen-remote decal survives its remote view"),
				RemoteDecal->IsActorBeingDestroyed());
		}
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33PresentationCClientRolesSpec,
	"WoT.RC33.PresentationC.P16.ClientObservableRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33PresentationCClientRolesSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	ResetWoTPayloadActivationForTesting();
	if (!TestFalse(TEXT("presentation-C payload authority publishes"),
		PublishWoTPayloadBootstrapActivationForTesting().IsError())
		|| !TestFalse(TEXT("presentation-C world authority publishes"),
			PublishWoTBootstrapWorldReadyForTesting().IsError()))
	{
		ResetWoTPayloadActivationForTesting();
		return false;
	}
	ON_SCOPE_EXIT { ResetWoTPayloadActivationForTesting(); };
	auto AddStarts = [](UWorld* World)
	{
		World->SpawnActor<AWoTPlayerStart>(FVector(-200, -100, 200),
			FRotator::ZeroRotator, RC33AlwaysSpawn());
		World->SpawnActor<AWoTPlayerStart>(FVector(-200, 100, 200),
			FRotator::ZeroRotator, RC33AlwaysSpawn());
	};
	auto SpawnWaterEntry = [this](FTestWorlds& Worlds,
		const FString& Label) -> bool
	{
		UWorld* ServerWorld = Worlds.Server.GetWorld();
		AWoTZoneVolume* Water = ServerWorld->SpawnActor<AWoTZoneVolume>(
			FVector(0, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
		if (!TestNotNull(Label + TEXT(" water zone"), Water)) return false;
		Water->Collision->SetBoxExtent(FVector(1000.0f));
		Water->bWaterZone = true;
		Water->EntrySoundSubPath = TEXT("AmbientA/DIVE");

		FActorSpawnParameters Params = RC33AlwaysSpawn();
		Params.Owner = ServerWorld->GetFirstPlayerController();
		AWoTFragment* Fragment = ServerWorld->SpawnActor<AWoTFragment>(
			AWoTFragment::StaticClass(), FVector(1500, 0, 5000),
			FRotator::ZeroRotator, Params);
		if (!TestNotNull(Label + TEXT(" fragment"), Fragment)) return false;
		Fragment->Init(EWoTFragmentType::Wall, 200.0f,
			FVector::ZeroVector, 1.0f, NAME_None);
		Fragment->bAlwaysRelevant = true;
		Fragment->ForceNetUpdate();
		if (!TestTrue(Label + TEXT(" fragment reaches every remote view"),
			Worlds.TickUntil([&Worlds]()
			{
				for (const FTestWorldInstance& Client : Worlds.Clients)
					if (!TActorIterator<AWoTFragment>(Client.GetWorld())) return false;
				return true;
			}, 120))) return false;

		Fragment->SetActorLocation(FVector(0, 0, 5000), false, nullptr,
			ETeleportType::TeleportPhysics);
		AWoTSoundBurst* ServerBurst = nullptr;
		if (!TestTrue(Label + TEXT(" authority publishes EntrySound"),
			Worlds.TickUntil([ServerWorld, &ServerBurst]()
			{
				for (TActorIterator<AWoTSoundBurst> It(ServerWorld); It; ++It)
				{
					if (It->SoundSubPath == TEXT("AmbientA/DIVE"))
					{
						ServerBurst = *It;
						return true;
					}
				}
				return false;
			}, 30))) return false;
		// Stabilize the network observation window without changing the
		// production creation path that the clients exercise.
		ServerBurst->bAlwaysRelevant = true;
		ServerBurst->SetLifeSpan(5.0f);
		ServerBurst->ForceNetUpdate();
		return TestTrue(Label + TEXT(" EntrySound reaches every remote view"),
			Worlds.TickUntil([&Worlds]()
			{
				for (const FTestWorldInstance& Client : Worlds.Clients)
				{
					bool bHeard = false;
					for (TActorIterator<AWoTSoundBurst> It(Client.GetWorld()); It; ++It)
						bHeard |= It->SoundSubPath == TEXT("AmbientA/DIVE");
					if (!bHeard) return false;
				}
				return true;
			}, 120));
	};
	auto AssertLocalPresentation = [this](UWorld* World, const FString& Label)
	{
		AWoTRainCyd* Rain = World->SpawnActor<AWoTRainCyd>();
		AWoTAmbientSound* Ambient = World->SpawnActorDeferred<AWoTAmbientSound>(
			AWoTAmbientSound::StaticClass(), FTransform::Identity);
		Ambient->LightType = 2;
		Ambient->FinishSpawning(FTransform::Identity);
		TestNotNull(Label + TEXT(" local RainCyd"), Rain);
		TestNotNull(Label + TEXT(" local ambient"), Ambient);
		if (Rain)
		{
			TestFalse(Label + TEXT(" RainCyd is local, not replicated"),
				Rain->GetIsReplicated());
			TestTrue(Label + TEXT(" RainCyd presentation ticks"),
				Rain->IsActorTickEnabled());
		}
		if (Ambient)
		{
			TestFalse(Label + TEXT(" ambient is local, not replicated"),
				Ambient->GetIsReplicated());
			TestTrue(Label + TEXT(" light-carried ambient ticks"),
				Ambient->IsActorTickEnabled());
		}
	};

	{
		FTestWorlds Dedicated(TEXT("/Engine/Maps/Entry"),
			TEXT("/Script/WoT.WoTGameMode"));
		Dedicated.SetDescription(TEXT("presentation-C dedicated two-client roles"));
		if (!TestTrue(TEXT("presentation-C dedicated server loads"),
			Dedicated.Server.IsLoaded())) return false;
		UWorld* ServerWorld = Dedicated.Server.GetWorld();
		AddStarts(ServerWorld);
		if (!TestTrue(TEXT("presentation-C owning-view client connects"),
			Dedicated.CreateAndConnectClient())
			|| !TestTrue(TEXT("presentation-C simulated-view client connects"),
				Dedicated.CreateAndConnectClient())) return false;
		if (!TestTrue(TEXT("both presentation-C client worlds begin play"),
			Dedicated.TickUntil([&Dedicated]()
			{
				for (const FTestWorldInstance& Client : Dedicated.Clients)
					if (!Client.GetWorld()->HasBegunPlay()
						|| !Client.GetWorld()->GetFirstPlayerController()) return false;
				return true;
			}, 120))) return false;

		AWoTRainCyd* ServerRain = ServerWorld->SpawnActor<AWoTRainCyd>();
		AWoTAmbientSound* ServerAmbient =
			ServerWorld->SpawnActorDeferred<AWoTAmbientSound>(
				AWoTAmbientSound::StaticClass(), FTransform::Identity);
		ServerAmbient->LightType = 2;
		ServerAmbient->FinishSpawning(FTransform::Identity);
		TestTrue(TEXT("dedicated RainCyd is hidden and inert"), ServerRain
			&& ServerRain->IsHidden() && !ServerRain->IsActorTickEnabled());
		TestTrue(TEXT("dedicated ambient is inert"), ServerAmbient
			&& !ServerAmbient->IsActorTickEnabled());
		for (int32 Index = 0; Index < Dedicated.Clients.Num(); ++Index)
			AssertLocalPresentation(Dedicated.Clients[Index].GetWorld(),
				FString::Printf(TEXT("dedicated client %d"), Index));
		if (!SpawnWaterEntry(Dedicated, TEXT("dedicated clients"))) return false;
	}

	{
		FTestWorlds Listen;
		Listen.Server = CreateRC33ListenServer();
		Listen.SetDescription(TEXT("presentation-C listen-host roles"));
		if (!TestTrue(TEXT("presentation-C listen server loads"),
			Listen.Server.IsLoaded())) return false;
		UWorld* HostWorld = Listen.Server.GetWorld();
		AddStarts(HostWorld);
		AWoTPlayerController* HostPC =
			HostWorld->SpawnActor<AWoTPlayerController>(RC33AlwaysSpawn());
		HostPC->SetAsLocalPlayerController();
		if (!TestTrue(TEXT("presentation-C listen remote connects"),
			Listen.CreateAndConnectClient())) return false;
		if (!TestTrue(TEXT("presentation-C listen remote begins play"),
			Listen.TickUntil([&Listen]()
			{
				return Listen.Clients[0].GetWorld()->HasBegunPlay()
					&& Listen.Clients[0].GetWorld()->GetFirstPlayerController();
			}, 120))) return false;
		AssertLocalPresentation(HostWorld, TEXT("listen host"));
		AssertLocalPresentation(Listen.Clients[0].GetWorld(), TEXT("listen remote"));
		if (!SpawnWaterEntry(Listen, TEXT("listen host and remote"))) return false;
		bool bHostHeard = false;
		for (TActorIterator<AWoTSoundBurst> It(HostWorld); It; ++It)
			bHostHeard |= It->SoundSubPath == TEXT("AmbientA/DIVE");
		TestTrue(TEXT("listen host receives the local EntrySound marker"), bHostHeard);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33PlayerStartEncroachmentLifecycleSpec,
	"WoT.RC33.Q1.PlayerStartActorEncroachmentLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33PlayerStartEncroachmentLifecycleSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 real-login rejection and fire retry"));
	if (!TestTrue(TEXT("spawn lifecycle dedicated server loads"),
		Worlds.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	for (TActorIterator<APlayerStart> It(ServerWorld); It; ++It)
	{
		It->Destroy();
	}
	Worlds.TickAll(2);
	const FVector StartLocation(0.0, 0.0, 5000.0);
	AWoTPlayerStart* Start = ServerWorld->SpawnActor<AWoTPlayerStart>(
		StartLocation, FRotator::ZeroRotator, RC33AlwaysSpawn());
	AWoTGameMode* Mode = ServerWorld->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("production game mode"), Mode)
		|| !TestNotNull(TEXT("single production PlayerStart"), Start)
		|| !TestTrue(TEXT("first real client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTPlayerController* ClientPC = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* FirstPawn = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("first login occupies the only start"), FirstPawn)
		|| !TestNotNull(TEXT("first connection has an owning controller"), ClientPC))
	{
		return false;
	}
	FString RejectedClientFailureReason;
	UWorld* FirstClientWorld = Worlds.Clients[0].GetWorld();
	const FDelegateHandle NetworkFailureHandle = GEngine->OnNetworkFailure().AddLambda(
		[&RejectedClientFailureReason, FirstClientWorld](UWorld* FailedWorld,
			UNetDriver*, ENetworkFailure::Type, const FString& Reason)
		{
			if (FailedWorld && FailedWorld != FirstClientWorld
				&& FailedWorld->GetNetMode() == NM_Client && !Reason.IsEmpty())
			{
				RejectedClientFailureReason = Reason;
			}
		});
	const FDelegateHandle TravelFailureHandle = GEngine->OnTravelFailure().AddLambda(
		[&RejectedClientFailureReason, FirstClientWorld](UWorld* FailedWorld,
			ETravelFailure::Type, const FString& Reason)
		{
			if (FailedWorld && FailedWorld != FirstClientWorld
				&& FailedWorld->GetNetMode() == NM_Client && !Reason.IsEmpty())
			{
				RejectedClientFailureReason = Reason;
			}
		});
	ON_SCOPE_EXIT
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	};

	// A second real connection reaches PostLogin but cannot enter the occupied
	// start. It must be rejected, never retained as a pawnless controller.
	Worlds.CreateAndConnectClient();
	Worlds.TickAll(30);
	TestTrue(TEXT("failed initial spawn reports a clear disconnect reason"),
		!Mode->LastInitialSpawnFailureReasonForTest().IsEmpty());
	TestTrue(TEXT("rejected client receives a nonempty network/travel failure reason"),
		!RejectedClientFailureReason.IsEmpty());
	TestEqual(TEXT("rejected client receives the exact server-authored reason"),
		RejectedClientFailureReason,
		Mode->LastInitialSpawnFailureReasonForTest());
	APlayerController* StrandedSecond = Worlds.Clients.Num() > 1
		? Worlds.GetServerPlayerControllerOfClient(1) : nullptr;
	TestNull(TEXT("failed initial login leaves no server-side pawnless controller"),
		StrandedSecond);

	FirstPawn->SetActorLocation(StartLocation + FVector(3000.0, 0.0, 0.0));
	AWoTCharacter* Blocker = ServerWorld->SpawnActor<AWoTCharacter>(
		StartLocation, FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestNotNull(TEXT("restart-blocking pawn"), Blocker)) return false;
	ServerPC->ArmRespawnFire(FirstPawn, 0.0f);
	ServerPC->UnPossess();
	FirstPawn->Destroy();
	ServerPC->ForceNetUpdate();
	TestTrue(TEXT("owner receives Dying before sending real fire input"),
		Worlds.TickUntil([ClientPC]()
		{
			return ClientPC->GetPlayerStateMachineState()
				== EWoTPlayerStateMachineState::Dying;
		}, 120));
	ClientPC->Fire();
	TestTrue(TEXT("blocked death restart enters retryable WaitingForSpawn"),
		Worlds.TickUntil([ServerPC, ClientPC]()
		{
			return !ServerPC->GetPawn()
				&& ServerPC->GetPlayerStateMachineState()
					== EWoTPlayerStateMachineState::WaitingForSpawn
				&& ClientPC->GetPlayerStateMachineState()
					== EWoTPlayerStateMachineState::WaitingForSpawn;
		}, 120));
	for (int32 Retry = 0; Retry < 3; ++Retry)
	{
		ClientPC->Fire();
		Worlds.TickAll(6);
	}
	TestNull(TEXT("permanently blocked repeated fire remains safely pawnless"),
		ServerPC->GetPawn());
	TestEqual(TEXT("permanently blocked retry preserves WaitingForSpawn"),
		ServerPC->GetPlayerStateMachineState(),
		EWoTPlayerStateMachineState::WaitingForSpawn);

	Blocker->SetActorLocation(StartLocation + FVector(-3000.0, 0.0, 0.0));
	ClientPC->Fire();
	TestTrue(TEXT("next real fire input respawns after the start clears"),
		Worlds.TickUntil([ServerPC]() { return ServerPC->GetPawn() != nullptr; }, 120));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33ReflectedProjectileRemoteNetSpec,
	"WoT.RC33.P15.ReflectedProjectileRemoteNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33ReflectedProjectileRemoteNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 reflected net-temporary projectile replacement"));
	if (!TestTrue(TEXT("reflection dedicated server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector(-8000.0, -2000.0, 5000.0), FRotator::ZeroRotator,
		RC33AlwaysSpawn());
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector(-8000.0, 2000.0, 5000.0), FRotator::ZeroRotator,
		RC33AlwaysSpawn());
	if (!TestTrue(TEXT("original caster client connects"),
			Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("third-party client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	int32 ClientExplosionSpawns[2] = {0, 0};
	FDelegateHandle ClientExplosionHandles[2];
	for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
	{
		ClientExplosionHandles[ClientIndex] =
			Worlds.Clients[ClientIndex].GetWorld()->AddOnActorSpawnedHandler(
				FOnActorSpawned::FDelegate::CreateLambda(
					[&ClientExplosionSpawns, ClientIndex](AActor* Spawned)
					{
						if (IsValid(Spawned) && Spawned->IsA<AWoTExplosionEffect>())
						{
							++ClientExplosionSpawns[ClientIndex];
						}
					}));
	}
	ON_SCOPE_EXIT
	{
		for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
		{
			if (UWorld* ClientWorld = Worlds.Clients[ClientIndex].GetWorld())
			{
				ClientWorld->RemoveOnActorSpawnedHandler(
					ClientExplosionHandles[ClientIndex]);
			}
		}
	};

	UWorld* ServerWorld = Worlds.Server.GetWorld();
	AWoTPlayerController* CasterPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* Caster = CasterPC
		? Cast<AWoTCharacter>(CasterPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("original caster authority pawn"), Caster)) return false;
	Caster->SetActorLocation(FVector(0.0, 0.0, 5000.0));
	Caster->ForceNetUpdate();

	AWoTNPCCharacter* ReflectHost = WoTCreatureData::SpawnCreature(
		*ServerWorld, TEXT("Soldier"), FVector(2000.0, 0.0, 5000.0),
		1, true, AWoTNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("production Soldier reflect host"), ReflectHost)) return false;
	ReflectHost->bAlwaysRelevant = true;
	ReflectHost->SetActorRotation(FRotator::ZeroRotator);
	ReflectHost->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	ReflectHost->GetCharacterMovement()->StopMovementImmediately();
	if (AController* ReflectController = ReflectHost->GetController())
	{
		ReflectController->StopMovement();
		ReflectController->SetActorTickEnabled(false);
	}
	UWoTShieldDeflectBehavior* Shield =
		ReflectHost->FindComponentByClass<UWoTShieldDeflectBehavior>();
	if (!TestNotNull(TEXT("production Soldier shield behavior"), Shield)
		|| !TestNotNull(TEXT("production Soldier shield weapon"),
			ReflectHost->GetMeleeWeapon()))
	{
		return false;
	}
	Shield->EnableDeflect();
	TestTrue(TEXT("production Soldier shield is actively deflecting"),
		Shield->IsDeflectEnabled());
	AWoTNPCCharacter* ClientSoldiers[2] = {nullptr, nullptr};
	TestTrue(TEXT("ordered Soldier reflector description reaches both clients"),
		Worlds.TickUntil([&]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				ClientSoldiers[Index] = FindRC33Creature(
					Worlds.Clients[Index].GetWorld(), TEXT("Soldier"));
			}
			return ClientSoldiers[0] && ClientSoldiers[1]
				&& !ClientSoldiers[0]->TouchReflectorDescriptions.IsEmpty()
				&& !ClientSoldiers[1]->TouchReflectorDescriptions.IsEmpty();
		}, 120));
	UWoTShieldReturnToSenderReflector* SoldierReflector =
		ReflectHost->FindComponentByClass<UWoTShieldReturnToSenderReflector>();
	if (!TestNotNull(TEXT("production Soldier installed shield reflector"),
		SoldierReflector)) return false;

	AWoTFireballProjectile* Incoming = ServerWorld->SpawnActor<AWoTFireballProjectile>(
		FVector(500.0, 0.0, 5000.0), FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestNotNull(TEXT("authority incoming deflectable weave"), Incoming)) return false;
	Incoming->SetInstigator(Caster);
	Incoming->SourcePawn = Caster;
	Incoming->SetIgnoredPawn(Caster);
	Incoming->bAlwaysRelevant = true;
	Incoming->Damage = 17;
	Incoming->ProjectileMovement->Velocity = FVector(1000.0, 0.0, 0.0);

	auto FindForwardProjectile = [](UWorld* World)
	{
		for (TActorIterator<AWoTGenericProjectile> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetClass() == AWoTFireballProjectile::StaticClass()
				&& It->ProjectileMovement
				&& It->ProjectileMovement->Velocity.X > 100.0f)
			{
				return *It;
			}
		}
		return static_cast<AWoTGenericProjectile*>(nullptr);
	};
	AWoTGenericProjectile* CasterIncoming = nullptr;
	AWoTGenericProjectile* ObserverIncoming = nullptr;
	TestTrue(TEXT("both clients receive the original net-temporary weave"),
		Worlds.TickUntil([&]()
		{
			CasterIncoming = FindForwardProjectile(Worlds.Clients[0].GetWorld());
			ObserverIncoming = FindForwardProjectile(Worlds.Clients[1].GetWorld());
			return CasterIncoming && ObserverIncoming;
		}, 120));
	ClientExplosionSpawns[0] = 0;
	ClientExplosionSpawns[1] = 0;
	const TWeakObjectPtr<AWoTGenericProjectile> CasterIncomingCopy(CasterIncoming);
	const TWeakObjectPtr<AWoTGenericProjectile> ObserverIncomingCopy(ObserverIncoming);

	AWoTNPCWeapon* ShieldWeapon = ReflectHost->GetMeleeWeapon();
	const int32 ShieldHealthBefore = ShieldWeapon ? ShieldWeapon->Health : INDEX_NONE;
	TestTrue(TEXT("real projectile movement reaches the production Soldier"),
		Worlds.TickUntil([Incoming]()
		{
			return !IsValid(Incoming) || Incoming->IsActorBeingDestroyed();
		}, 360));
	TestEqual(TEXT("production Soldier redirects exactly one damage event"),
		ShieldWeapon ? ShieldWeapon->Health : INDEX_NONE,
		ShieldHealthBefore - 17);
	TestEqual(TEXT("server records exactly one Soldier redirect damage event"),
		SoldierReflector->RedirectImpactDamageCountForTest, 1);
	Caster->SetActorLocation(FVector(0.0, 10000.0, 5000.0));
	Caster->ForceNetUpdate();
	int32 ServerReplacementCount = 0;
	for (TActorIterator<AWoTGenericProjectile> It(ServerWorld); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed()
			&& It->GetClass() == AWoTFireballProjectile::StaticClass())
		{
			++ServerReplacementCount;
		}
	}
	TestEqual(TEXT("authority retains exactly one replacement projectile"),
		ServerReplacementCount, 1);
	auto FindReturnedProjectile = [](UWorld* World)
	{
		for (TActorIterator<AWoTGenericProjectile> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetClass() == AWoTFireballProjectile::StaticClass()
				&& It->ProjectileMovement
				&& It->ProjectileMovement->Velocity.X < -100.0f)
			{
				return *It;
			}
		}
		return static_cast<AWoTGenericProjectile*>(nullptr);
	};
	AWoTGenericProjectile* CasterReturned = nullptr;
	AWoTGenericProjectile* ObserverReturned = nullptr;
	TestTrue(TEXT("returned replacement is visible to caster and third-party client"),
		Worlds.TickUntil([&]()
		{
			CasterReturned = FindReturnedProjectile(Worlds.Clients[0].GetWorld());
			ObserverReturned = FindReturnedProjectile(Worlds.Clients[1].GetWorld());
			return CasterReturned && ObserverReturned;
		}, 240));
	TestTrue(TEXT("caster sees a distinct reflected projectile"),
		CasterReturned && CasterReturned != CasterIncoming);
	TestTrue(TEXT("observer sees a distinct reflected projectile"),
		ObserverReturned && ObserverReturned != ObserverIncoming);
	// Each client retires its own net-temporary copy when that copy touches the
	// Soldier. The authority's replacement can replicate a tick or two before a
	// client's copy, spawned a net update after the server's, gets there (an
	// intermittent Mac failure when this was checked the instant the
	// replacement appeared), so the copy has a short bound to retire in.
	auto Retired = [](const TWeakObjectPtr<AWoTGenericProjectile>& Copy)
	{
		return !Copy.IsValid() || Copy->IsActorBeingDestroyed();
	};
	Worlds.TickUntil([&]()
	{
		return Retired(CasterIncomingCopy) && Retired(ObserverIncomingCopy);
	}, 30);
	for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
	{
		TestTrue(*FString::Printf(TEXT("client %d silently retired the original"),
			ClientIndex), Retired(ClientIndex == 0 ? CasterIncomingCopy
				: ObserverIncomingCopy));
		TestEqual(*FString::Printf(TEXT("client %d emitted no false explosion actor"),
			ClientIndex), ClientExplosionSpawns[ClientIndex], 0);
		int32 ReturnedProjectiles = 0;
		for (TActorIterator<AWoTGenericProjectile> It(
			Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
		{
			ReturnedProjectiles += IsValid(*It) && !It->IsActorBeingDestroyed()
				&& It->GetClass() == AWoTFireballProjectile::StaticClass()
				&& It->GetVelocity().X < -100.0 ? 1 : 0;
		}
		TestEqual(*FString::Printf(TEXT("client %d has only one returned replacement"),
			ClientIndex), ReturnedProjectiles, 1);
	}

	// Plain ReturnToSender uses the same replicated presentation descriptor and
	// authority replacement path, without the Soldier shield-damage rider.
	for (TActorIterator<AWoTGenericProjectile> It(ServerWorld); It; ++It)
		if (IsValid(*It)) It->Destroy();
	for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
	{
		for (TActorIterator<AWoTGenericProjectile> It(
			Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
		{
			if (IsValid(*It)) It->Destroy();
		}
	}
	Worlds.TickAll(3);
	Caster->SetActorLocation(FVector(0.0, 0.0, 5000.0));
	Caster->ForceNetUpdate();
	AWoTCharacter* RTSHost = ServerWorld->SpawnActor<AWoTCharacter>(
		FVector(2000, 3000, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	RTSHost->bAlwaysRelevant = true;
	RTSHost->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTReturnToSenderReflector* RTS =
		NewObject<UWoTReturnToSenderReflector>(RTSHost);
	RTS->Install(RTSHost);
	RTSHost->ForceNetUpdate();
	TestTrue(TEXT("plain ReturnToSender description reaches both clients"),
		Worlds.TickUntil([&]()
		{
			for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
			{
				bool bFound = false;
				for (TActorIterator<AWoTCharacter> It(
					Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
				{
					bFound |= It->GetLocalRole() == ROLE_SimulatedProxy
						&& FVector::DistSquared(It->GetActorLocation(),
							FVector(2000, 3000, 5000)) < FMath::Square(100.0)
						&& !It->TouchReflectorDescriptions.IsEmpty();
				}
				if (!bFound) return false;
			}
			return true;
		}, 120));
	ClientExplosionSpawns[0] = 0;
	ClientExplosionSpawns[1] = 0;
	AWoTFireballProjectile* RTSIncoming = ServerWorld->SpawnActor<AWoTFireballProjectile>(
		FVector(500, 3000, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	RTSIncoming->SetInstigator(Caster);
	RTSIncoming->SourcePawn = Caster;
	RTSIncoming->SetIgnoredPawn(Caster);
	RTSIncoming->bAlwaysRelevant = true;
	AWoTGenericProjectile* ClientRTSOriginals[2] = {nullptr, nullptr};
	TestTrue(TEXT("both clients receive the plain ReturnToSender original"),
		Worlds.TickUntil([&]()
		{
			for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
			{
				ClientRTSOriginals[ClientIndex] = FindForwardProjectile(
					Worlds.Clients[ClientIndex].GetWorld());
			}
			return ClientRTSOriginals[0] && ClientRTSOriginals[1];
		}, 120));
	TestTrue(TEXT("plain ReturnToSender is reached by real projectile movement"),
		Worlds.TickUntil([RTSIncoming]()
		{
			return !IsValid(RTSIncoming) || RTSIncoming->IsActorBeingDestroyed();
		}, 360));
	Caster->SetActorLocation(FVector(0.0, 10000.0, 5000.0));
	Caster->ForceNetUpdate();
	int32 PlainServerReplacementCount = 0;
	for (TActorIterator<AWoTFireballProjectile> It(ServerWorld); It; ++It)
	{
		PlainServerReplacementCount += IsValid(*It)
			&& !It->IsActorBeingDestroyed() ? 1 : 0;
	}
	TestEqual(TEXT("plain RTS authority retains exactly one replacement"),
		PlainServerReplacementCount, 1);
	AWoTGenericProjectile* ClientRTSReturned[2] = {nullptr, nullptr};
	TestTrue(TEXT("plain RTS replacement reaches both clients"),
		Worlds.TickUntil([&]()
		{
			for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
			{
				ClientRTSReturned[ClientIndex] = FindReturnedProjectile(
					Worlds.Clients[ClientIndex].GetWorld());
			}
			return ClientRTSReturned[0] && ClientRTSReturned[1];
		}, 120));
	for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
	{
		int32 Live = 0;
		for (TActorIterator<AWoTFireballProjectile> It(
			Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
			Live += IsValid(*It) && !It->IsActorBeingDestroyed() ? 1 : 0;
		TestEqual(*FString::Printf(TEXT("plain RTS client %d has one live replacement"),
			ClientIndex), Live, 1);
		TestFalse(*FString::Printf(TEXT("plain RTS client %d retired its original"),
			ClientIndex), IsValid(ClientRTSOriginals[ClientIndex]));
		TestEqual(*FString::Printf(TEXT("plain RTS client %d emitted no explosion actor"),
			ClientIndex), ClientExplosionSpawns[ClientIndex], 0);
	}

	// Absorb retires the original silently on every renderer and creates no
	// replacement actor.
	for (TActorIterator<AWoTGenericProjectile> It(ServerWorld); It; ++It)
		if (IsValid(*It)) It->Destroy();
	for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
	{
		for (TActorIterator<AWoTGenericProjectile> It(
			Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
		{
			if (IsValid(*It)) It->Destroy();
		}
	}
	Worlds.TickAll(3);
	Caster->SetActorLocation(FVector(0.0, 0.0, 5000.0));
	Caster->ForceNetUpdate();
	AWoTCharacter* AbsorbHost = ServerWorld->SpawnActor<AWoTCharacter>(
		FVector(2000, -3000, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	AbsorbHost->bAlwaysRelevant = true;
	AbsorbHost->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(AbsorbHost);
	Absorb->Install(AbsorbHost);
	AbsorbHost->ForceNetUpdate();
	TestTrue(TEXT("Absorb description reaches both clients"),
		Worlds.TickUntil([&]()
		{
			for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
			{
				bool bFound = false;
				for (TActorIterator<AWoTCharacter> It(
					Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
				{
					bFound |= It->GetLocalRole() == ROLE_SimulatedProxy
						&& FVector::DistSquared(It->GetActorLocation(),
							FVector(2000, -3000, 5000)) < FMath::Square(100.0)
						&& !It->TouchReflectorDescriptions.IsEmpty();
				}
				if (!bFound) return false;
			}
			return true;
		}, 120));
	ClientExplosionSpawns[0] = 0;
	ClientExplosionSpawns[1] = 0;
	AWoTFireballProjectile* AbsorbIncoming =
		ServerWorld->SpawnActor<AWoTFireballProjectile>(
			FVector(500, -3000, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	AbsorbIncoming->SetInstigator(Caster);
	AbsorbIncoming->SourcePawn = Caster;
	AbsorbIncoming->SetIgnoredPawn(Caster);
	AbsorbIncoming->bAlwaysRelevant = true;
	AWoTGenericProjectile* ClientAbsorbOriginals[2] = {nullptr, nullptr};
	TestTrue(TEXT("both clients receive the Absorb original"),
		Worlds.TickUntil([&]()
		{
			for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
			{
				ClientAbsorbOriginals[ClientIndex] = FindForwardProjectile(
					Worlds.Clients[ClientIndex].GetWorld());
			}
			return ClientAbsorbOriginals[0] && ClientAbsorbOriginals[1];
		}, 120));
	// Exercise the client-side descriptor while the one-shot is still installed.
	// Server worlds tick before clients: under a loaded full suite the authority
	// could consume Absorb and replicate its removal before one simulated copy
	// reached the host, turning this into a descriptor-lifetime race instead of
	// the intended local-retirement check. Keep the real client trajectories but
	// advance their already-replicated copies ahead of the authority projectile.
	for (AWoTGenericProjectile* ClientOriginal : ClientAbsorbOriginals)
	{
		if (ClientOriginal)
		{
			ClientOriginal->SetActorLocation(FVector(1900, -3000, 5000),
				false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
	TestTrue(TEXT("Absorb is reached by real projectile movement"),
		Worlds.TickUntil([AbsorbIncoming]()
		{
			return !IsValid(AbsorbIncoming) || AbsorbIncoming->IsActorBeingDestroyed();
		}, 360));
	Worlds.TickAll(10);
	int32 AbsorbServerProjectiles = 0;
	for (TActorIterator<AWoTFireballProjectile> It(ServerWorld); It; ++It)
	{
		AbsorbServerProjectiles += IsValid(*It)
			&& !It->IsActorBeingDestroyed() ? 1 : 0;
	}
	TestEqual(TEXT("Absorb authority creates no replacement"),
		AbsorbServerProjectiles, 0);
	for (int32 ClientIndex = 0; ClientIndex < 2; ++ClientIndex)
	{
		int32 Live = 0;
		for (TActorIterator<AWoTFireballProjectile> It(
			Worlds.Clients[ClientIndex].GetWorld()); It; ++It)
			Live += IsValid(*It) && !It->IsActorBeingDestroyed() ? 1 : 0;
		TestEqual(*FString::Printf(TEXT("Absorb client %d has no replacement"),
			ClientIndex), Live, 0);
		TestFalse(*FString::Printf(TEXT("Absorb client %d retired its original"),
			ClientIndex), IsValid(ClientAbsorbOriginals[ClientIndex]));
		TestEqual(*FString::Printf(TEXT("Absorb client %d emitted no explosion actor"),
			ClientIndex), ClientExplosionSpawns[ClientIndex], 0);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33LightningAuthorityNetSpec,
	"WoT.RC33.P4.LightningAuthorityNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33LightningAuthorityNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 retail Lightning authority strike replication"));
	if (!TestTrue(TEXT("Lightning dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("Lightning presentation client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}

	UWorld* ServerWorld = Worlds.Server.GetWorld();
	TestEqual(TEXT("Lightning fixture is a real dedicated server"),
		ServerWorld->GetNetMode(), NM_DedicatedServer);
	AWoTLightning* ServerLightning = ServerWorld->SpawnActor<AWoTLightning>(
		FVector(0.0f, 0.0f, 5000.0f), FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestNotNull(TEXT("authority production Lightning"), ServerLightning))
	{
		return false;
	}
	ServerLightning->bAlwaysRelevant = true;
	ServerLightning->ForceNetUpdate();

	AWoTLightning* ClientLightning = nullptr;
	TestTrue(TEXT("simulated proxy receives the production Lightning actor"),
		Worlds.TickUntil([&Worlds, &ClientLightning]()
		{
			TActorIterator<AWoTLightning> It(Worlds.Clients[0].GetWorld());
			if (It)
			{
				ClientLightning = *It;
				return true;
			}
			return false;
		}, 120));
	if (!ClientLightning) return false;
	TestEqual(TEXT("client Lightning is a simulated proxy"),
		ClientLightning->GetLocalRole(), ROLE_SimulatedProxy);

	const uint16 Before = ClientLightning->StrikeSerial;
	ServerLightning->WoTTrigger(ServerLightning, nullptr);
	TestFalse(TEXT("dedicated server never enables Lightning presentation"),
		ServerLightning->Light->IsVisible());
	TestTrue(TEXT("authority strike serial and flash reach the simulated proxy"),
		Worlds.TickUntil([ClientLightning, Before]()
		{
			return ClientLightning->StrikeSerial != Before
				&& ClientLightning->Light->Intensity > 0.0f;
		}, 120));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33TriggeredWindClientNetSpec,
	"WoT.RC33.PresentationB.P4.TriggeredWindClientNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33TriggeredWindClientNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Battle2_03.Wind0 client-local impulse edges"));
	if (!TestTrue(TEXT("Wind dedicated server loads"), Worlds.Server.IsLoaded()))
		return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-8000, -2000, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-8000, 2000, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("Wind owner-view client connects"),
			Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("Wind simulated-view client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}

	// Load exactly the authenticated Battle2_03 source and all twenty decoded
	// class-default objects through the same production contract/import boundary
	// as WoT.World.Projector.RealRetailCensus. The timed mutations below happen
	// only after its real Wind0 has projected.
	FWoTWorldProjectionSource ProjectionSource;
	TArray<FWoTRuntimeWorldActor> ProjectionRecords;
	TUniquePtr<FWoTRuntimeClassDefaults> Defaults;
	FString ProjectionError;
	if (!TestTrue(TEXT("authenticated Battle2_03 Wind0 projection source loads"),
		LoadRC33Battle203WindProjectionSource(
			ProjectionSource, ProjectionRecords, Defaults, ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}
	TestEqual(TEXT("authenticated source selects the real Battle2_03 map"),
		ProjectionSource.MapIdentity, FString(TEXT("Battle2_03")));
	TestEqual(TEXT("authenticated source contains only selected Wind0"),
		ProjectionSource.Records.Num(), 1);
	if (!ProjectionSource.Records.IsEmpty())
		TestEqual(TEXT("selected authenticated record is Battle2_03.Wind0"),
			ProjectionSource.Records[0].Identity,
			FString(TEXT("Battle2_03.Wind0")));
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("Battle2_03 Wind record projects"),
		FWoTWorldProjector::ProjectDeferred(*ServerWorld, ProjectionSource,
			*Defaults, Projection, ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}
	AWoTWind* ServerWind = nullptr;
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (AWoTWind* Wind = Cast<AWoTWind>(Deferred)) ServerWind = Wind;
		Deferred->FinishSpawning(Deferred->GetActorTransform());
	}
	if (!TestNotNull(TEXT("real projected Battle2_03 Wind"), ServerWind))
		return false;
	ServerWind->bAlwaysRelevant = true;
	ServerWind->ForceNetUpdate();
	TestEqual(TEXT("real Battle2_03 Wind targets heal-zone sprayers"),
		ServerWind->EventName, FName(TEXT("heal_zone")));
	TestEqual(TEXT("real Battle2_03 Wind is trigger-controlled"),
		ServerWind->InitialState, FName(TEXT("TriggerControl")));
	TestFalse(TEXT("real Battle2_03 Wind starts disabled"),
		ServerWind->IsEnabledForTesting());

	AWoTParticleSprayer* ClientSprayers[2] = {nullptr, nullptr};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		UWorld* ClientWorld = Worlds.Clients[Index].GetWorld();
		ClientSprayers[Index] = ClientWorld->SpawnActor<AWoTParticleSprayer>(
			FVector(0, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
		ClientSprayers[Index]->TagName = TEXT("heal_zone");
		ClientSprayers[Index]->Gravity = FVector::ZeroVector;
	}
	AWoTWind* ClientWinds[2] = {nullptr, nullptr};
	TestTrue(TEXT("both rendering clients receive the triggered Wind record"),
		Worlds.TickUntil([&]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				TActorIterator<AWoTWind> It(Worlds.Clients[Index].GetWorld());
				if (It)
				{
					ClientWinds[Index] = *It;
				}
			}
			return ClientWinds[0] && ClientWinds[1]
				&& ClientWinds[0]->EventName == TEXT("heal_zone")
				&& ClientWinds[1]->EventName == TEXT("heal_zone");
		}, 120));
	TestEqual(TEXT("Wind fixture is a real dedicated server"),
		ServerWorld->GetNetMode(), NM_DedicatedServer);
	TestEqual(TEXT("dedicated server creates no local sprayer presentation"),
		TActorIterator<AWoTParticleSprayer>(ServerWorld) ? 1 : 0, 0);

	ServerWind->WoTTrigger(ServerWind, nullptr);
	TestTrue(TEXT("trigger edge enables both clients and applies local impulses"),
		Worlds.TickUntil([&]()
		{
			return ClientWinds[0] && ClientWinds[1]
				&& ClientWinds[0]->IsEnabledForTesting()
				&& ClientWinds[1]->IsEnabledForTesting()
				&& !ClientSprayers[0]->Gravity.IsNearlyZero()
				&& !ClientSprayers[1]->Gravity.IsNearlyZero();
		}, 120));
	TestTrue(TEXT("real Wind0's fixed 1200 speed reaches both client renderers"),
		FMath::IsNearlyEqual(ClientWinds[0]->GetLastWindForTesting().Size(), 1200.0f)
		&& FMath::IsNearlyEqual(ClientWinds[1]->GetLastWindForTesting().Size(), 1200.0f));
	ServerWind->WoTUnTrigger(ServerWind, nullptr);
	TestTrue(TEXT("restore edge disables clients and removes their local wind"),
		Worlds.TickUntil([&]()
		{
			return !ClientWinds[0]->IsEnabledForTesting()
				&& !ClientWinds[1]->IsEnabledForTesting()
				&& ClientSprayers[0]->Gravity.IsNearlyZero()
				&& ClientSprayers[1]->Gravity.IsNearlyZero();
		}, 120));
	auto TickWindSeconds = [&Worlds](float Seconds)
	{
		Worlds.TickAll(FMath::CeilToInt(
			Seconds / Worlds.GetTickDeltaSeconds()));
	};

	// Delay delivery beyond a 50 ms pulse. Receipt must still produce one real
	// presentation update with bOn and the gravity offset applied, then restore;
	// a RepNotify counter alone cannot satisfy this case.
	ServerWind->InitialState = TEXT("TriggerTimed");
	ServerWind->TimerDuration = 0.05f;
	const int32 ShortPulseBefore[2] = {
		ClientWinds[0]->GetTimedTriggerObserveCountForTesting(),
		ClientWinds[1]->GetTimedTriggerObserveCountForTesting()};
	ServerWind->WoTTrigger(ServerWind, nullptr);
	// The harness worlds maintain independent time origins when only the server
	// ticks. Backdate this test pair to emulate a one-second transport delay,
	// then withhold client processing until the real authority pulse has ended.
	ServerWind->SetTimedTriggerServerTimeForTesting(
		ServerWind->GetTimedTriggerServerTimeForTesting() - 1.0f);
	for (int32 Tick = 0; Tick < 30 && ServerWind->IsEnabledForTesting(); ++Tick)
	{
		Worlds.TickServer();
		++GFrameCounter;
	}
	TestFalse(TEXT("authority has restored before clients receive the short pulse"),
		ServerWind->IsEnabledForTesting());
	Worlds.TickClients();
	TestTrue(TEXT("delayed expired pair is received by both clients"),
		ClientWinds[0]->GetTimedTriggerObserveCountForTesting()
			> ShortPulseBefore[0]
		&& ClientWinds[1]->GetTimedTriggerObserveCountForTesting()
			> ShortPulseBefore[1]);
	TestTrue(TEXT("expired pair presents an actual bOn transition for one update"),
		ClientWinds[0]->IsEnabledForTesting()
		&& ClientWinds[1]->IsEnabledForTesting());
	TestTrue(TEXT("expired pair applies actual client-local gravity for that update"),
		!ClientSprayers[0]->Gravity.IsNearlyZero()
		&& !ClientSprayers[1]->Gravity.IsNearlyZero());
	for (AWoTWind* ClientWind : ClientWinds) ClientWind->Tick(0.0f);
	TestTrue(TEXT("expired pair restores exactly after its one presentation update"),
		!ClientWinds[0]->IsEnabledForTesting()
		&& !ClientWinds[1]->IsEnabledForTesting()
		&& ClientSprayers[0]->Gravity.IsNearlyZero()
		&& ClientSprayers[1]->Gravity.IsNearlyZero());
	Worlds.TickClients();
	TestTrue(TEXT("authoritative off state cannot leave a stale timed gravity offset"),
		!ClientWinds[0]->IsEnabledForTesting()
		&& !ClientWinds[1]->IsEnabledForTesting()
		&& ClientSprayers[0]->Gravity.IsNearlyZero()
		&& ClientSprayers[1]->Gravity.IsNearlyZero());

	// The authority's off level must clean clients immediately even while their
	// replicated pair still has a future local deadline. Do not manually tick
	// either Wind before observing the replicated cleanup.
	ServerWind->TimerDuration = 0.50f;
	ServerWind->WoTTrigger(ServerWind, nullptr);
	TestTrue(TEXT("authority-off case starts both client Winds with gravity"),
		Worlds.TickUntil([&]()
		{
			return ClientWinds[0]->IsEnabledForTesting()
				&& ClientWinds[1]->IsEnabledForTesting()
				&& !ClientSprayers[0]->Gravity.IsNearlyZero()
				&& !ClientSprayers[1]->Gravity.IsNearlyZero();
		}, 120));
	const float FutureRestoreAt[2] = {
		ClientWinds[0]->GetRestoreAtForTesting(),
		ClientWinds[1]->GetRestoreAtForTesting()};
	TestTrue(TEXT("both clients hold a future local timed deadline"),
		FutureRestoreAt[0] > Worlds.Clients[0].GetWorld()->GetTimeSeconds()
		&& FutureRestoreAt[1] > Worlds.Clients[1].GetWorld()->GetTimeSeconds());
	for (int32 Tick = 0; Tick < 120 && ServerWind->IsEnabledForTesting(); ++Tick)
	{
		Worlds.TickServer();
		++GFrameCounter;
	}
	TestFalse(TEXT("authority reaches off while client clocks remain before their deadlines"),
		ServerWind->IsEnabledForTesting());
	TestTrue(TEXT("client clocks are still before the saved local deadlines"),
		Worlds.Clients[0].GetWorld()->GetTimeSeconds() < FutureRestoreAt[0]
		&& Worlds.Clients[1].GetWorld()->GetTimeSeconds() < FutureRestoreAt[1]);
	Worlds.TickClients();
	TestTrue(TEXT("replicated authority-off immediately removes both local winds"),
		!ClientWinds[0]->IsEnabledForTesting()
		&& !ClientWinds[1]->IsEnabledForTesting()
		&& ClientSprayers[0]->Gravity.IsNearlyZero()
		&& ClientSprayers[1]->Gravity.IsNearlyZero());
	TestTrue(TEXT("authority-off cleanup happens before either local deadline"),
		Worlds.Clients[0].GetWorld()->GetTimeSeconds() < FutureRestoreAt[0]
		&& Worlds.Clients[1].GetWorld()->GetTimeSeconds() < FutureRestoreAt[1]);

	// RepNotify is carried by the pair, not just the serial. A newer timestamp
	// at the same serial must apply exactly one refreshed pair on each client.
	ServerWind->TimerDuration = 0.50f;
	ServerWind->WoTTrigger(ServerWind, nullptr);
	TestTrue(TEXT("timestamp-only case starts a new timed pair"),
		Worlds.TickUntil([&]()
		{
			return ClientWinds[0]->IsEnabledForTesting()
				&& ClientWinds[1]->IsEnabledForTesting();
		}, 120));
	const uint8 TimestampSerial = ServerWind->GetTimedTriggerSerialForTesting();
	const int32 TimestampBefore[2] = {
		ClientWinds[0]->GetTimedTriggerObserveCountForTesting(),
		ClientWinds[1]->GetTimedTriggerObserveCountForTesting()};
	const float NewerTimestamp =
		ServerWind->GetTimedTriggerServerTimeForTesting() + 0.05f;
	ServerWind->SetTimedTriggerServerTimeForTesting(NewerTimestamp);
	TestTrue(TEXT("newer timestamp at unchanged serial reaches both clients"),
		Worlds.TickUntil([&]()
		{
			return ClientWinds[0]->GetTimedTriggerObserveCountForTesting()
					> TimestampBefore[0]
				&& ClientWinds[1]->GetTimedTriggerObserveCountForTesting()
					> TimestampBefore[1];
		}, 120));
	TestTrue(TEXT("timestamp-only update preserves the serial everywhere"),
		ServerWind->GetTimedTriggerSerialForTesting() == TimestampSerial
		&& ClientWinds[0]->GetTimedTriggerSerialForTesting() == TimestampSerial
		&& ClientWinds[1]->GetTimedTriggerSerialForTesting() == TimestampSerial);
	TestTrue(TEXT("timestamp-only RepNotify applies exactly one refreshed pair"),
		ClientWinds[0]->GetTimedTriggerObserveCountForTesting()
			== TimestampBefore[0] + 1
		&& ClientWinds[1]->GetTimedTriggerObserveCountForTesting()
			== TimestampBefore[1] + 1);
	TestTrue(TEXT("timestamp-only pair publishes the newer server time"),
		ClientWinds[0]->GetTimedTriggerServerTimeForTesting() > NewerTimestamp - KINDA_SMALL_NUMBER
		&& ClientWinds[1]->GetTimedTriggerServerTimeForTesting() > NewerTimestamp - KINDA_SMALL_NUMBER);
	TestTrue(TEXT("timestamp-only refreshed pair eventually restores cleanly"),
		Worlds.TickUntil([&]()
		{
			return !ClientWinds[0]->IsEnabledForTesting()
				&& !ClientWinds[1]->IsEnabledForTesting()
				&& ClientSprayers[0]->Gravity.IsNearlyZero()
				&& ClientSprayers[1]->Gravity.IsNearlyZero();
		}, 120));

	ServerWind->TimerDuration = 0.30f;
	ServerWind->WoTTrigger(ServerWind, nullptr);
	TestTrue(TEXT("refresh case starts both timed client Winds"),
		Worlds.TickUntil([&]()
		{
			return ClientWinds[0]->IsEnabledForTesting()
				&& ClientWinds[1]->IsEnabledForTesting();
		}, 60));
	TickWindSeconds(0.18f);
	ServerWind->WoTTrigger(ServerWind, nullptr);
	TickWindSeconds(0.18f);
	TestTrue(TEXT("retrigger refresh keeps both timed Winds past the old deadline"),
		ClientWinds[0]->IsEnabledForTesting()
		&& ClientWinds[1]->IsEnabledForTesting());
	TestTrue(TEXT("refreshed timed Wind eventually restores"),
		Worlds.TickUntil([&]()
		{
			return !ClientWinds[0]->IsEnabledForTesting()
				&& !ClientWinds[1]->IsEnabledForTesting();
		}, 120));

	ServerWind->SetTimedTriggerSerialForTesting(255);
	const int32 WrapBefore =
		ClientWinds[0]->GetTimedTriggerObserveCountForTesting();
	ServerWind->TimerDuration = 6.0f;
	ServerWind->WoTTrigger(ServerWind, nullptr);
	TestTrue(TEXT("uint8 timed serial wraps 255 to the nonzero value 1"),
		Worlds.TickUntil([&]()
		{
			return ServerWind->GetTimedTriggerSerialForTesting() == 1
				&& ClientWinds[0]->GetTimedTriggerSerialForTesting() == 1
				&& ClientWinds[1]->GetTimedTriggerSerialForTesting() == 1
				&& ClientWinds[0]->GetTimedTriggerObserveCountForTesting()
					> WrapBefore
				&& ClientWinds[0]->IsEnabledForTesting()
				&& ClientWinds[1]->IsEnabledForTesting();
		}, 120));

	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-8000, 5000, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	const float PulseServerStart =
		ServerWind->GetTimedTriggerServerTimeForTesting();
	TickWindSeconds(2.0f);
	TestTrue(TEXT("a substantial part of the live pulse elapses before late join"),
		ServerWorld->GetGameState()->GetServerWorldTimeSeconds()
			- PulseServerStart >= 1.75f);
	TestTrue(TEXT("late client connects during the wrapped nonzero pulse"),
		Worlds.CreateAndConnectClient());
	AWoTParticleSprayer* LateSprayer =
		Worlds.Clients[2].GetWorld()->SpawnActor<AWoTParticleSprayer>(
			FVector(0, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	LateSprayer->TagName = TEXT("heal_zone");
	LateSprayer->Gravity = FVector::ZeroVector;
	AWoTWind* LateWind = nullptr;
	TestTrue(TEXT("late joiner reconstructs the wrapped live timed pair"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTWind> It(
				Worlds.Clients[2].GetWorld()); It; ++It)
			{
				if (It->TagName == TEXT("heal_zone_wind")) LateWind = *It;
			}
			return LateWind
				&& LateWind->GetTimedTriggerSerialForTesting() == 1
				&& LateWind->IsEnabledForTesting()
				&& !LateSprayer->Gravity.IsNearlyZero();
		}, 120));
	const float LateJoinTime = Worlds.Clients[2].GetWorld()->GetTimeSeconds();
	int32 RestoreTicks[4] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
	const int32 MaxRestoreTicks = FMath::CeilToInt(
		ServerWind->TimerDuration * 1.5f / Worlds.GetTickDeltaSeconds());
	for (int32 Tick = 1; Tick <= MaxRestoreTicks; ++Tick)
	{
		Worlds.TickAll();
		if (RestoreTicks[0] == INDEX_NONE && !ServerWind->IsEnabledForTesting())
			RestoreTicks[0] = Tick;
		if (RestoreTicks[1] == INDEX_NONE && !ClientWinds[0]->IsEnabledForTesting())
			RestoreTicks[1] = Tick;
		if (RestoreTicks[2] == INDEX_NONE && !ClientWinds[1]->IsEnabledForTesting())
			RestoreTicks[2] = Tick;
		if (RestoreTicks[3] == INDEX_NONE && LateWind
			&& !LateWind->IsEnabledForTesting())
		{
			RestoreTicks[3] = Tick;
		}
		if (RestoreTicks[0] != INDEX_NONE && RestoreTicks[1] != INDEX_NONE
			&& RestoreTicks[2] != INDEX_NONE && RestoreTicks[3] != INDEX_NONE)
		{
			break;
		}
	}
	TestTrue(TEXT("server, existing clients, and late joiner all record a restore frame"),
		RestoreTicks[0] != INDEX_NONE && RestoreTicks[1] != INDEX_NONE
		&& RestoreTicks[2] != INDEX_NONE && RestoreTicks[3] != INDEX_NONE);
	if (RestoreTicks[0] != INDEX_NONE && RestoreTicks[1] != INDEX_NONE
		&& RestoreTicks[2] != INDEX_NONE && RestoreTicks[3] != INDEX_NONE)
	{
		const int32 Earliest = FMath::Min(
			FMath::Min(RestoreTicks[0], RestoreTicks[1]),
			FMath::Min(RestoreTicks[2], RestoreTicks[3]));
		const int32 Latest = FMath::Max(
			FMath::Max(RestoreTicks[0], RestoreTicks[1]),
			FMath::Max(RestoreTicks[2], RestoreTicks[3]));
		TestTrue(TEXT("late joiner restores within one tick of the shared deadline"),
			Latest - Earliest <= 1);
		const float LateElapsed =
			Worlds.Clients[2].GetWorld()->GetTimeSeconds() - LateJoinTime;
		TestTrue(TEXT("late joiner uses remaining time, well below a fresh full timer"),
			LateElapsed < ServerWind->TimerDuration * 0.75f);
	}
	TestTrue(TEXT("the shared restore removes every client-local gravity offset"),
		LateSprayer->Gravity.IsNearlyZero()
		&& ClientSprayers[0]->Gravity.IsNearlyZero()
		&& ClientSprayers[1]->Gravity.IsNearlyZero());
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33CreaturePresentationClientNetSpec,
	"WoT.RC33.PresentationB.P3P6.CreaturePresentationClientNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33CreaturePresentationClientNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Myrddraal and Legion client presentation cues"));
	if (!TestTrue(TEXT("creature-presentation dedicated server loads"),
		Worlds.Server.IsLoaded())) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-9000, -2500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-9000, 2500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("Myrddraal owner-view client connects"),
			Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("Myrddraal simulated-view client connects"),
			Worlds.CreateAndConnectClient())) return false;
	// The decoded 4-second timed-sprayer checks advance this world well beyond
	// the original cue-only spec. Keep both real client viewers in range instead
	// of allowing their unpossessed Entry pawns to fall away from later markers.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		APlayerController* ServerPC = Worlds.GetServerPlayerControllerOfClient(Index);
		AWoTCharacter* Viewer = ServerPC
			? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
		if (Viewer)
		{
			// Stay inside Myrddraal's 2048 / sqrt(1.3) hearing gate while
			// exercising the replicated client presentation path.
			Viewer->SetActorLocation(FVector(500.0, Index ? 100.0 : -100.0, 5000.0));
			Viewer->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Viewer->GetCharacterMovement()->StopMovementImmediately();
			Viewer->ForceNetUpdate();
		}
	}

	AWoTNPCCharacter* ServerMyrddraal = WoTCreatureData::SpawnCreature(
		*ServerWorld, TEXT("Myrddraal"), FVector(0, 0, 5000), 1, true,
		AWoTNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("production Myrddraal"), ServerMyrddraal)) return false;
	ServerMyrddraal->bAlwaysRelevant = true;
	ServerMyrddraal->ForceNetUpdate();
	AWoTNPCCharacter* ClientMyrddraal[2] = {nullptr, nullptr};
	TestTrue(TEXT("both clients receive the production Myrddraal proxy"),
		Worlds.TickUntil([&]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
				ClientMyrddraal[Index] = FindRC33Creature(
					Worlds.Clients[Index].GetWorld(), TEXT("Myrddraal"));
			return ClientMyrddraal[0] && ClientMyrddraal[1];
		}, 120));
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Myrddraal client %d is simulated"), Index),
			ClientMyrddraal[Index]->GetLocalRole(), ROLE_SimulatedProxy);
	}

	const FVector EnterLocation(100, 200, 5100);
	ServerMyrddraal->EmitTeleportPresentation(1, EnterLocation);
	TestTrue(TEXT("both clients create enter sprayers and teleport sound markers"),
		Worlds.TickUntil([&]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				if (!ClientMyrddraal[Index]->GetTeleportEnterSprayerForTest()
					|| CountRC33SoundMarkers(Worlds.Clients[Index].GetWorld(),
						TEXT("Myrddraal_Teleport")) == 0) return false;
			}
			return true;
		}, 120));
	TestNull(TEXT("dedicated server creates no Myrddraal enter sprayer"),
		ServerMyrddraal->GetTeleportEnterSprayerForTest());
	const FWoTSprayerBundleRow* TeleportRow =
		WoTWeaveFX::Find(TEXT("MyrddraalTeleportSprayer2"));
	if (!TestNotNull(TEXT("production Myrddraal sprayer row"), TeleportRow))
		return false;
	TestEqual(TEXT("Myrddraal trigger state is decoded"),
		TeleportRow->InitialState, FName(TEXT("TriggerTimed")));
	TestFalse(TEXT("Myrddraal initial state is decoded off"),
		TeleportRow->bInitiallyOn);
	TestTrue(TEXT("Myrddraal CDO bOn is decoded on"), TeleportRow->bOn);
	TestEqual(TEXT("Myrddraal timer duration is decoded"),
		TeleportRow->TimerDuration, 4.0f);
	auto TickSeconds = [&Worlds](float Seconds)
	{
		Worlds.TickAll(FMath::CeilToInt(
			Seconds / Worlds.GetTickDeltaSeconds()));
	};
	TickSeconds(2.0f);
	ServerMyrddraal->EmitTeleportPresentation(1, EnterLocation);
	Worlds.TickAll(2);
	TickSeconds(2.5f);
	TestTrue(TEXT("retrigger refresh keeps both client sprayers live past the old deadline"),
		ClientMyrddraal[0]->GetTeleportEnterSprayerForTest()->bOn
		&& ClientMyrddraal[1]->GetTeleportEnterSprayerForTest()->bOn);
	TickSeconds(2.0f);
	TestFalse(TEXT("decoded timed restore expires client sprayer zero"),
		ClientMyrddraal[0]->GetTeleportEnterSprayerForTest()->bOn);
	TestFalse(TEXT("decoded timed restore expires client sprayer one"),
		ClientMyrddraal[1]->GetTeleportEnterSprayerForTest()->bOn);
	const FVector ExitLocation(900, 300, 5200);
	ServerMyrddraal->EmitTeleportPresentation(2, ExitLocation);
	TestTrue(TEXT("both clients create exit sprayers anchored at the replicated exit location"),
		Worlds.TickUntil([&]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const AWoTParticleSprayer* Sprayer =
					ClientMyrddraal[Index]->GetTeleportExitSprayerForTest();
				// ActorRotator animates the particle origin within the authored
				// 24uu radius / 72uu height envelope around the replicated center.
				if (!Sprayer || FVector::Dist(Sprayer->GetActorLocation(), ExitLocation) > 100.0f)
					return false;
			}
			return true;
		}, 120));
	ServerMyrddraal->EmitTeleportPresentation(3, ExitLocation);
	TestTrue(TEXT("both clients receive the reappear sound marker"),
		Worlds.TickUntil([&]()
		{
			return CountRC33SoundMarkers(Worlds.Clients[0].GetWorld(),
					TEXT("Myrddraal_Reappear")) > 0
				&& CountRC33SoundMarkers(Worlds.Clients[1].GetWorld(),
					TEXT("Myrddraal_Reappear")) > 0;
		}, 120));

	ServerMyrddraal->SetScaleGlowPresentation(0.75f);
	TestTrue(TEXT("first Myrddraal fade glow reaches both client bodies"),
		Worlds.TickUntil([&]()
		{
			return FMath::IsNearlyEqual(ClientMyrddraal[0]->ScaleGlow, 0.75f)
				&& FMath::IsNearlyEqual(ClientMyrddraal[1]->ScaleGlow, 0.75f);
		}, 120));
	ServerMyrddraal->SetScaleGlowPresentation(0.25f);
	TestTrue(TEXT("second Myrddraal fade glow reaches both client bodies"),
		Worlds.TickUntil([&]()
		{
			return FMath::IsNearlyEqual(ClientMyrddraal[0]->ScaleGlow, 0.25f)
				&& FMath::IsNearlyEqual(ClientMyrddraal[1]->ScaleGlow, 0.25f);
		}, 120));
	for (int32 Index = 0; Index < 2; ++Index)
	{
		float MaterialGlow = 0.0f;
		TestTrue(*FString::Printf(TEXT("client %d body has a live MLScaleGlow parameter"),
			Index), ReadRC33BodyGlow(ClientMyrddraal[Index], MaterialGlow));
		TestEqual(*FString::Printf(TEXT("client %d material glow follows the fade"),
			Index), MaterialGlow, 0.25f, 0.001f);
	}
	ServerMyrddraal->Destroy();
	TestTrue(TEXT("destroying Myrddraal removes every client-local sprayer and rotator"),
		Worlds.TickUntil([&Worlds]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				for (TActorIterator<AWoTParticleSprayer> It(
					Worlds.Clients[Index].GetWorld()); It; ++It)
				{
					if (It->ConfigName == TEXT("MyrddraalTeleportSprayer2"))
						return false;
				}
				for (TActorIterator<AWoTActorRotator> It(
					Worlds.Clients[Index].GetWorld()); It; ++It)
				{
					if (IsValid(*It)) return false;
				}
			}
			return true;
		}, 120));

	// LegionProjectile's spawn, hurt and death cues are authority events carried
	// by replicated sound markers to both rendering clients. Retail is silent
	// when a spirit dies against world geometry or another non-pawn.
	// Keep these coordinate-agnostic emitters inside the already-settled viewer
	// positions above; an immediate second teleport can precede the connection's
	// updated network-view location and incorrectly cull a valid fixture cue.
	auto SpawnSpirit = [ServerWorld](const FVector& Location)
	{
		AWoTLegionProjectile* Spirit = ServerWorld->SpawnActor<AWoTLegionProjectile>(
			Location, FRotator::ZeroRotator, RC33AlwaysSpawn());
		Spirit->bAlwaysRelevant = true;
		Spirit->ForceNetUpdate();
		return Spirit;
	};
	AWoTLegionProjectile* HurtSpirit = SpawnSpirit(FVector(0, 0, 5000));
	FDamageEvent DamageEvent;
	HurtSpirit->TakeDamage(1.0f, DamageEvent, nullptr, HurtSpirit);
	AWoTLegionProjectile* DeadSpirit = SpawnSpirit(FVector(500, 0, 5000));
	DeadSpirit->TakeDamage(100.0f, DamageEvent, nullptr, DeadSpirit);
	AWoTLegionProjectile* WallSpirit = SpawnSpirit(FVector(1000, 0, 5000));
	AStaticMeshActor* Wall = ServerWorld->SpawnActor<AStaticMeshActor>(
		FVector(1100, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	WallSpirit->ProcessTouch(Wall, Wall->GetActorLocation());
	TestTrue(TEXT("both clients receive Legion spawn and hurt/death cues but no wall cue"),
		Worlds.TickUntil([&]()
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				UWorld* ClientWorld = Worlds.Clients[Index].GetWorld();
				if (CountRC33SoundMarkers(ClientWorld, TEXT("Spirit_Spawn1")) < 3
					|| CountRC33SoundMarkers(ClientWorld, TEXT("Spirit_TakeDamage")) < 2
					|| CountRC33SoundMarkers(ClientWorld, TEXT("Spirit_HitWall1")) != 0)
					return false;
			}
			return true;
		}, 180));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33MyrddraalDecodedSprayerMutationNetSpec,
	"WoT.WorldFX.PresentationB.P4.MyrddraalDecodedSprayerMutationNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33MyrddraalDecodedSprayerMutationNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FWoTRetailFamiliesRestoreScope RestoreFamilies;
	const FWoTSprayerBundleRow* Installed =
		WoTWeaveFX::Find(TEXT("MyrddraalTeleportSprayer2"));
	if (!TestNotNull(TEXT("authenticated Myrddraal sprayer row is installed"),
		Installed)) return false;
	const FWoTSprayerBundleRow RetailRow = *Installed;

	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(
		TEXT("mutated decoded Myrddraal sprayer rows on a dedicated client"));
	if (!TestTrue(TEXT("sprayer-mutation dedicated server loads"),
		Worlds.Server.IsLoaded())) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-8000, 0, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("real Myrddraal presentation client connects"),
		Worlds.CreateAndConnectClient())) return false;

	int32 MutatedBeginPlayCount = 0;
	bool bObservedInitialOn = true;
	EWoTParticleTriggerMode ObservedTriggerMode = EWoTParticleTriggerMode::None;
	WoTSetParticleSprayerLifecycleObserverForTesting(
		[&](const TCHAR* Boundary, const AActor& Actor, const UActorComponent*)
		{
			const AWoTParticleSprayer* Sprayer = Cast<AWoTParticleSprayer>(&Actor);
			if (FCString::Strcmp(Boundary, TEXT("BeginPlay")) != 0 || !Sprayer
				|| Sprayer->ConfigName != TEXT("MyrddraalTeleportSprayer2")
				|| Sprayer->GetNetMode() == NM_DedicatedServer) return;
			++MutatedBeginPlayCount;
			bObservedInitialOn = Sprayer->bOn;
			ObservedTriggerMode = Sprayer->TriggerMode;
		});
	ON_SCOPE_EXIT { WoTSetParticleSprayerLifecycleObserverForTesting({}); };
	auto InstallMutation = [&RetailRow](
		TFunctionRef<void(FWoTSprayerBundleRow&)> Mutate)
	{
		FWoTSprayerBundleRow Row = RetailRow;
		Mutate(Row);
		FWoTWeaveFXSet Set;
		Set.Bundles.Add(TEXT("MyrddraalTeleportSprayer2"), MoveTemp(Row));
		FWoTWeaveFXBuilder::Install(MoveTemp(Set));
	};
	auto TickClientTimer = [&Worlds](float Seconds)
	{
		// Only the decoded timer contract is under test. Drive the real client
		// timer manager without advancing hundreds of unrelated network frames.
		++GFrameCounter;
		Worlds.Clients[0].GetWorld()->GetTimerManager().Tick(Seconds);
	};
	auto SpawnPresentation = [this, &Worlds, ServerWorld](int32 CaseIndex,
		AWoTNPCCharacter*& OutServer, AWoTNPCCharacter*& OutClient,
		AWoTParticleSprayer*& OutSprayer)
	{
		OutServer = WoTCreatureData::SpawnCreature(*ServerWorld, TEXT("Myrddraal"),
			FVector(1000.0 * CaseIndex, 0, 5000), 1, true,
			AWoTNPCCharacter::StaticClass());
		if (!TestNotNull(FString::Printf(TEXT("case %d production Myrddraal"),
			CaseIndex), OutServer)) return false;
		OutServer->bAlwaysRelevant = true;
		OutServer->ForceNetUpdate();
		OutClient = nullptr;
		if (!TestTrue(FString::Printf(TEXT("case %d client receives Myrddraal"),
			CaseIndex), Worlds.TickUntil([&]()
		{
			OutClient = FindRC33Creature(
				Worlds.Clients[0].GetWorld(), TEXT("Myrddraal"));
			return OutClient != nullptr;
		}, 120))) return false;
		OutServer->EmitTeleportPresentation(1,
			FVector(1000.0 * CaseIndex, 100, 5100));
		OutSprayer = nullptr;
		return TestTrue(FString::Printf(TEXT("case %d real presentation spawns sprayer"),
			CaseIndex), Worlds.TickUntil([&]()
		{
			OutSprayer = OutClient->GetTeleportEnterSprayerForTest();
			return OutSprayer != nullptr;
		}, 120));
	};
	auto DestroyPresentation = [this, &Worlds](int32 CaseIndex,
		AWoTNPCCharacter* ServerMyrddraal)
	{
		ServerMyrddraal->Destroy();
		return TestTrue(FString::Printf(
			TEXT("case %d destroys its client-local sprayer and rotator"), CaseIndex),
			Worlds.TickUntil([&Worlds]()
			{
				for (TActorIterator<AWoTParticleSprayer> It(
					Worlds.Clients[0].GetWorld()); It; ++It)
				{
					if (It->ConfigName == TEXT("MyrddraalTeleportSprayer2"))
						return false;
				}
				for (TActorIterator<AWoTActorRotator> It(
					Worlds.Clients[0].GetWorld()); It; ++It)
				{
					if (IsValid(*It)) return false;
				}
				return true;
			}, 120));
	};
	AWoTNPCCharacter* ServerMyrddraal = nullptr;
	AWoTNPCCharacter* ClientMyrddraal = nullptr;
	AWoTParticleSprayer* Sprayer = nullptr;

	// A decoded duration mutation must reach the live timer assignment in
	// WoTWeaveFX::SpawnLocal; restoring its old four-second literal fails here.
	InstallMutation([](FWoTSprayerBundleRow& Row) { Row.TimerDuration = 0.20f; });
	TestTrue(TEXT("duration mutation drives the real Myrddraal path"),
		SpawnPresentation(1, ServerMyrddraal, ClientMyrddraal, Sprayer));
	if (Sprayer)
	{
		TestEqual(TEXT("live sprayer receives mutated TimerDuration"),
			Sprayer->TriggerSeconds, 0.20f);
		TestTrue(TEXT("mutated-duration sprayer starts its timed emission"),
			Sprayer->bOn);
		TickClientTimer(0.10f);
		TestTrue(TEXT("mutated-duration sprayer remains live before expiry"),
			Sprayer->bOn);
		TickClientTimer(0.11f);
		TestFalse(TEXT("mutated-duration sprayer expires at the decoded deadline"),
			Sprayer->bOn);
	}
	DestroyPresentation(1, ServerMyrddraal);

	InstallMutation([](FWoTSprayerBundleRow& Row)
	{
		Row.InitialState = TEXT("TriggerControl");
	});
	TestTrue(TEXT("InitialState mutation drives the real Myrddraal path"),
		SpawnPresentation(2, ServerMyrddraal, ClientMyrddraal, Sprayer));
	if (Sprayer)
	{
		TestEqual(TEXT("live sprayer receives mutated InitialState"),
			Sprayer->TriggerMode, EWoTParticleTriggerMode::Control);
		TickClientTimer(RetailRow.TimerDuration + 0.25f);
		TestTrue(TEXT("TriggerControl mutation has no timed restore"), Sprayer->bOn);
	}
	DestroyPresentation(2, ServerMyrddraal);

	InstallMutation([](FWoTSprayerBundleRow& Row) { Row.bInitiallyOn = true; });
	TestTrue(TEXT("bInitiallyOn mutation drives the real Myrddraal path"),
		SpawnPresentation(3, ServerMyrddraal, ClientMyrddraal, Sprayer));
	if (Sprayer)
	{
		TestTrue(TEXT("live sprayer receives mutated bInitiallyOn"),
			Sprayer->bInitiallyOn);
		TestFalse(TEXT("timed trigger flips mutated bInitiallyOn off"), Sprayer->bOn);
		TickClientTimer(RetailRow.TimerDuration + 0.25f);
		TestTrue(TEXT("timed restore returns to mutated bInitiallyOn"), Sprayer->bOn);
	}
	DestroyPresentation(3, ServerMyrddraal);

	const int32 BeginPlayBeforeBOn = MutatedBeginPlayCount;
	InstallMutation([](FWoTSprayerBundleRow& Row) { Row.bOn = false; });
	TestTrue(TEXT("bOn mutation drives the real Myrddraal path"),
		SpawnPresentation(4, ServerMyrddraal, ClientMyrddraal, Sprayer));
	TestTrue(TEXT("live SpawnLocal BeginPlay observes the mutated bOn value"),
		MutatedBeginPlayCount > BeginPlayBeforeBOn && !bObservedInitialOn);
	TestEqual(TEXT("bOn-only mutation retains the decoded timed state"),
		ObservedTriggerMode, EWoTParticleTriggerMode::Timed);
	if (Sprayer)
		TestTrue(TEXT("real Myrddraal trigger turns the bOn-mutated sprayer on"),
			Sprayer->bOn);
	DestroyPresentation(4, ServerMyrddraal);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33FadedWarderDisguiseClientNetSpec,
	"WoT.RC33.PresentationB.P13.FadedWarderDisguiseClientNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33FadedWarderDisguiseClientNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("faded Warder disguise owner and simulated proxy"));
	if (!TestTrue(TEXT("disguise dedicated server loads"), Worlds.Server.IsLoaded()))
		return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-9000, -2500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-9000, 2500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("disguise owner connects"), Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("disguise third-party client connects"),
			Worlds.CreateAndConnectClient())) return false;

	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* ServerDriver = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("authority disguise driver"), ServerDriver)) return false;
	const FVector DriverLocation(1000, 0, 5000);
	ServerDriver->SetActorLocation(DriverLocation);
	AWoTNPCCharacter* Warder = WoTCreatureData::SpawnCreature(*ServerWorld,
		TEXT("Warder"), FVector(0, 0, 5000), 1, true,
		AWoTNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("production Warder source"), Warder)) return false;
	Warder->Visibility = 16;
	Warder->AmbientGlow = 77;
	Warder->SetScaleGlowPresentation(0.35f);
	Warder->SetFadeSkinIndex(0);
	const UWoTVertexAnimComponent* ServerWarderBody =
		Cast<UWoTVertexAnimComponent>(Warder->GetMesh());
	TestTrue(TEXT("dedicated source retains normalized live fade skins without rendering"),
		ServerWarderBody && !ServerWarderBody->GetRuntimeSkinOverrides().IsEmpty());
	TestTrue(TEXT("authority copies the faded Warder's live appearance"),
		ServerDriver->ApplyDisguiseFrom(Warder));
	ServerDriver->ForceNetUpdate();

	AWoTPlayerController* OwnerPC = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* OwnerView = OwnerPC
		? Cast<AWoTCharacter>(OwnerPC->GetPawn()) : nullptr;
	AWoTCharacter* ThirdPartyView = nullptr;
	TestTrue(TEXT("owner and third party receive the faded Warder appearance"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTCharacter> It(Worlds.Clients[1].GetWorld()); It; ++It)
			{
				if (!It->IsNPCPawn()
					&& FVector::DistSquared(It->GetActorLocation(), DriverLocation) < 100.0f)
				{
					ThirdPartyView = *It;
					break;
				}
			}
			return OwnerView && ThirdPartyView
				&& OwnerView->IsDisguised() && ThirdPartyView->IsDisguised();
		}, 180));
	// OnRep_Disguise can land before the client's BeginPlay binds the base look.
	// Sample only after both views have begun play, so a BeginPlay rebind that
	// wipes the disguise cannot hide behind an early read.
	TestTrue(TEXT("owner and third-party disguise views have begun play"),
		Worlds.TickUntil([&]()
		{
			return OwnerView && ThirdPartyView
				&& OwnerView->HasActorBegunPlay() && ThirdPartyView->HasActorBegunPlay();
		}, 180));
	Worlds.TickAll(2);
	for (const TPair<const TCHAR*, AWoTCharacter*> View : {
		TPair<const TCHAR*, AWoTCharacter*>(TEXT("owner"), OwnerView),
		TPair<const TCHAR*, AWoTCharacter*>(TEXT("third party"), ThirdPartyView)})
	{
		if (!View.Value) continue;
		const FWoTDisguiseState& State = View.Value->GetDisguiseStateForTest();
		TestEqual(*FString::Printf(TEXT("%s receives modulated fade style"), View.Key),
			State.SourceStyle, uint8(2));
		TestEqual(*FString::Printf(TEXT("%s receives live AmbientGlow"), View.Key),
			State.SourceAmbientGlow, uint8(77));
		TestEqual(*FString::Printf(TEXT("%s receives live ScaleGlow"), View.Key),
			State.SourceScaleGlow, 0.35f, 0.001f);
		TestTrue(*FString::Printf(TEXT("%s receives normalized fade skin identities"),
			View.Key), !State.SourceRuntimeSkins.IsEmpty());
		const UWoTVertexAnimComponent* Body =
			Cast<UWoTVertexAnimComponent>(View.Value->GetMesh());
		TestTrue(*FString::Printf(TEXT("%s applies live fade skins to the body"), View.Key),
			Body && !Body->GetRuntimeSkinOverrides().IsEmpty());
	}

	AWoTCharacter* SightProbe = ServerWorld->SpawnActor<AWoTCharacter>(
		DriverLocation + FVector(0, 2500, 0), FRotator::ZeroRotator,
		RC33AlwaysSpawn());
	SightProbe->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	TestEqual(TEXT("disguise copies the faded Warder visibility for AI"),
		ServerDriver->Visibility, uint8(16));
	TestFalse(TEXT("AI sight sees the disguised pawn with the faded Warder range"),
		WoTSight::OmniSight(SightProbe, ServerDriver,
			WoTSight::VisibilityOf(ServerDriver)));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33LateJoinDisguiseRelayerSpec,
	"WoT.RC33.PresentationB.P13.LateJoinDisguiseRelayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33LateJoinDisguiseRelayerSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	// The disguise source carries a runtime logical-id mesh, the guarded morph
	// carrier. The fixture bytes are synthetic; the id's mesh name resolves to the
	// editor Warder carrier through SetVertexMeshLogicalIds on every role.
	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!TestTrue(TEXT("late-join mesh fixture loads"), FFileHelper::LoadFileToArray(
			MeshBytes, *WoTRepositoryOrReferencePath(
				TEXT("Tools/dist/fixtures/cache-v3-mesh-plain.bin"))))
		|| !TestTrue(TEXT("late-join animation fixture loads"), FFileHelper::LoadFileToArray(
			AnimationBytes, *WoTRepositoryOrReferencePath(
				TEXT("Tools/dist/fixtures/cache-v3-animation-plain.bin")))))
		return false;
	auto Decoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
		MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
		TEXT("cache/mesh/WoTPawns__Warder"), TEXT("cache/animation/WoTPawns__Warder"));
	if (Decoded.IsError())
	{
		AddError(Decoded.GetError().ToString());
		return false;
	}
	auto SameSkins = [](const TMap<int32, FSoftObjectPath>& A,
		const TMap<int32, FSoftObjectPath>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (const TPair<int32, FSoftObjectPath>& Pair : A)
		{
			const FSoftObjectPath* Other = B.Find(Pair.Key);
			if (!Other || *Other != Pair.Value) return false;
		}
		return true;
	};

	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("late joiner receives an already active disguise"));
	if (!TestTrue(TEXT("late-join dedicated server loads"), Worlds.Server.IsLoaded()))
		return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-9000, -2500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-9000, 2500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("late-join disguise owner connects"),
		Worlds.CreateAndConnectClient())) return false;
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* ServerDriver = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	const UWoTVertexAnimComponent* ServerBody = ServerDriver
		? Cast<UWoTVertexAnimComponent>(ServerDriver->GetMesh()) : nullptr;
	if (!TestNotNull(TEXT("late-join authority driver body"), ServerBody)) return false;
	const FVector DriverLocation(1000, 0, 5000);
	ServerDriver->SetActorLocation(DriverLocation);
	const FName BaseMesh = ServerBody->GetDisplayMeshName();
	TestTrue(TEXT("authority driver has a non-Warder base mesh"),
		!BaseMesh.IsNone() && !BaseMesh.ToString().StartsWith(TEXT("Warder")));

	AWoTNPCCharacter* Warder = WoTCreatureData::SpawnCreature(*ServerWorld,
		TEXT("Warder"), FVector(0, 0, 5000), 1, true,
		AWoTNPCCharacter::StaticClass());
	UWoTVertexAnimComponent* WarderBody = Warder
		? Cast<UWoTVertexAnimComponent>(Warder->GetMesh()) : nullptr;
	if (!TestNotNull(TEXT("late-join Warder source body"), WarderBody)) return false;
	TestTrue(TEXT("source binds the runtime logical-id mesh"),
		WarderBody->BindRuntimeVertexMeshForTesting(Decoded.GetValue()));
	Warder->SetFadeSkinIndex(0);
	TestTrue(TEXT("source carries live fade skins"),
		!WarderBody->GetRuntimeSkinOverrides().IsEmpty());
	TestTrue(TEXT("authority disguises before the late joiner exists"),
		ServerDriver->ApplyDisguiseFrom(Warder));
	ServerDriver->ForceNetUpdate();
	const FName DisguiseMesh = ServerBody->GetDisplayMeshName();
	TestTrue(TEXT("authority morphs through the logical-id mesh"),
		DisguiseMesh.ToString().StartsWith(TEXT("Warder")));
	const TMap<int32, FSoftObjectPath> DisguiseSkins = ServerBody->GetRuntimeSkinOverrides();
	TestFalse(TEXT("authority wears the live fade skins"), DisguiseSkins.IsEmpty());

	AWoTPlayerController* OwnerPC = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	TestTrue(TEXT("owner sees its own disguise first"), Worlds.TickUntil([&]()
	{
		const AWoTCharacter* OwnerView = OwnerPC
			? Cast<AWoTCharacter>(OwnerPC->GetPawn()) : nullptr;
		return OwnerView && OwnerView->IsDisguised();
	}, 180));
	if (!TestTrue(TEXT("late joiner connects after the disguise"),
		Worlds.CreateAndConnectClient())) return false;
	AWoTCharacter* LateView = nullptr;
	TestTrue(TEXT("late joiner receives the disguised pawn and begins play"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTCharacter> It(Worlds.Clients[1].GetWorld()); It; ++It)
			{
				if (!It->IsNPCPawn() && It->GetLocalRole() == ROLE_SimulatedProxy
					&& FVector::DistSquared(It->GetActorLocation(), DriverLocation) < 100.0f)
				{
					LateView = *It;
					break;
				}
			}
			return LateView && LateView->IsDisguised() && LateView->HasActorBegunPlay();
		}, 240));
	Worlds.TickAll(2);
	const UWoTVertexAnimComponent* LateBody = LateView
		? Cast<UWoTVertexAnimComponent>(LateView->GetMesh()) : nullptr;
	if (!TestNotNull(TEXT("late joiner body"), LateBody)) return false;
	TestEqual(TEXT("late joiner keeps the disguise mesh through BeginPlay"),
		LateBody->GetDisplayMeshName(), DisguiseMesh);
	TestTrue(TEXT("late joiner keeps the live fade skins through BeginPlay"),
		SameSkins(LateBody->GetRuntimeSkinOverrides(), DisguiseSkins));

	ServerDriver->RevertDisguise();
	ServerDriver->ForceNetUpdate();
	TestTrue(TEXT("late joiner receives the revert"), Worlds.TickUntil([&]()
	{
		return !LateView->IsDisguised();
	}, 180));
	Worlds.TickAll(2);
	TestEqual(TEXT("authority restores its base mesh"),
		ServerBody->GetDisplayMeshName(), BaseMesh);
	TestEqual(TEXT("late joiner restores the real base mesh, not an unbound body"),
		LateBody->GetDisplayMeshName(), BaseMesh);
	TestTrue(TEXT("late joiner restores the base skins"),
		SameSkins(LateBody->GetRuntimeSkinOverrides(), ServerBody->GetRuntimeSkinOverrides()));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33SpewDescriptorClientNetSpec,
	"WoT.RC33.PresentationB.P4.SpewDescriptorClientNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33SpewDescriptorClientNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetTickInSeconds(1.0f / 60.0f);
	Worlds.SetDescription(TEXT("decoded Spew descriptor reaches both clients"));
	if (!TestTrue(TEXT("Spew dedicated server loads"), Worlds.Server.IsLoaded()))
		return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-1500, -500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-1500, 500, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("Spew owner-view client connects"),
			Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("Spew simulated-view client connects"),
			Worlds.CreateAndConnectClient())) return false;
	// Keep both network viewers within the loud chunk's retail hearing gate.
	// The original fixture relied on globally replicated sound markers even
	// though its Entry pawns could fall outside R / sqrt(1.3).
	for (int32 Index = 0; Index < 2; ++Index)
	{
		APlayerController* ServerPC = Worlds.GetServerPlayerControllerOfClient(Index);
		AWoTCharacter* Viewer = ServerPC
			? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
		if (Viewer)
		{
			Viewer->SetActorLocation(FVector(250.0, Index ? 100.0 : -100.0, 400.0));
			Viewer->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Viewer->GetCharacterMovement()->StopMovementImmediately();
			Viewer->ForceNetUpdate();
		}
	}
	Worlds.TickAll(2);

	// The trigger and its ThingT are projected from the same decoded property
	// shapes used by production maps.  No behavior field is assigned directly
	// to a trigger or chunk in this spec.
	auto NameProperty = [](const TCHAR* Name, const TCHAR* Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Name;
		Property.NameValue = Value;
		return Property;
	};
	auto FloatProperty = [](const TCHAR* Name, float Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Float;
		Property.Value.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
		return Property;
	};
	auto IntProperty = [](const TCHAR* Name, int32 Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Integer;
		Property.Value.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
		return Property;
	};
	auto ByteProperty = [](const TCHAR* Name, uint8 Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Byte;
		Property.Value.Add(Value);
		return Property;
	};
	auto BoolProperty = [](const TCHAR* Name, bool Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Boolean;
		Property.Value.Add(Value ? 1 : 0);
		return Property;
	};
	auto ObjectProperty = [](const TCHAR* Name, const TCHAR* Identity,
		uint32 Slot = 0)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.ArrayIndex = Slot;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		return Property;
	};
	auto VectorProperty = [](const TCHAR* Name, const FVector3f& Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("Vector");
		Property.Value.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
		return Property;
	};
	auto RotatorProperty = [](const TCHAR* Name, int32 Pitch, int32 Yaw,
		int32 Roll)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("Rotator");
		const int32 Values[3] = {Pitch, Yaw, Roll};
		Property.Value.Append(reinterpret_cast<const uint8*>(Values), sizeof(Values));
		return Property;
	};
	auto ThingProperty = [&]()
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = TEXT("Things");
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("ThingT");
		Property.StructMembers = {
			ObjectProperty(TEXT("Actor"), TEXT("WOTDecorations.FixtureBounce")),
			IntProperty(TEXT("Count"), 1),
			ByteProperty(TEXT("DetailLevel"), 0),
			FloatProperty(TEXT("MinSpeed"), 400.0f),
			FloatProperty(TEXT("MaxSpeed"), 400.0f),
			FloatProperty(TEXT("DrawScale"), 1.25f)};
		return Property;
	};

	FWoTRuntimeClassDefaultRecord SpewDefaults;
	SpewDefaults.Identity = TEXT("WOT.SpewTrigger");
	SpewDefaults.Properties = {
		NameProperty(TEXT("Tag"), TEXT("decoded_spew")),
		NameProperty(TEXT("InitialState"), TEXT("TriggerTurnsOn")),
		BoolProperty(TEXT("bEnabled"), false),
		BoolProperty(TEXT("bTumble"), true),
		BoolProperty(TEXT("bInheritVelocity"), true),
		BoolProperty(TEXT("bCollision"), true),
		BoolProperty(TEXT("bSilent"), false),
		FloatProperty(TEXT("SpewVisibilityHeight"), 20000.0f),
		FloatProperty(TEXT("SpewVisibilityRadius"), 20000.0f),
		FloatProperty(TEXT("TimeBetweenSpewsSecs"), 0.01f),
		IntProperty(TEXT("SpewCount"), 1),
		RotatorProperty(TEXT("SpewRotationSpread"), 0, 0, 0)};
	FWoTRuntimeClassDefaultRecord ThingDefaults;
	ThingDefaults.Identity = TEXT("WOTDecorations.FixtureBounce");
	ThingDefaults.Properties = {
		ObjectProperty(TEXT("Mesh"), TEXT("WOTDecorations.FixtureMesh")),
		ObjectProperty(TEXT("MultiSkins"), TEXT("WOTDecorations.Skins.JFixture")),
		ObjectProperty(TEXT("LandSound1"), TEXT("WOTDecorations.BounceFixture")),
		FloatProperty(TEXT("CollisionRadius"), 17.0f),
		FloatProperty(TEXT("CollisionHeight"), 23.0f),
		FloatProperty(TEXT("LifeSpan"), 6.0f),
		FloatProperty(TEXT("DrawScale"), 1.25f),
		FloatProperty(TEXT("MinBounceSoundSpeed"), 300.0f),
		FloatProperty(TEXT("MinFirstHitSpeed"), 2000.0f),
		FloatProperty(TEXT("MinBounceSpeed"), 100.0f),
		FloatProperty(TEXT("SpeedDampeningRatio"), 0.55f),
		FloatProperty(TEXT("RotationDampeningRatio"), 0.31f),
		FloatProperty(TEXT("BaseBounceVolume"), 0.8f),
		FloatProperty(TEXT("BaseBouncePitch"), 1.2f),
		FloatProperty(TEXT("VolumePerturbPercent"), 11.0f),
		FloatProperty(TEXT("PitchPerturbPercent"), 13.0f)};
	FWoTRuntimeClassDefaults Defaults;
	FString ProjectionError;
	if (!TestTrue(TEXT("decoded Spew class defaults load"),
		Defaults.LoadFixture({SpewDefaults, ThingDefaults}, ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}
	auto MakeSpewRecord = [&](const TCHAR* Identity, float X, bool bSilent)
	{
		FWoTRuntimeWorldActor Record;
		Record.Identity = Identity;
		Record.ClassIdentity = TEXT("WOT.SpewTrigger");
		Record.Properties = {
			VectorProperty(TEXT("Location"), FVector3f(X, 0.0f, 400.0f)),
			RotatorProperty(TEXT("Rotation"), -16384, 0, 0),
			BoolProperty(TEXT("bSilent"), bSilent),
			ThingProperty()};
		return Record;
	};
	TArray<FWoTRuntimeWorldActor> SpewRecords;
	SpewRecords.Add(MakeSpewRecord(
		TEXT("DecodedSpewFixture.Loud0"), 0.0f, false));
	SpewRecords.Add(MakeSpewRecord(
		TEXT("DecodedSpewFixture.Silent0"), 500.0f, true));
	FWoTWorldProjectionSource ProjectionSource;
	ProjectionSource.MapIdentity = TEXT("DecodedSpewFixture");
	ProjectionSource.Records = SpewRecords;
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("decoded Spew records project"),
		FWoTWorldProjector::ProjectDeferred(*ServerWorld, ProjectionSource,
			Defaults, Projection, ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}
	AWoTSpewTrigger* LoudTrigger = nullptr;
	AWoTSpewTrigger* SilentTrigger = nullptr;
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (AWoTSpewTrigger* Spew = Cast<AWoTSpewTrigger>(Deferred))
		{
			(Spew->bSilent ? SilentTrigger : LoudTrigger) = Spew;
			Spew->bAlwaysRelevant = true;
		}
		Deferred->FinishSpawning(Deferred->GetActorTransform());
	}
	if (!LoudTrigger || !SilentTrigger)
	{
		for (const FString& Sample : Projection.SkippedSamples)
			AddError(FString::Printf(TEXT("Spew projection skip: %s"), *Sample));
	}
	if (!TestNotNull(TEXT("decoded loud SpewTrigger"), LoudTrigger)
		|| !TestNotNull(TEXT("decoded silent SpewTrigger"), SilentTrigger))
	{
		return false;
	}
	const FTransform FloorTransform(FRotator::ZeroRotator,
		FVector(250.0, 0.0, 0.0));
	AWoTCollisionBlocker* Floor = ServerWorld->SpawnActorDeferred<
		AWoTCollisionBlocker>(AWoTCollisionBlocker::StaticClass(), FloorTransform,
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Floor->Configure(EWoTCollisionBlockerMode::All, 1500.0f, 20.0f);
	Floor->FinishSpawning(FloorTransform);
	WoTRandom::Seed(0x5EED33);
	LoudTrigger->WoTTrigger(nullptr, nullptr);
	AWoTSpewChunk* LoudChunk = nullptr;
	AWoTSpewChunk* ClientLoudChunks[2] = {nullptr, nullptr};
	FVector FirstPre = FVector::ZeroVector;
	FVector FirstPost = FVector::ZeroVector;
	FVector FirstNormal = FVector::ZeroVector;
	int32 FirstHitCount = 0;
	float FirstImpactTime = -1.0f;
	auto CaptureFirstImpact = [&]()
	{
		if (LoudChunk && FirstImpactTime < 0.0f
			&& LoudChunk->GetBlockingHitCountForTest() >= 1)
		{
			FirstPre = LoudChunk->GetLastPreImpactVelocityForTest();
			FirstPost = LoudChunk->GetLastPostImpactVelocityForTest();
			FirstNormal = LoudChunk->GetLastImpactNormalForTest();
			FirstHitCount = LoudChunk->GetBlockingHitCountForTest();
			FirstImpactTime = ServerWorld->GetTimeSeconds();
		}
	};
	TestTrue(TEXT("decoded loud chunk reaches both rendering clients"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTSpewChunk> It(ServerWorld); It; ++It)
				if (IsValid(*It) && It->GetOwner() == LoudTrigger) LoudChunk = *It;
			CaptureFirstImpact();
			for (int32 Index = 0; Index < 2; ++Index)
			{
				for (TActorIterator<AWoTSpewChunk> It(
					Worlds.Clients[Index].GetWorld()); It; ++It)
				{
					if (IsValid(*It) && !It->bSilent) ClientLoudChunks[Index] = *It;
				}
			}
			return LoudChunk && ClientLoudChunks[0] && ClientLoudChunks[1];
		}, 180));
	if (!LoudChunk)
	{
		WoTRandom::Seed(0x7051);
		return false;
	}
	const FWoTSpewThing& DecodedThing = LoudTrigger->Things[0];
	TestTrue(TEXT("decoded descriptor enables chunk world collision"),
		LoudChunk->bCollision);
	TestEqual(TEXT("decoded initial speed reaches runtime movement"),
		LoudChunk->InitialVelocity.Size(), 400.0, 0.5);
	TestFalse(TEXT("dedicated server renders no Spew display"),
		LoudChunk->DisplayMesh->IsVisible());
	for (int32 Index = 0; Index < 2; ++Index)
	{
		AWoTSpewChunk* Chunk = ClientLoudChunks[Index];
		if (!Chunk) continue;
		TestEqual(*FString::Printf(TEXT("client %d retains class identity"), Index),
			Chunk->ClassIdentity, DecodedThing.ClassIdentity);
		TestEqual(*FString::Printf(TEXT("client %d retains mesh identity"), Index),
			Chunk->MeshName, DecodedThing.MeshName);
		TestEqual(*FString::Printf(TEXT("client %d retains exact skin slots"), Index),
			Chunk->SkinIdentities, DecodedThing.SkinIdentities);
		TestEqual(*FString::Printf(TEXT("client %d retains decoded lifetime"), Index),
			Chunk->AuthoredLifeSpan, 6.0f);
		TestEqual(*FString::Printf(TEXT("client %d retains decoded damping"), Index),
			Chunk->SpeedDampeningRatio, 0.55f);
		TestEqual(*FString::Printf(TEXT("client %d retains decoded sound threshold"),
			Index), Chunk->MinBounceSoundSpeed, 300.0f);
		TestTrue(*FString::Printf(TEXT("client %d renders decoded detail level"),
			Index), Chunk->DisplayMesh->IsVisible());
	}

	TestTrue(TEXT("real first blocking hit occurs through swept movement"),
		Worlds.TickUntil([&]()
		{
			CaptureFirstImpact();
			return FirstImpactTime >= 0.0f;
		}, 180));
	const FVector ExpectedReflected = DecodedThing.SpeedDampeningRatio
		* FirstPre.MirrorByVector(FirstNormal);
	const FString FirstImpactDiagnostic = FString::Printf(
		TEXT("first impact is above decoded sound threshold "
			"(FirstPre.Size()=%.3f, hit count=%d, impact time=%.6f)"),
		FirstPre.Size(), FirstHitCount, FirstImpactTime);
	TestTrue(*FirstImpactDiagnostic,
		FirstPre.Size() > DecodedThing.MinBounceSoundSpeed);
	TestTrue(TEXT("blocking-floor normal points upward"),
		FirstNormal.Z > 0.9f);
	TestTrue(TEXT("blocking hit reflects and damps by decoded ratio"),
		FirstPost.Equals(ExpectedReflected, 0.5f));
	TestTrue(TEXT("decoded MinBounceSpeed keeps the first bounce live"),
		LoudChunk->IsBouncingForTest());
	TestEqual(TEXT("above-threshold first hit emits exactly one marker"),
		LoudChunk->GetBounceSoundCountForTest(), 1);
	// Keep the second real collision comfortably inside the decoded six-second
	// lifetime without fabricating a hit call. Derive the next contact from the
	// exact semi-implicit production steps until the real speed is between the
	// decoded stop and sound thresholds, then put a thin blocking plane there.
	// This remains valid for both vertical and lateral reflected trajectories.
	const FVector CurrentVelocity = LoudChunk->GetVelocityForTest();
	const FVector ChunkLocation = LoudChunk->GetActorLocation();
	const float StepSeconds = Worlds.GetTickDeltaSeconds();
	const FVector Gravity = AWoTZoneVolume::GetGravityAt(ServerWorld,
		ChunkLocation);
	FVector ContactVelocity = CurrentVelocity;
	FVector ContactTravel = FVector::ZeroVector;
	bool bFoundContact = false;
	for (int32 Step = 0; Step < 180; ++Step)
	{
		ContactVelocity += Gravity * StepSeconds;
		ContactTravel += ContactVelocity * StepSeconds;
		const float ContactSpeed = ContactVelocity.Size();
		if (ContactSpeed > DecodedThing.MinBounceSpeed
			&& ContactSpeed < DecodedThing.MinBounceSoundSpeed)
		{
			bFoundContact = true;
			break;
		}
	}
	TestTrue(TEXT("derived second contact is between decoded bounce thresholds"),
		bFoundContact && !ContactVelocity.IsNearlyZero());
	const FVector ContactNormal = ContactVelocity.GetSafeNormal();
	const float ChunkSupport = LoudChunk->CollisionRadius
		+ FMath::Max(0.0f, LoudChunk->CollisionHeight - LoudChunk->CollisionRadius)
			* FMath::Abs(ContactNormal.Z);
	constexpr float PlaneHalfThickness = 10.0f;
	AStaticMeshActor* SecondBlocker = SpawnRC33Wall(ServerWorld,
		ChunkLocation + ContactTravel
			+ ContactNormal * (ChunkSupport + PlaneHalfThickness),
		FVector(PlaneHalfThickness, 1500.0f, 1500.0f));
	SecondBlocker->SetActorRotation(ContactNormal.Rotation(),
		ETeleportType::TeleportPhysics);

	TestTrue(TEXT("real second blocking hit falls below decoded sound threshold"),
		Worlds.TickUntil([&]()
		{
			return LoudChunk->GetBlockingHitCountForTest() >= 2;
		}, 180));
	const FVector SecondPre = LoudChunk->GetLastPreImpactVelocityForTest();
	TestTrue(TEXT("second impact speed is below decoded sound threshold"),
		SecondPre.Size() < DecodedThing.MinBounceSoundSpeed);
	TestEqual(TEXT("below-threshold hit emits no additional replicated marker"),
		LoudChunk->GetBounceSoundCountForTest(), 1);
	TestTrue(TEXT("first above-threshold hit reaches both client sound markers"),
		Worlds.TickUntil([&]()
		{
			return CountRC33SoundMarkers(Worlds.Clients[0].GetWorld(),
					TEXT("BounceFixture")) >= 1
				&& CountRC33SoundMarkers(Worlds.Clients[1].GetWorld(),
					TEXT("BounceFixture")) >= 1;
		}, 180));
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TestEqual(*FString::Printf(
			TEXT("client %d receives no additional below-threshold marker"), Index),
			CountRC33SoundMarkers(Worlds.Clients[Index].GetWorld(),
				TEXT("BounceFixture")), 1);
	}

	SilentTrigger->WoTTrigger(nullptr, nullptr);
	AWoTSpewChunk* SilentChunk = nullptr;
	TestTrue(TEXT("decoded silent chunk takes a real blocking hit"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTSpewChunk> It(ServerWorld); It; ++It)
				if (IsValid(*It) && It->GetOwner() == SilentTrigger) SilentChunk = *It;
			return SilentChunk && SilentChunk->GetBlockingHitCountForTest() >= 1;
		}, 180));
	if (!SilentChunk)
	{
		WoTRandom::Seed(0x7051);
		return false;
	}
	TestTrue(TEXT("silent chunk carries decoded bSilent"), SilentChunk->bSilent);
	Worlds.TickAll(10);
	TestEqual(TEXT("silent blocking hit emits no server marker"),
		SilentChunk->GetBounceSoundCountForTest(), 0);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("client %d receives no silent marker"), Index),
			CountRC33SoundMarkers(Worlds.Clients[Index].GetWorld(),
				TEXT("BounceFixture")), 1);
	}

	const TWeakObjectPtr<AWoTSpewChunk> SilentLifetimeProbe = SilentChunk;
	const float SilentAgeAtProbe = SilentChunk->GetGameTimeSinceCreation();
	const float SilentBirthTime = ServerWorld->GetTimeSeconds() - SilentAgeAtProbe;
	TestEqual(TEXT("silent chunk retains decoded lifetime"),
		SilentChunk->AuthoredLifeSpan, 6.0f);
	TestTrue(TEXT("chunk is destroyed at decoded LifeSpan"),
		Worlds.TickUntil([&]() { return !SilentLifetimeProbe.IsValid(); }, 420));
	const float SilentLifetime = ServerWorld->GetTimeSeconds() - SilentBirthTime;
	TestTrue(TEXT("decoded LifeSpan, not a hardcoded timeout, owns destruction"),
		SilentLifetime >= 5.9f && SilentLifetime <= 6.3f);
	WoTRandom::Seed(0x7051);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33DifficultyProductionSpec,
	"WoT.RC33.R1.DifficultyProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33DifficultyProductionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	TestNotNull(TEXT("production game mode"), Game);
	TestEqual(TEXT("shipped Difficulty default"), Game->Difficulty, 1);
	FString Error;
	Game->InitGame(TEXT("RC33"), TEXT("?Difficulty=3"), Error);
	AWoTNPCCharacter* Pawn = World->SpawnActor<AWoTNPCCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, RC33AlwaysSpawn());
	TestEqual(TEXT("URL Difficulty reaches production pawn Skill initialization"),
		Pawn->Skill, 3.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33PawnOwnedSightStateSpec,
	"WoT.RC33.R4.PawnOwnedSightState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33PawnOwnedSightStateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* A = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, RC33AlwaysSpawn());
	AWoTCharacter* B = World->SpawnActor<AWoTCharacter>(
		FVector(0, 100, 0), FRotator::ZeroRotator, RC33AlwaysSpawn());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(100, 0, 0), FRotator::ZeroRotator, RC33AlwaysSpawn());
	WoTSight::SetTraceOverrideForTesting(
		[](const FVector&, const FVector&) { return true; });
	WoTSight::ConeSight(A, 0.0f, 2500.0f, Target, 128);
	TestTrue(TEXT("observer A owns its toggled LOSFlag"), A->bSightLOSFlag);
	TestFalse(TEXT("observer B state is independent and untouched"), B->bSightLOSFlag);
	TestEqual(TEXT("stimulus is stored on observer A"), A->SightStimulus, 1.0f);
	WoTSight::ResetStateForTesting(A);
	TestFalse(TEXT("reset clears only the pawn-owned state"), A->bSightLOSFlag);
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33GruntDamageReactionSpec,
	"WoT.RC33.G4.GruntDamageReactions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33GruntDamageReactionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Victim = SpawnRC33NPC(World, FVector::ZeroVector, 1);
	AWoTTestNPCCharacter* Enemy = SpawnRC33NPC(World, FVector(100, 0, 0), 2);
	WoTTest::TickWorld(World, 0.1f);
	FWoTDamageEvent Hurt;
	Hurt.WoTDamageType = TEXT("hurt");
	Victim->TakeDamage(10.0f, Hurt, Enemy->GetController(), Enemy);
	TestEqual(TEXT("damage sighting acquires its hostile instigator"),
		Victim->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
		static_cast<AActor*>(Enemy));
	TestFalse(TEXT("hostile damage below 20 does not retreat"),
		Victim->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	Victim->TakeDamage(20.0f, Hurt, Enemy->GetController(), Enemy);
	const FWoTGoal& Retreat = Victim->Brain->GetGoal(EWoTGoalIndex::Intermediate);
	TestTrue(TEXT("hostile damage of 20 starts the intermediate retreat"),
		Retreat.HasVector());
	TestEqual(TEXT("retreat distance is the shipped 160uu"),
		FVector::Dist(Victim->GetActorLocation(), Retreat.GoalVector), 160.0, 0.1);
	TestTrue(TEXT("damage retreat follows the pawn's shipped facing"),
		Retreat.GoalVector.X > Victim->GetActorLocation().X);
	AWoTTestNPCCharacter* EnvironmentVictim = SpawnRC33NPC(
		World, FVector(0, 125, 0), 1);
	WoTTest::TickWorld(World, 0.1f);
	EnvironmentVictim->TakeDamage(20.0f, Hurt, nullptr, nullptr);
	TestTrue(TEXT("heavy environmental damage also retreats with no instigator"),
		EnvironmentVictim->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));

	AWoTNPCCharacter* Champion = WoTCreatureData::SpawnCreature(*World,
		TEXT("BATrolloc"), FVector(0, 250, 0), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Champion spawned"), Champion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 0.1f);
	Champion->TakeDamage(100.0f, Hurt, Enemy->GetController(), Enemy);
	TestFalse(TEXT("Champion-class -1 override disables hostile retreat"),
		Champion->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));

	AWoTTestNPCCharacter* FriendlyVictim = SpawnRC33NPC(
		World, FVector(0, 500, 0), 1);
	AWoTTestNPCCharacter* FriendlyShooter = SpawnRC33NPC(
		World, FVector(100, 500, 0), 1);
	WoTTest::TickWorld(World, 0.1f);
	FriendlyVictim->TakeDamage(1.0f, Hurt, FriendlyShooter->GetController(),
		FriendlyShooter);
	TestTrue(TEXT("friendly fire retreats regardless of damage threshold"),
		FriendlyVictim->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	TestEqual(TEXT("friendly attacker is classified as colleague"),
		FriendlyVictim->Brain->GetGoal(EWoTGoalIndex::Colleague).GoalActor.Get(),
		static_cast<AActor*>(FriendlyShooter));

	AWoTTestNPCCharacter* Bumped = SpawnRC33NPC(World, FVector(0, 1000, 0), 1);
	AWoTTestNPCCharacter* FriendlyNPC = SpawnRC33NPC(World, FVector(0, 1000, 0), 1);
	WoTTest::TickWorld(World, 0.1f);
	FriendlyNPC->GetCharacterMovement()->Velocity = FVector(100, 0, 0);
	CastChecked<AWoTNPCController>(Bumped->GetController())->HandleGruntBump(FriendlyNPC);
	TestFalse(TEXT("shipped friendly-NPC bump returns before sidestepping"),
		Bumped->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	Bumped->AssignedTeam = WoTTeam::PC;
	AWoTPlayerController* PlayerController = nullptr;
	AWoTCharacter* FriendlyPlayer = SpawnRC33Player(
		World, FVector(0, 1000, 0), 3, PlayerController);
	FriendlyPlayer->GetCharacterMovement()->Velocity = FVector(100, 0, 0);
	CastChecked<AWoTNPCController>(Bumped->GetController())->HandleGruntBump(FriendlyPlayer);
	TestTrue(TEXT("a moving friendly player bump sidesteps 160uu"),
		Bumped->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	TestTrue(TEXT("friendly-player bump follows the player's facing"),
		Bumped->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalVector.X
			> Bumped->GetActorLocation().X);

	AWoTNPCCharacter* Myrddraal = WoTCreatureData::SpawnCreature(*World,
		TEXT("Myrddraal"), FVector(0, 1500, 0), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	FActorSpawnParameters NodeParams = RC33AlwaysSpawn();
	AWoTPathNode* ShadowNode = World->SpawnActor<AWoTPathNode>(
		FVector(400, 1500, 0), FRotator::ZeroRotator, NodeParams);
	ShadowNode->bDark = true;
	WoTTest::TickWorld(World, 0.1f);
	Myrddraal->TakeDamage(20.0f, Hurt, Enemy->GetController(), Enemy);
	AWoTNPCController* MyrddraalController =
		CastChecked<AWoTNPCController>(Myrddraal->GetController());
	TestTrue(TEXT("Myrddraal heavy-damage trigger starts its escape episode"),
		MyrddraalController->IsTeleportEpisodeActive());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33PlayerMovementStatesSpec,
	"WoT.RC33.G5.PlayerMovementStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33PlayerMovementStatesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = nullptr;
	AWoTCharacter* Player = SpawnRC33Player(World, FVector::ZeroVector, 1, PC);
	FWoTDamageEvent Skew;
	Skew.WoTDamageType = TEXT("SkewRip");
	Player->TakeDamage(1.0f, Skew, nullptr, nullptr);
	TestEqual(TEXT("SkewRip enters Impaled"),
		static_cast<int32>(Player->PlayerMovementConstraint),
		static_cast<int32>(EWoTPlayerMovementConstraint::Impaled));
	TestEqual(TEXT("Impaled prediction speed is zero"),
		Player->GetCharacterMovement()->GetMaxSpeed(), 0.0f);
	Player->AddForwardInputForTest(1.0f);
	TestTrue(TEXT("Impaled consumes movement input"),
		Player->GetPendingMovementInputVector().IsNearlyZero());
	FHitResult FloorHit;
	FloorHit.Normal = FVector::UpVector;
	Player->Landed(FloorHit);
	TestFalse(TEXT("Landed leaves Impaled/Immobilized/Grabbed"),
		Player->IsPlayerMovementConstrained());
	FWoTDamageEvent Grab;
	Grab.WoTDamageType = TEXT("Grabbed");
	Player->TakeDamage(1.0f, Grab, nullptr, nullptr);
	TestEqual(TEXT("Grabbed damage enters Grabbed"),
		static_cast<int32>(Player->PlayerMovementConstraint),
		static_cast<int32>(EWoTPlayerMovementConstraint::Grabbed));
	Player->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Immobilized);
	TestEqual(TEXT("base Immobilized state also suppresses predicted speed"),
		Player->GetCharacterMovement()->GetMaxSpeed(), 0.0f);
	const FProperty* ConstraintProperty = FindFProperty<FProperty>(
		AWoTCharacter::StaticClass(), TEXT("PlayerMovementConstraint"));
	TestTrue(TEXT("constraint state replicates to drive client prediction"),
		ConstraintProperty && ConstraintProperty->HasAnyPropertyFlags(CPF_Net));
	TestNull(TEXT("separate hard-landing replication channel is removed"),
		FindFProperty<FProperty>(AWoTCharacter::StaticClass(), TEXT("bHardLanding")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33ImpaledRotationSpec,
	"WoT.RC33.G5.ImpaledRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33ImpaledRotationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = nullptr;
	AWoTCharacter* Player = SpawnRC33Player(World, FVector::ZeroVector, 1, PC);
	Player->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Impaled);
	constexpr double UnitsToDegrees = 360.0 / 65536.0;

	PC->SetControlRotation(FRotator(2800.0f * UnitsToDegrees, 0.0f, 0.0f));
	PC->ApplyPitchRotationForTest(400.0f * UnitsToDegrees);
	TestEqual(TEXT("positive crossing chooses the 3000-unit boundary"),
		PC->GetControlRotation().Pitch, 3000.0 * UnitsToDegrees, 0.01);
	TestEqual(TEXT("positive crossing copies resulting view pitch to actor"),
		Player->GetActorRotation().Pitch, 3000.0 * UnitsToDegrees, 0.01);

	PC->SetControlRotation(FRotator(8200.0f * UnitsToDegrees, 0.0f, 0.0f));
	PC->ApplyPitchRotationForTest(-400.0f * UnitsToDegrees);
	TestEqual(TEXT("negative crossing chooses the 8000-unit boundary"),
		PC->GetControlRotation().Pitch, 8000.0 * UnitsToDegrees, 0.01);
	TestEqual(TEXT("negative crossing copies resulting view pitch to actor"),
		Player->GetActorRotation().Pitch, 8000.0 * UnitsToDegrees, 0.01);

	PC->SetControlRotation(FRotator(89.0f, 0.0f, 0.0f));
	PC->ApplyPitchRotationForTest(0.0f);
	TestEqual(TEXT("positive actor pitch clamps at 15000 units"),
		Player->GetActorRotation().Pitch, 15000.0 * UnitsToDegrees, 0.01);
	PC->SetControlRotation(FRotator(-89.0f, 0.0f, 0.0f));
	PC->ApplyPitchRotationForTest(0.0f);
	TestEqual(TEXT("negative actor pitch clamps at minus 15000 units"),
		Player->GetActorRotation().Pitch, -15000.0 * UnitsToDegrees, 0.01);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33ConstraintPredictionNetSpec,
	"WoT.RC33.G5.ConstraintPredictionCorrectionRelevancyNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33ConstraintPredictionNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 authoritative constraints and prediction latch"));
	if (!TestTrue(TEXT("constraint dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("constraint owner connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* ServerPawn = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	AWoTPlayerController* ClientPC = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* ClientPawn = ClientPC
		? Cast<AWoTCharacter>(ClientPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("constraint authority pawn"), ServerPawn)
		|| !TestNotNull(TEXT("constraint autonomous pawn"), ClientPawn))
	{
		return false;
	}
	UWoTCharacterMovement* ClientMovement = CastChecked<UWoTCharacterMovement>(
		ClientPawn->GetCharacterMovement());
	auto ForceCorrection = [&Worlds, ServerPawn, ClientPawn, ClientMovement]()
	{
		const int32 Before = ClientMovement->GetClientCorrectionCountForTest();
		// Send any delayed move first: combining a pre-teleport pending move with
		// the next one restores its start location and velocity, which erases the
		// injected error before the server can see it (see ForceCorrectionAndConverge).
		ClientMovement->FlushServerMoves();
		ClientPawn->SetActorLocation(ClientPawn->GetActorLocation()
			+ FVector(900.0f, 0.0f, 0.0f), false, nullptr,
			ETeleportType::TeleportPhysics);
		return Worlds.TickUntil([ClientMovement, Before]()
		{
			return ClientMovement->GetClientCorrectionCountForTest() > Before;
		}, 240);
	};

	ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Grabbed);
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("Grabbed authority value replicates to owner latch"),
		Worlds.TickUntil([ClientPawn]()
		{
			return ClientPawn->PlayerMovementConstraint
				== EWoTPlayerMovementConstraint::Grabbed
				&& ClientPawn->GetEffectivePlayerMovementConstraint()
				== EWoTPlayerMovementConstraint::Grabbed;
		}, 120));
	FHitResult FloorHit;
	FloorHit.Normal = FVector::UpVector;
	ClientPawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	ClientPawn->Landed(FloorHit);
	TestEqual(TEXT("premature landing leaves replicated Grabbed value untouched"),
		ClientPawn->PlayerMovementConstraint,
		EWoTPlayerMovementConstraint::Grabbed);
	TestEqual(TEXT("premature landing clears only autonomous latch"),
		ClientPawn->GetEffectivePlayerMovementConstraint(),
		EWoTPlayerMovementConstraint::None);
	TestEqual(TEXT("server still holds Grabbed after client landing"),
		ServerPawn->PlayerMovementConstraint,
		EWoTPlayerMovementConstraint::Grabbed);
	TestTrue(TEXT("movement correction reasserts server-held Grabbed latch"),
		ForceCorrection() && ClientPawn->GetEffectivePlayerMovementConstraint()
			== EWoTPlayerMovementConstraint::Grabbed);

	ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Impaled);
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("Impaled authority value replicates before water prediction"),
		Worlds.TickUntil([ClientPawn]()
		{
			return ClientPawn->GetEffectivePlayerMovementConstraint()
				== EWoTPlayerMovementConstraint::Impaled;
		}, 120));
	ClientPawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	TestEqual(TEXT("premature water leaves replicated Impaled value untouched"),
		ClientPawn->PlayerMovementConstraint,
		EWoTPlayerMovementConstraint::Impaled);
	TestEqual(TEXT("premature water clears only autonomous latch"),
		ClientPawn->GetEffectivePlayerMovementConstraint(),
		EWoTPlayerMovementConstraint::None);
	TestEqual(TEXT("server still holds Impaled after client water"),
		ServerPawn->PlayerMovementConstraint,
		EWoTPlayerMovementConstraint::Impaled);
	TestTrue(TEXT("movement correction reasserts server-held Impaled latch"),
		ForceCorrection() && ClientPawn->GetEffectivePlayerMovementConstraint()
			== EWoTPlayerMovementConstraint::Impaled);

	ServerPawn->bAlwaysRelevant = false;
	ServerPawn->SetNetCullDistanceSquared(FMath::Square(1000.0f));
	ServerPawn->SetActorLocation(FVector(50000.0f, 0.0f, 5000.0f));
	ServerPawn->ForceNetUpdate();
	Worlds.TickAll(4);
	TestTrue(TEXT("late observer connects while Impaled is already held"),
		Worlds.CreateAndConnectClient());
	AWoTPlayerController* ObserverServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(1));
	AWoTCharacter* ObserverServerPawn = ObserverServerPC
		? Cast<AWoTCharacter>(ObserverServerPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("late observer authority pawn"), ObserverServerPawn))
	{
		return false;
	}
	ObserverServerPawn->SetActorLocation(
		ServerPawn->GetActorLocation() + FVector(100.0f, 0.0f, 0.0f));
	ObserverServerPawn->ForceNetUpdate();
	ServerPawn->ForceNetUpdate();
	AWoTCharacter* ObservedPawn = nullptr;
	TestTrue(TEXT("newly relevant observer receives held Impaled state"),
		Worlds.TickUntil([&Worlds, &ObservedPawn]()
		{
			for (TActorIterator<AWoTCharacter> It(Worlds.Clients[1].GetWorld()); It; ++It)
			{
				if (!It->IsLocallyControlled()
					&& It->PlayerMovementConstraint
						== EWoTPlayerMovementConstraint::Impaled)
				{
					ObservedPawn = *It;
					return true;
				}
			}
			return false;
		}, 240));
	if (ObservedPawn)
	{
		TestEqual(TEXT("simulated late/relevant copy exposes authoritative state"),
			ObservedPawn->GetEffectivePlayerMovementConstraint(),
			EWoTPlayerMovementConstraint::Impaled);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33WarderBATMovementSpec,
	"WoT.RC33.G5.WarderPullAndBATHeadToss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33WarderBATMovementSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	auto ExerciseWorld = [this](FTestWorlds& Worlds, ENetMode ExpectedMode,
		const FString& Label)
	{
		if (!TestTrue(*FString::Printf(TEXT("%s server loads"), *Label),
			Worlds.Server.IsLoaded()))
		{
			return false;
		}
		UWorld* ServerWorld = Worlds.Server.GetWorld();
		ServerWorld->SpawnActor<AWoTPlayerStart>(
			FVector(-4000.0, -1000.0, 5000.0), FRotator::ZeroRotator,
			RC33AlwaysSpawn());
		ServerWorld->SpawnActor<AWoTPlayerStart>(
			FVector(-4000.0, 1000.0, 5000.0), FRotator::ZeroRotator,
			RC33AlwaysSpawn());
		if (!TestTrue(*FString::Printf(TEXT("%s autonomous client connects"), *Label),
				Worlds.CreateAndConnectClient())
			|| !TestTrue(*FString::Printf(TEXT("%s simulated observer connects"), *Label),
				Worlds.CreateAndConnectClient()))
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("%s real server mode"), *Label),
			ServerWorld->GetNetMode(), ExpectedMode);
		AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
			Worlds.GetServerPlayerControllerOfClient(0));
		AWoTCharacter* ServerPawn = ServerPC
			? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
		AWoTPlayerController* ClientPC = Cast<AWoTPlayerController>(
			Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
		AWoTCharacter* ClientPawn = ClientPC
			? Cast<AWoTCharacter>(ClientPC->GetPawn()) : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s authority pawn"), *Label), ServerPawn)
			|| !TestNotNull(*FString::Printf(TEXT("%s autonomous pawn"), *Label), ClientPawn))
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("%s owner is AutonomousProxy"), *Label),
			ClientPawn->GetLocalRole(), ROLE_AutonomousProxy);
		AWoTPlayerController* ObserverServerPC = nullptr;
		AWoTCharacter* ObserverServerPawn = nullptr;
		TestTrue(*FString::Printf(TEXT("%s observer authority pawn resolves"), *Label),
			Worlds.TickUntil([&Worlds, &ObserverServerPC, &ObserverServerPawn]()
			{
				ObserverServerPC = Cast<AWoTPlayerController>(
					Worlds.GetServerPlayerControllerOfClient(1));
				ObserverServerPawn = ObserverServerPC
					? Cast<AWoTCharacter>(ObserverServerPC->GetPawn()) : nullptr;
				return ObserverServerPawn != nullptr;
			}, 120));
		if (!ObserverServerPawn)
		{
			return false;
		}
		ObserverServerPawn->SetActorLocation(FVector(0, 4000, 5000));
		ObserverServerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		ObserverServerPawn->GetCharacterMovement()->StopMovementImmediately();
		ObserverServerPawn->ForceNetUpdate();
		AWoTCharacter* SimulatedPawn = nullptr;
		TestTrue(*FString::Printf(TEXT("%s simulated copy resolves"), *Label),
			Worlds.TickUntil([&Worlds, &SimulatedPawn]()
			{
				for (TActorIterator<AWoTCharacter> It(
					Worlds.Clients[1].GetWorld()); It; ++It)
				{
					if (It->GetLocalRole() == ROLE_SimulatedProxy)
					{
						SimulatedPawn = *It;
						return true;
					}
				}
				return false;
			}, 120));
		if (!SimulatedPawn)
		{
			return false;
		}
		TestNull(*FString::Printf(TEXT("%s additive owner impulse RPC is removed"), *Label),
			AWoTPlayerController::StaticClass()->FindFunctionByName(
				TEXT("ClientApplyCreatureImpulse")));
		if (AWoTPlayerState* PS = ServerPC->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = 2;
			PS->ForceNetUpdate();
		}
		// Keep stochastic production-table selection from turning a long retry
		// sequence into an unrelated death/respawn episode.
		ServerPawn->Health = ServerPawn->MaxHealth = 100000;
		ServerPawn->SetActorLocation(FVector(40, 0, 5000));
		ServerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		ServerPawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		ClientPawn->SetActorLocation(FVector(40, 0, 5000));
		ClientPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		ClientPawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		ServerPawn->ForceNetUpdate();
		Worlds.TickAll(4);
		UWoTCharacterMovement* ClientMovement = CastChecked<UWoTCharacterMovement>(
			ClientPawn->GetCharacterMovement());
		auto VelocitiesEqualAtSameSimulationTime = [&Worlds, ServerPawn,
			ClientMovement]()
		{
			const FVector ServerVelocity =
				ServerPawn->GetCharacterMovement()->Velocity;
			// FTestWorlds advances the server and then the client. Falling
			// velocity is therefore observed one deterministic gravity step
			// later on the client even when the correction is exact. Compare
			// the same simulation time; flying/grounded cases remain direct.
			if (!ServerPawn->GetCharacterMovement()->IsFalling()
				|| !ClientMovement->IsFalling())
			{
				return ClientMovement->Velocity.Equals(ServerVelocity, 2.0f);
			}
			// Net-test server/client scheduling can expose the same correction at
			// zero, one, or two deterministic gravity integrations apart. Normalize
			// only those known phase offsets; horizontal velocity remains exact.
			const float GravityStep =
				ServerPawn->GetCharacterMovement()->GetGravityZ()
				* Worlds.GetTickDeltaSeconds();
			for (int32 Steps = 0; Steps <= 2; ++Steps)
			{
				FVector PhasedServerVelocity = ServerVelocity;
				PhasedServerVelocity.Z += GravityStep * Steps;
				if (ClientMovement->Velocity.Equals(PhasedServerVelocity, 2.0f))
					return true;
			}
			return false;
		};
		auto ForceCorrectionAndConverge = [this, &Worlds, ServerPawn,
			ClientPawn, ClientMovement, &Label,
			&VelocitiesEqualAtSameSimulationTime](const TCHAR* Stage)
		{
			const int32 CorrectionsBefore =
				ClientMovement->GetClientCorrectionCountForTest();
			// The error is injected between ticks, outside any saved move. If a
			// delayed pending move from before the teleport combines with the next
			// move, ReplicateMoveToServer reverts to the pending move's start
			// location and velocity (CombineWith), so the error vanishes and no
			// correction ever comes. Constrained pawns combine readily (MaxSpeed 0,
			// falling, no acceleration); the Impaled release only blocks it when its
			// OnRep arrives first. Send the pending move before injecting.
			ClientMovement->FlushServerMoves();
			ClientPawn->SetActorLocation(
				ClientPawn->GetActorLocation() + FVector(750.0f, 0.0f, 0.0f),
				false, nullptr, ETeleportType::TeleportPhysics);
			ClientMovement->Velocity += FVector(777.0f, -333.0f, 111.0f);
			const bool bConverged = Worlds.TickUntil(
				[ClientMovement, CorrectionsBefore,
					&VelocitiesEqualAtSameSimulationTime]()
				{
					return ClientMovement->GetClientCorrectionCountForTest()
						> CorrectionsBefore
						&& VelocitiesEqualAtSameSimulationTime();
				}, 240);
			return TestTrue(*FString::Printf(
				TEXT("%s %s forced correction reconverges velocity "
					"(server=%s mode=%d client=%s mode=%d corrections=%d->%d)"),
				*Label, Stage,
				*ServerPawn->GetCharacterMovement()->Velocity.ToString(),
				static_cast<int32>(ServerPawn->GetCharacterMovement()->MovementMode),
				*ClientMovement->Velocity.ToString(),
				static_cast<int32>(ClientMovement->MovementMode), CorrectionsBefore,
				ClientMovement->GetClientCorrectionCountForTest()), bConverged);
		};
		if (!ForceCorrectionAndConverge(TEXT("before Warder event")))
		{
			return false;
		}

		AWoTNPCCharacter* Warder = WoTCreatureData::SpawnCreature(*ServerWorld,
			TEXT("Warder"), FVector(0, 0, 5000), 1, true,
			AWoTTestNPCCharacter::StaticClass());
		if (!TestNotNull(TEXT("Warder spawned"), Warder))
		{
			return false;
		}
		Warder->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		auto HasAttack = [](const AWoTNPCCharacter* NPC, FName Type, float Fraction)
		{
			return NPC && NPC->MeleeAttacks.ContainsByPredicate(
				[Type, Fraction](const FWoTMeleeAttack& Attack)
				{
					return Attack.DamageTypeName == Type
						&& FMath::IsNearlyEqual(Attack.NotifyFraction, Fraction);
				});
		};
		TestTrue(*FString::Printf(TEXT("%s generated Warder .02 SkewRip row"), *Label),
			HasAttack(Warder, TEXT("SkewRip"), 0.02f));
		TestTrue(*FString::Printf(TEXT("%s generated Warder .70 Ripped row"), *Label),
			HasAttack(Warder, TEXT("Ripped"), 0.70f));
		AWoTNPCController* WarderController =
			CastChecked<AWoTNPCController>(Warder->GetController());
		WarderController->DispatchSeePawn(ServerPawn);
		TestTrue(*FString::Printf(TEXT("%s Warder acquires through normal waiting cadence"), *Label),
			Worlds.TickUntil([WarderController]()
		{
			return WarderController->GetBrainState() == EWoTBrainState::Tracking;
		}, 120));
		// ReflectTick's production emission seam: exercise normal generated swings
		// until both authored Warder sequences have been selected.
		for (int32 Attempt = 0; Attempt < 128
			&& (ServerPawn->ImpaledConstraintApplyCountForTest == 0
				|| ServerPawn->AuthorityCreatureImpulseCountForTest == 0); ++Attempt)
		{
			Worlds.TickUntil([WarderController]()
			{
				return WarderController->GetBrainState() == EWoTBrainState::Tracking;
			}, 600);
			Warder->Brain->OnNotifierPulse.Broadcast(EWoTNotifierSlot::Offensive);
			Worlds.TickAll(1);
			Worlds.TickUntil([WarderController, ServerPawn]()
			{
				return ServerPawn->ImpaledConstraintApplyCountForTest > 0
					&& ServerPawn->AuthorityCreatureImpulseCountForTest > 0
					|| WarderController->GetBrainState() == EWoTBrainState::Tracking;
			}, 600);
			if (ServerPawn->IsPlayerMovementConstrained())
			{
				ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
				Worlds.TickUntil([ClientPawn]()
				{
					return !ClientPawn->IsPlayerMovementConstrained();
				}, 120);
			}
		}
		TestTrue(*FString::Printf(TEXT("%s Warder .02 state reaches authority+owner"), *Label),
			ServerPawn->ImpaledConstraintApplyCountForTest > 0
			&& ClientPawn->ImpaledConstraintApplyCountForTest > 0);
		TestEqual(*FString::Printf(TEXT("%s Warder state applies equally on both roles"), *Label),
			ClientPawn->ImpaledConstraintApplyCountForTest,
			ServerPawn->ImpaledConstraintApplyCountForTest);
		TestEqual(*FString::Printf(TEXT("%s Warder state applies exactly once"), *Label),
			ServerPawn->ImpaledConstraintApplyCountForTest, 1);
		TestTrue(*FString::Printf(TEXT("%s Warder .70 impulse reaches authority"), *Label),
			ServerPawn->AuthorityCreatureImpulseCountForTest > 0);
		TestEqual(*FString::Printf(TEXT("%s Warder velocity applies exactly once"), *Label),
			ServerPawn->AuthorityCreatureImpulseCountForTest, 1);
		TestTrue(*FString::Printf(TEXT("%s Warder owner settles to authority velocity "
			"without a manufactured correction"), *Label),
			Worlds.TickUntil([&VelocitiesEqualAtSameSimulationTime]()
			{
				return VelocitiesEqualAtSameSimulationTime();
			}, 240));
		TestTrue(*FString::Printf(TEXT("%s Warder pull points inward with 250 lift"), *Label),
			ServerPawn->LastAuthorityCreatureImpulseForTest.X < 0.0f
			&& ServerPawn->LastAuthorityCreatureImpulseForTest.Z >= 249.9f);
		Warder->Destroy();

		ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
		ServerPawn->SetActorLocation(FVector(40, 500, 5000));
		ServerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		ServerPawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		ClientPawn->SetActorLocation(FVector(40, 500, 5000));
		ClientPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		ClientPawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		ServerPawn->ForceNetUpdate();
		Worlds.TickAll(4);
		if (!ForceCorrectionAndConverge(TEXT("before BAT event")))
		{
			return false;
		}
		AWoTNPCCharacter* BAT = WoTCreatureData::SpawnCreature(*ServerWorld,
			TEXT("BATrolloc"), FVector(0, 500, 5000), 1, true,
			AWoTTestNPCCharacter::StaticClass());
		if (!TestNotNull(TEXT("BAT spawned"), BAT))
		{
			return false;
		}
		BAT->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		TestTrue(*FString::Printf(TEXT("%s generated BAT .25 Tossed row"), *Label),
			HasAttack(BAT, TEXT("Tossed"), 0.25f));
		AWoTNPCWeapon* Halberd = BAT->GetRangedWeapon();
		AWoTTestNPCCharacter* ThrowTarget = SpawnRC33NPC(
			ServerWorld, FVector(100000, 500, 5000), 2);
		TestNotNull(*FString::Printf(TEXT("%s BAT production halberd"), *Label),
			Halberd);
		TestNotNull(*FString::Printf(TEXT("%s BAT throw target"), *Label),
			ThrowTarget);
		AWoTNPCController* BATController =
			CastChecked<AWoTNPCController>(BAT->GetController());
		BATController->DispatchSeePawn(ServerPawn);
		TestTrue(*FString::Printf(TEXT("%s BAT acquires through normal waiting cadence"), *Label),
			Worlds.TickUntil([BATController]()
		{
			return BATController->GetBrainState() == EWoTBrainState::Tracking;
		}, 120));
		const int32 WarderImpulseCount =
			ServerPawn->AuthorityCreatureImpulseCountForTest;
		const int32 ClientPitchObserveCount =
			ClientPawn->CreaturePresentationPitchObserveCountForTest;
		const int32 ClientPitchClearCount =
			ClientPawn->CreaturePresentationPitchClearCountForTest;
		const int32 ClientPitchRetainCount =
			ClientPawn->CreaturePresentationPitchRetainCountForTest;
		const int32 SimulatedPitchObserveCount =
			SimulatedPawn->CreaturePresentationPitchObserveCountForTest;
		const int32 SimulatedPitchRetainCount =
			SimulatedPawn->CreaturePresentationPitchRetainCountForTest;
		const int32 SimulatedPitchClearCount =
			SimulatedPawn->CreaturePresentationPitchClearCountForTest;
		for (int32 Attempt = 0;
			Attempt < 128
				&& ServerPawn->AuthorityCreatureImpulseCountForTest == WarderImpulseCount;
			++Attempt)
		{
			// A BAT can select retail's Charge only while its real throwable
			// halberd is out of hand. Exercise the same production weapon method
			// used by PerformProjectile, then let the normal melee selector consume
			// the generated MeleeAttack table (Charge odds .40).
			if (!Halberd->IsInFlight())
			{
				Halberd->UseProjectileWeapon(BAT, ThrowTarget);
			}
			TestTrue(*FString::Printf(TEXT("%s BAT halberd is really in flight"), *Label),
				Halberd->IsInFlight());
			TestEqual(*FString::Printf(TEXT("%s BAT routes to body melee table"), *Label),
				BAT->GetEffectiveMeleeAnimSlot(), FName(TEXT("MeleeAttack")));
			Worlds.TickUntil([BATController]()
			{
				return BATController->GetBrainState() == EWoTBrainState::Tracking;
			}, 600);
			BAT->Brain->OnNotifierPulse.Broadcast(EWoTNotifierSlot::Offensive);
			Worlds.TickAll(1);
			Worlds.TickUntil([BATController, ServerPawn, WarderImpulseCount]()
			{
				return ServerPawn->AuthorityCreatureImpulseCountForTest > WarderImpulseCount
					|| BATController->GetBrainState() == EWoTBrainState::Tracking;
			}, 600);
		}
		TestTrue(*FString::Printf(TEXT("%s BAT .25/.45 production swing applies once"), *Label),
			Worlds.TickUntil([ServerPawn, WarderImpulseCount]()
			{
				return ServerPawn->AuthorityCreatureImpulseCountForTest
						== WarderImpulseCount + 1;
			}, 240));
		// No manufactured location error/correction after this impulse: ordinary
		// character movement replication must settle the autonomous copy itself.
		TestTrue(*FString::Printf(TEXT("%s BAT owner settles without a forced correction"),
			*Label), Worlds.TickUntil([ServerPawn, ClientMovement]()
			{
				return ClientMovement->Velocity.Equals(
					ServerPawn->GetCharacterMovement()->Velocity, 2.0f);
			}, 240));
		TestEqual(*FString::Printf(TEXT("%s BAT HeadToss authored one 22.5 pitch pulse"),
			*Label), ServerPawn->LastCreaturePresentationPitchForTest, 22.5f, 0.1f);
		TestTrue(*FString::Printf(TEXT("%s both clients retain the 22.5 pitch through "
			"their first presentation update"), *Label),
			Worlds.TickUntil([ClientPawn, SimulatedPawn, ClientPitchRetainCount,
				SimulatedPitchRetainCount]()
			{
				return ClientPawn->CreaturePresentationPitchRetainCountForTest
						> ClientPitchRetainCount
					&& SimulatedPawn->CreaturePresentationPitchRetainCountForTest
						> SimulatedPitchRetainCount;
			}, 120));
		TestTrue(*FString::Printf(TEXT("%s owner client observed the 22.5 pitch event"),
			*Label), ClientPawn->CreaturePresentationPitchObserveCountForTest
				> ClientPitchObserveCount);
		TestEqual(*FString::Printf(TEXT("%s owner client event payload is 22.5"),
			*Label), ClientPawn->LastCreaturePresentationPitchForTest, 22.5f, 0.1f);
		TestTrue(*FString::Printf(TEXT("%s simulated client observed the 22.5 pitch event"),
			*Label), SimulatedPawn->CreaturePresentationPitchObserveCountForTest
				> SimulatedPitchObserveCount);
		TestEqual(*FString::Printf(TEXT("%s simulated client event payload is 22.5"),
			*Label), SimulatedPawn->LastCreaturePresentationPitchForTest, 22.5f, 0.1f);
		TestEqual(*FString::Printf(TEXT("%s first presentation update retains owner pitch"),
			*Label), ClientPawn->LastCreaturePresentationRetainedActorPitchForTest,
			22.5f, 0.1f);
		TestEqual(*FString::Printf(TEXT("%s first presentation update retains simulated pitch"),
			*Label), SimulatedPawn->LastCreaturePresentationRetainedActorPitchForTest,
			22.5f, 0.1f);
		TestTrue(*FString::Printf(TEXT("%s next presentation update clears both net roles"),
			*Label), Worlds.TickUntil([ClientPawn, SimulatedPawn,
				ClientPitchClearCount, SimulatedPitchClearCount]()
			{
				return ClientPawn->CreaturePresentationPitchClearCountForTest
						> ClientPitchClearCount
					&& SimulatedPawn->CreaturePresentationPitchClearCountForTest
						> SimulatedPitchClearCount;
			}, 120));
		TestTrue(*FString::Printf(TEXT("%s clear update writes zero to both actual transforms"),
			*Label), FMath::IsNearlyZero(
				ClientPawn->LastCreaturePresentationClearedActorPitchForTest, 0.1f)
			&& FMath::IsNearlyZero(
				SimulatedPawn->LastCreaturePresentationClearedActorPitchForTest, 0.1f));
		BAT->Destroy();
		const bool bPitchCleared = Worlds.TickUntil([ServerPawn, ClientPawn,
			ClientPitchClearCount]()
			{
				return FMath::IsNearlyZero(ServerPawn->GetActorRotation().Pitch, 0.1f)
					&& FMath::IsNearlyZero(ClientPawn->GetActorRotation().Pitch, 0.1f)
					&& ClientPawn->CreaturePresentationPitchClearCountForTest
						> ClientPitchClearCount;
			}, 120);
		TestTrue(*FString::Printf(TEXT("%s next normal update clears BAT pitch on both "
			"roles (server=%.3f client=%.3f replicated=%.3f)"), *Label,
			ServerPawn->GetActorRotation().Pitch,
			ClientPawn->GetActorRotation().Pitch,
			ServerPawn->CreaturePresentationPitch), bPitchCleared);

		ServerPawn->SetPlayerMovementConstraint(
			EWoTPlayerMovementConstraint::Impaled);
		ServerPawn->ForceNetUpdate();
		TestTrue(*FString::Printf(TEXT("%s Impaled reaches both clients before pitch"), *Label),
			Worlds.TickUntil([ClientPawn, SimulatedPawn]()
			{
				return ClientPawn->GetEffectivePlayerMovementConstraint()
						== EWoTPlayerMovementConstraint::Impaled
					&& SimulatedPawn->GetEffectivePlayerMovementConstraint()
						== EWoTPlayerMovementConstraint::Impaled;
			}, 120));
		const int32 ImpaledPitchReceiptCount =
			ClientPawn->CreaturePresentationPitchObserveCountForTest;
		const int32 SimulatedImpaledPitchReceiptCount =
			SimulatedPawn->CreaturePresentationPitchObserveCountForTest;
		const int32 OwnerRetainsBeforeImpaled =
			ClientPawn->CreaturePresentationPitchRetainCountForTest;
		const int32 SimulatedRetainsBeforeImpaled =
			SimulatedPawn->CreaturePresentationPitchRetainCountForTest;
		ServerPawn->SetCreaturePresentationPitch(22.5f);
		TestTrue(*FString::Printf(TEXT("%s Impaled clients consume the BAT pitch event"),
			*Label), Worlds.TickUntil([ClientPawn, SimulatedPawn,
				ImpaledPitchReceiptCount, SimulatedImpaledPitchReceiptCount]()
			{
				return ClientPawn->CreaturePresentationPitchObserveCountForTest
						> ImpaledPitchReceiptCount
					&& SimulatedPawn->CreaturePresentationPitchObserveCountForTest
						> SimulatedImpaledPitchReceiptCount;
			}, 120));
		TestTrue(*FString::Printf(TEXT("%s Impaled discards pitch on every transform"),
			*Label), FMath::IsNearlyZero(ServerPawn->GetActorRotation().Pitch, 0.1f)
			&& FMath::IsNearlyZero(ClientPawn->GetActorRotation().Pitch, 0.1f)
			&& FMath::IsNearlyZero(SimulatedPawn->GetActorRotation().Pitch, 0.1f)
			&& FMath::IsNearlyZero(ServerPawn->CreaturePresentationPitch, 0.1f)
			&& FMath::IsNearlyZero(ClientPawn->CreaturePresentationPitch, 0.1f)
			&& FMath::IsNearlyZero(SimulatedPawn->CreaturePresentationPitch, 0.1f)
			&& ClientPawn->CreaturePresentationPitchRetainCountForTest
				== OwnerRetainsBeforeImpaled
			&& SimulatedPawn->CreaturePresentationPitchRetainCountForTest
				== SimulatedRetainsBeforeImpaled);
		ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
		ServerPawn->ForceNetUpdate();
		TestTrue(*FString::Printf(TEXT("%s correction after Impaled release converges"),
			*Label), ForceCorrectionAndConverge(TEXT("after discarded Impaled pitch")));
		TestTrue(*FString::Printf(TEXT("%s no stale pitch applies after Impaled release"),
			*Label), FMath::IsNearlyZero(ServerPawn->GetActorRotation().Pitch, 0.1f)
			&& FMath::IsNearlyZero(ClientPawn->GetActorRotation().Pitch, 0.1f));
		Worlds.TickAll(30);
		TestEqual(*FString::Printf(TEXT("%s authority impulse count remains exact"), *Label),
			ServerPawn->AuthorityCreatureImpulseCountForTest, WarderImpulseCount + 1);
		// TickAll advances the client one gravity integration beyond the server
		// correction snapshot; normalize that deterministic step before comparing.
		TestTrue(*FString::Printf(TEXT("%s owner remains velocity-converged without "
			"an event RPC (server=%s mode=%d client=%s mode=%d)"), *Label,
			*ServerPawn->GetCharacterMovement()->Velocity.ToString(),
			static_cast<int32>(ServerPawn->GetCharacterMovement()->MovementMode),
			*ClientMovement->Velocity.ToString(),
			static_cast<int32>(ClientMovement->MovementMode)),
			VelocitiesEqualAtSameSimulationTime());

		ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Immobilized);
		ServerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
		TestTrue(*FString::Printf(TEXT("%s water transition releases both roles"), *Label),
			Worlds.TickUntil([ServerPawn, ClientPawn]()
			{
				return !ServerPawn->IsPlayerMovementConstrained()
					&& !ClientPawn->IsPlayerMovementConstrained();
			}, 120));

		AWoTTeleporter* Source = ServerWorld->SpawnActor<AWoTTeleporter>(
			FVector(0, 2000, 300), FRotator::ZeroRotator, RC33AlwaysSpawn());
		AWoTTeleporter* Dest = ServerWorld->SpawnActor<AWoTTeleporter>(
			FVector(6000, 2000, 300), FRotator::ZeroRotator, RC33AlwaysSpawn());
		Source->URL = TEXT("G5Release");
		Dest->TeleTag = TEXT("G5Release");
		ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Grabbed);
		WoTTest::PlaceWithoutOverlapEvents(ServerPawn, Source->GetActorLocation());
		Source->NotifyActorBeginOverlap(ServerPawn);
		TestTrue(*FString::Printf(TEXT("%s teleport releases authority+owner"), *Label),
			Worlds.TickUntil([ServerPawn, ClientPawn, Dest]()
			{
				return !ServerPawn->IsPlayerMovementConstrained()
					&& !ClientPawn->IsPlayerMovementConstrained()
					&& FVector::Dist(ServerPawn->GetActorLocation(),
						Dest->GetActorLocation()) < 100.0f;
			}, 180));

		ServerPawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Impaled);
		AWoTCharacter* OldServerPawn = ServerPawn;
		AWoTCharacter* OldClientPawn = ClientPawn;
		ServerPawn->TakeRawDamage(5000, TEXT("hurt"), nullptr,
			ServerPawn->GetActorLocation());
		TestTrue(*FString::Printf(TEXT("%s death releases authority+owner"), *Label),
			Worlds.TickUntil([OldServerPawn, OldClientPawn]()
			{
				return !OldServerPawn->IsPlayerMovementConstrained()
					&& !OldClientPawn->IsPlayerMovementConstrained();
			}, 180));
		ServerPC->ServerRestartPlayerWoT();
		TestTrue(*FString::Printf(TEXT("%s production respawn replaces both pawns"), *Label),
			Worlds.TickUntil([ServerPC, ClientPC, OldServerPawn, OldClientPawn]()
			{
				return ServerPC->GetPawn() && ServerPC->GetPawn() != OldServerPawn
					&& ClientPC->GetPawn() && ClientPC->GetPawn() != OldClientPawn;
			}, 240));
		AWoTCharacter* RespawnedServer = Cast<AWoTCharacter>(ServerPC->GetPawn());
		AWoTCharacter* RespawnedClient = Cast<AWoTCharacter>(ClientPC->GetPawn());
		TestTrue(*FString::Printf(TEXT("%s respawn/possession starts unconstrained"), *Label),
			RespawnedServer && RespawnedClient
			&& !RespawnedServer->IsPlayerMovementConstrained()
			&& !RespawnedClient->IsPlayerMovementConstrained());
		TestEqual(*FString::Printf(TEXT("%s respawn remains autonomous"), *Label),
			RespawnedClient ? RespawnedClient->GetLocalRole() : ROLE_None,
			ROLE_AutonomousProxy);
		return !HasAnyErrors();
	};

	{
		FTestWorlds Dedicated(TEXT("/Engine/Maps/Entry"),
			TEXT("/Script/WoT.WoTGameMode"));
		Dedicated.SetDescription(TEXT("RC33 G5 dedicated production melee"));
		if (!ExerciseWorld(Dedicated, NM_DedicatedServer, TEXT("dedicated")))
		{
			return false;
		}
	}
	{
		FTestWorlds Listen;
		Listen.Server = CreateRC33ListenServer();
		Listen.SetDescription(TEXT("RC33 G5 listen production melee"));
		if (!ExerciseWorld(Listen, NM_ListenServer, TEXT("listen")))
		{
			return false;
		}
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33CasterIntervalSpec,
	"WoT.RC33.G8.CasterChargeGroupInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33CasterIntervalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Sitter = WoTCreatureData::SpawnCreature(*World,
		TEXT("Sitter"), FVector::ZeroVector, 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Sitter spawned"), Sitter))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTInventoryComponent* Inventory =
		Sitter->FindComponentByClass<UWoTInventoryComponent>();
	AWoTAngrealInvEarthTremor* Tremor = Inventory
		? Cast<AWoTAngrealInvEarthTremor>(
			Inventory->FindInventoryType(AWoTAngrealInvEarthTremor::StaticClass()))
		: nullptr;
	TestNotNull(TEXT("Sitter production loadout contains Earth Tremor"), Tremor);
	if (Tremor)
	{
		TestEqual(TEXT("Sitter slot overrides Earth Tremor interval to 15s"),
			Tremor->MinChargeGroupInterval, 15.0f);
	}
	const FWoTCreatureRow* SisterRow = WoTCreatureData::Find(TEXT("Sister"));
	TestTrue(TEXT("Sister sentinel slots remain -1 and preserve class defaults"),
		SisterRow && SisterRow->Weaves.Num() > 0
		&& SisterRow->Weaves[0].MinChargeGroupInterval == -1.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33MashadarShockSpec,
	"WoT.RC33.G9.MashadarLightningShock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33MashadarShockSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = nullptr;
	AWoTCharacter* Caster = SpawnRC33Player(World, FVector(100, 0, 100), 2, PC);
	AWoTAngrealInvLightning* Lightning =
		World->SpawnActorDeferred<AWoTAngrealInvLightning>(
			AWoTAngrealInvLightning::StaticClass(), Caster->GetActorTransform(),
			Caster, Caster, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Lightning->bHeldItem = true;
	Lightning->FinishSpawning(Caster->GetActorTransform());
	Lightning->GiveTo(Caster);
	Lightning->bCasting = true;

	AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FTransform(FRotator::ZeroRotator,
			FVector::ZeroVector), nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Guide->bCitadelPlaced = false;
	Guide->FinishSpawning(Guide->GetActorTransform());
	Guide->Team = 1;
	Guide->Deploy();
	Guide->NotifySeePawn(Caster);
	AWoTMashadarSegment* Segment = World->SpawnActor<AWoTMashadarSegment>(
		AWoTMashadarSegment::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	Segment->Guide = Guide;
	Segment->BiteTarget(Caster);
	TestTrue(TEXT("active Lightning shocks the tendril"), Guide->IsShocked());
	TestFalse(TEXT("shock deactivates and retracts the tendril"), Guide->bActive);
	Guide->NotifySeePawn(Caster);
	TestFalse(TEXT("the same still-casting pawn cannot be re-chased"), Guide->bActive);
	Lightning->bCasting = false;
	// SeePlayer keeps a per-pawn 3-6 second retry window. Let the native observer
	// deliver the next eligible callback instead of bypassing the retail delay.
	WoTTest::TickWorld(World, 6.1f);
	TestTrue(TEXT("the next eligible callback after Lightning stops permits re-chase"),
		Guide->bActive);

	// Drive damage through the guide-created production chain. The inactive and
	// manager-membership gates precede the real active lethal path.
	AWoTMashadarGuide* LethalGuide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(300, 0, 0),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	LethalGuide->MashadarHealth = 1;
	AWoTMashadarSegment* LethalSegment = LethalGuide->GetMasterSegment();
	TestNotNull(TEXT("production guide creates its managed head segment"), LethalSegment);
	FWoTDamageEvent LethalHit;
	if (LethalSegment)
	{
		TestEqual(TEXT("inactive guide rejects managed-segment damage"),
			LethalSegment->TakeDamage(1.0f, LethalHit, PC, Caster), 0.0f);
		TestEqual(TEXT("inactive rejection preserves Mashadar health"),
			LethalGuide->MashadarHealth, 1);
	}
	AWoTMashadarSegment* UnmanagedSegment = World->SpawnActor<AWoTMashadarSegment>(
		AWoTMashadarSegment::StaticClass(), FVector(300, 0, 0),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	UnmanagedSegment->Guide = LethalGuide;
	LethalGuide->Deploy();
	LethalGuide->Activate();
	TestEqual(TEXT("active guide rejects a segment with no manager membership"),
		UnmanagedSegment->TakeDamage(1.0f, LethalHit, PC, Caster), 0.0f);
	TestEqual(TEXT("unmanaged rejection preserves Mashadar health"),
		LethalGuide->MashadarHealth, 1);
	if (LethalSegment)
	{
		LethalSegment->TakeDamage(1.0f, LethalHit, PC, Caster);
	}
	TestFalse(TEXT("production lethal segment flow destroys its guide immediately"),
		IsValid(LethalGuide));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33ProjectileSizesSpec,
	"WoT.RC33.G11.ProjectileCollisionSizes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33ProjectileSizesSpec::RunTest(const FString&)
{
	const AWoTTrollocAxeProjectile* Axe = GetDefault<AWoTTrollocAxeProjectile>();
	const AWoTMyrddraalBolt* Bolt = GetDefault<AWoTMyrddraalBolt>();
	const AWoTQuestShieldProjectile* Shield = GetDefault<AWoTQuestShieldProjectile>();
	const AWoTLegionProjectile* Spirit = GetDefault<AWoTLegionProjectile>();
	TestEqual(TEXT("Trolloc axe radius"), Axe->UE1CollisionRadius, 0.0f);
	TestEqual(TEXT("Trolloc axe height"), Axe->UE1CollisionHeight, 0.0f);
	TestEqual(TEXT("Myrddraal bolt radius"), Bolt->UE1CollisionRadius, 0.0f);
	TestEqual(TEXT("Myrddraal bolt height"), Bolt->UE1CollisionHeight, 0.0f);
	TestEqual(TEXT("Questioner shield radius"), Shield->UE1CollisionRadius, 18.0f);
	TestEqual(TEXT("Questioner shield height"), Shield->UE1CollisionHeight, 2.0f);
	TestEqual(TEXT("Legion spirit radius"), Spirit->UE1CollisionRadius, 6.0f);
	TestEqual(TEXT("Legion spirit height"), Spirit->UE1CollisionHeight, 12.0f);
	TestEqual(TEXT("Trolloc axe uses a zero-extent point sweep"),
		Axe->Collision->GetUnscaledSphereRadius(), 0.0f);
	TestEqual(TEXT("Myrddraal bolt uses a zero-extent point sweep"),
		Bolt->Collision->GetUnscaledSphereRadius(), 0.0f);
	const UWoTFlatCylinderComponent* ShieldCylinder =
		Cast<UWoTFlatCylinderComponent>(Shield->GetRootComponent());
	const UWoTFlatCylinderComponent* SpiritCylinder =
		Cast<UWoTFlatCylinderComponent>(Spirit->GetRootComponent());
	TestNotNull(TEXT("Questioner shield swept root is a flat cylinder"), ShieldCylinder);
	TestNotNull(TEXT("Legion spirit swept root is a flat cylinder"), SpiritCylinder);
	if (ShieldCylinder)
	{
		TestEqual(TEXT("Questioner cylinder radius"),
			ShieldCylinder->GetUnscaledCapsuleRadius(), 18.0f);
		TestEqual(TEXT("Questioner cylinder half-height"),
			ShieldCylinder->GetUnscaledCapsuleHalfHeight(), 2.0f);
	}
	if (SpiritCylinder)
	{
		TestEqual(TEXT("Legion cylinder radius"),
			SpiritCylinder->GetUnscaledCapsuleRadius(), 6.0f);
		TestEqual(TEXT("Legion cylinder half-height"),
			SpiritCylinder->GetUnscaledCapsuleHalfHeight(), 12.0f);
	}

	auto MovingDamageCount = [](UClass* ProjectileClass, const FVector& Offset,
		float Seconds)
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestProjectileCollisionTarget* Target =
			World->SpawnActor<AWoTTestProjectileCollisionTarget>(
				FVector(100.0f, 0.0f, 200.0f) + Offset,
				FRotator::ZeroRotator, RC33AlwaysSpawn());
		World->SpawnActor<AWoTProjectile>(ProjectileClass,
			FVector(0.0f, 0.0f, 200.0f), FRotator::ZeroRotator,
			RC33AlwaysSpawn());
		WoTTest::TickWorld(World, Seconds);
		const int32 Count = Target->DamageCount;
		WoTTest::DestroySpecWorld(World);
		return Count;
	};
	TestTrue(TEXT("moving 0/0 bolt hits an on-axis half-unit target"),
		MovingDamageCount(AWoTMyrddraalBolt::StaticClass(), FVector::ZeroVector, 0.2f) > 0);
	TestEqual(TEXT("moving 0/0 bolt misses at two units lateral"),
		MovingDamageCount(AWoTMyrddraalBolt::StaticClass(), FVector(0, 2, 0), 0.2f), 0);
	TestTrue(TEXT("moving 18x2 shield reaches 17 units laterally"),
		MovingDamageCount(AWoTQuestShieldProjectile::StaticClass(), FVector(0, 17, 0), 0.2f) > 0);
	TestEqual(TEXT("moving 18x2 shield does not have sphere-height reach"),
		MovingDamageCount(AWoTQuestShieldProjectile::StaticClass(), FVector(0, 0, 10), 0.2f), 0);
	TestTrue(TEXT("moving 6x12 spirit reaches 10 units vertically"),
		MovingDamageCount(AWoTLegionProjectile::StaticClass(), FVector(0, 0, 10), 0.8f) > 0);
	TestEqual(TEXT("moving 6x12 spirit misses at ten units lateral"),
		MovingDamageCount(AWoTLegionProjectile::StaticClass(), FVector(0, 10, 0), 0.8f), 0);
	return true;
}

// A seeking Seeker passes a blocking mover (ECC_WorldDynamic) on its way to the
// target: retail seeking sets bCollideWorld=False (SeekingProjectile.uc:389), and
// UE1 brushes block only bCollideWorld actors (AActor::IsBlockedBy). The gate's
// Battle2_03 corner shot pinned a Seeker under a closing portcullis for 10 s.
// Not seeking, the same Seeker stops at the mover.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33SeekerPassesMoverWhileSeekingSpec,
	"WoT.RC33.SeekerPassesMoverWhileSeeking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33SeekerPassesMoverWhileSeekingSpec::RunTest(const FString&)
{
	auto SpawnMoverSlab = [](UWorld* World, const FVector& Location)
	{
		AStaticMeshActor* Slab = World->SpawnActor<AStaticMeshActor>(
			Location, FRotator::ZeroRotator, RC33AlwaysSpawn());
		UBoxComponent* Box = NewObject<UBoxComponent>(Slab);
		Box->SetupAttachment(Slab->GetRootComponent());
		Box->SetBoxExtent(FVector(5.0f, 300.0f, 300.0f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldDynamic);   // AWoTMover's channel
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		return Slab;
	};

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestProjectileCollisionTarget* Target =
		World->SpawnActor<AWoTTestProjectileCollisionTarget>(
			FVector(700.0f, 0.0f, 300.0f), FRotator::ZeroRotator, RC33AlwaysSpawn());
	SpawnMoverSlab(World, FVector(250.0f, 0.0f, 300.0f));
	AWoTSeekerProjectile* Seeker = World->SpawnActor<AWoTSeekerProjectile>(
		FVector(0.0f, 0.0f, 300.0f), FRotator::ZeroRotator, RC33AlwaysSpawn());
	Seeker->SetDestination(Target);
	float MaxX = Seeker->GetActorLocation().X;
	for (int32 Step = 0; Step < 50 && IsValid(Seeker); ++Step)
	{
		WoTTest::TickWorld(World, 0.1f);
		if (IsValid(Seeker))
		{
			MaxX = FMath::Max(MaxX, static_cast<float>(Seeker->GetActorLocation().X));
		}
	}
	TestTrue(TEXT("seeking Seeker crosses the mover toward its target"),
		!IsValid(Seeker) || MaxX > 270.0f);
	WoTTest::DestroySpecWorld(World);

	UWorld* Control = WoTTest::CreateSpecWorld();
	SpawnMoverSlab(Control, FVector(250.0f, 0.0f, 300.0f));
	AWoTSeekerProjectile* Plain = Control->SpawnActor<AWoTSeekerProjectile>(
		FVector(0.0f, 0.0f, 300.0f), FRotator::ZeroRotator, RC33AlwaysSpawn());
	Plain->SetDestination(nullptr);
	Plain->ProjectileMovement->Velocity = FVector(150.0f, 0.0f, 0.0f);
	for (int32 Step = 0; Step < 30 && IsValid(Plain); ++Step)
	{
		WoTTest::TickWorld(Control, 0.1f);
	}
	TestTrue(TEXT("a Seeker that is not seeking still stops at the mover"),
		!IsValid(Plain) || Plain->GetActorLocation().X < 250.0f);
	WoTTest::DestroySpecWorld(Control);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33LegionSeekingCylinderNetSpec,
	"WoT.RC33.G11.LegionSeekingCylinderNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33LegionSeekingCylinderNetSpec::RunTest(const FString&)
{
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestProjectileCollisionTarget* Target =
			World->SpawnActor<AWoTTestProjectileCollisionTarget>(
				FVector(1000.0f, 0.0f, 900.0f), FRotator::ZeroRotator,
				RC33AlwaysSpawn());
		AWoTLegionProjectile* Seeking = World->SpawnActor<AWoTLegionProjectile>(
			FVector(0.0f, 0.0f, 300.0f), FRotator(30.0f, 0.0f, 0.0f),
			RC33AlwaysSpawn());
		Seeking->SetDestination(Target);
		WoTTest::TickWorld(World, 0.1f);
		UPrimitiveComponent* Active = Seeking->GetActiveSweptCollision();
		TestEqual(TEXT("inclined seeking changes collision on active cylinder root"),
			Active ? Active->GetCollisionResponseToChannel(ECC_WorldStatic) : ECR_MAX,
			ECR_Ignore);
		TestEqual(TEXT("seeking ignores movers too (UE1 IsBlockedBy: brushes need bCollideWorld)"),
			Active ? Active->GetCollisionResponseToChannel(ECC_WorldDynamic) : ECR_MAX,
			ECR_Ignore);
		TestEqual(TEXT("inclined seeking keeps cylinder pitch world-up"),
			Seeking->GetRootComponent()->GetComponentRotation().Pitch, 0.0, 0.01);
		TestEqual(TEXT("inclined seeking keeps cylinder roll world-up"),
			Seeking->GetRootComponent()->GetComponentRotation().Roll, 0.0, 0.01);
		TestTrue(TEXT("inclined seeking rotates presentation to flight"),
			FMath::Abs(Seeking->GetProjectilePresentationRoot()
				->GetComponentRotation().Pitch) > 1.0f);
		Seeking->SetDestination(nullptr);
		TestEqual(TEXT("leaving seek re-blocks world on active cylinder root"),
			Active ? Active->GetCollisionResponseToChannel(ECC_WorldStatic) : ECR_MAX,
			ECR_Block);
		TestEqual(TEXT("leaving seek re-blocks movers on active cylinder root"),
			Active ? Active->GetCollisionResponseToChannel(ECC_WorldDynamic) : ECR_MAX,
			ECR_Block);

		SpawnRC33Wall(World, FVector(150.0f, 1000.0f, 300.0f),
			FVector(5.0f, 300.0f, 300.0f));
		AWoTLegionProjectile* Straight = World->SpawnActor<AWoTLegionProjectile>(
			FVector(0.0f, 1000.0f, 300.0f), FRotator::ZeroRotator,
			RC33AlwaysSpawn());
		Straight->SetDestination(nullptr);
		TestEqual(TEXT("non-seeking spirit actively blocks world geometry"),
			Straight->GetActiveSweptCollision()->GetCollisionResponseToChannel(
				ECC_WorldStatic), ECR_Block);
		WoTTest::TickWorld(World, 1.0f);
		TestTrue(TEXT("non-seeking cylinder cannot clip through wall"),
			!IsValid(Straight) || Straight->GetActorLocation().X < 170.0f);
		WoTTest::DestroySpecWorld(World);
	}

#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 persistent Legion cylinder replication"));
	if (!TestTrue(TEXT("Legion replication server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("Legion replication client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTLegionProjectile* ServerSpirit =
		Worlds.Server.GetWorld()->SpawnActor<AWoTLegionProjectile>(
			FVector(0.0f, 3000.0f, 5000.0f), FRotator(25.0f, 0.0f, 0.0f),
			RC33AlwaysSpawn());
	ServerSpirit->ProjectileMovement->Velocity = FVector(250.0f, 0.0f, 150.0f);
	ServerSpirit->AlignPresentationToVelocity(
		ServerSpirit->ProjectileMovement->Velocity);
	ServerSpirit->ForceNetUpdate();
	AWoTLegionProjectile* ClientSpirit = nullptr;
	TestTrue(TEXT("persistent Legion flight replicates into client world"),
		Worlds.TickUntil([&Worlds, &ClientSpirit]()
		{
			TActorIterator<AWoTLegionProjectile> It(Worlds.Clients[0].GetWorld());
			ClientSpirit = It ? *It : nullptr;
			return ClientSpirit != nullptr;
		}, 120));
	if (ClientSpirit)
	{
		const FVector InitialClientLocation = ClientSpirit->GetActorLocation();
		Worlds.TickAll(30);
		TestTrue(TEXT("replicated Legion copy advances in flight"),
			FVector::Dist(InitialClientLocation, ClientSpirit->GetActorLocation()) > 10.0f);
		TestTrue(TEXT("replicated Legion position follows authority"),
			FVector::Dist(ServerSpirit->GetActorLocation(),
				ClientSpirit->GetActorLocation()) < 100.0f);
		TestNotNull(TEXT("replicated Legion active root remains a flat cylinder"),
			Cast<UWoTFlatCylinderComponent>(ClientSpirit->GetRootComponent()));
		TestEqual(TEXT("replicated Legion cylinder stays world-up"),
			ClientSpirit->GetRootComponent()->GetComponentRotation().Pitch, 0.0, 0.1);
		const double ClientPresentationPitch = ClientSpirit
			->GetProjectilePresentationRoot()->GetComponentRotation().Pitch;
		TestTrue(*FString::Printf(TEXT("replicated Legion presentation retains inclined flight "
			"(pitch %.3f, velocity %s, replicated %s)"), ClientPresentationPitch,
			*ClientSpirit->GetVelocity().ToCompactString(),
			*ClientSpirit->GetReplicatedMovement().LinearVelocity.ToCompactString()),
			FMath::Abs(ClientPresentationPitch) > 1.0);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33GuardianPlacementSpec,
	"WoT.RC33.G17.GuardianTracePlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33GuardianPlacementSpec::RunTest(const FString&)
{
	{
		UWorld* Standalone = WoTTest::CreateSpecWorld();
		AWoTPlayerController* StandalonePC = nullptr;
		AWoTCharacter* StandaloneCaster = SpawnRC33Player(
			Standalone, FVector(0.0f, 0.0f, 100.0f), 1, StandalonePC);
		StandaloneCaster->SummonFaction = EWoTCreatureFaction::Hound;
		StandaloneCaster->SetActorRotation(FRotator::ZeroRotator);
		StandalonePC->SetControlRotation(FRotator::ZeroRotator);
		SpawnRC33Wall(Standalone, FVector(80.0f, 0.0f, 100.0f),
			FVector(5.0f, 500.0f, 500.0f));
		AWoTAngrealInvGuardian* StandaloneGuardian =
			Standalone->SpawnActor<AWoTAngrealInvGuardian>(RC33AlwaysSpawn());
		StandaloneGuardian->SetOwner(StandaloneCaster);
		StandaloneGuardian->CurCharges = 3;
		StandaloneGuardian->CastArtifact();
		AWoTDecal* Crack = StandaloneGuardian->LastSpawnedCrackForTest.Get();
		TestNotNull(TEXT("standalone Guardian spawns fitted crack presentation"), Crack);
		if (Crack)
		{
			TestTrue(TEXT("crack uses exact authoritative fit location"),
				Crack->GetActorLocation().Equals(
					StandaloneGuardian->LastValidatedCrackTransformForTest.GetLocation(),
					0.001f));
			TestTrue(TEXT("crack uses exact authoritative random-roll rotation"),
				Crack->GetActorRotation().Equals(
					StandaloneGuardian->LastValidatedCrackTransformForTest.Rotator(),
					0.001f));
			TestEqual(TEXT("crack uses exact authoritative fitted scale"),
				Crack->FinalScale,
				StandaloneGuardian->LastValidatedCrackScaleForTest, 0.001f);
		}
		WoTTest::DestroySpecWorld(Standalone);
	}
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 Guardian dedicated authoritative fit"));
	if (!TestTrue(TEXT("dedicated Guardian server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("dedicated Guardian owner connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* World = Worlds.Server.GetWorld();
	TestEqual(TEXT("Guardian fixture is a real dedicated server"),
		World->GetNetMode(), NM_DedicatedServer);
	AWoTPlayerController* PC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* Caster = PC ? Cast<AWoTCharacter>(PC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("dedicated Guardian authority caster"), Caster))
	{
		return false;
	}
	Caster->SetActorLocation(FVector(0, 0, 100));
	Caster->SetActorRotation(FRotator::ZeroRotator);
	PC->SetControlRotation(FRotator::ZeroRotator);
	Caster->SummonFaction = EWoTCreatureFaction::Hound;

	AWoTAngrealInvGuardian* Guardian =
		World->SpawnActor<AWoTAngrealInvGuardian>(RC33AlwaysSpawn());
	Guardian->SetOwner(Caster);
	Guardian->CurCharges = 3;
	Guardian->CastArtifact();
	TestFalse(TEXT("dedicated no-fit path spawns no guide"),
		Guardian->CurrentGuide.IsValid());
	TestEqual(TEXT("dedicated no-fit path spends no charge"),
		Guardian->CurCharges, 3);

	SpawnRC33Wall(World, FVector(80, 0, 100), FVector(5.0f, 500.0f, 500.0f));
	Guardian = World->SpawnActor<AWoTAngrealInvGuardian>(RC33AlwaysSpawn());
	Guardian->SetOwner(Caster);
	Guardian->CurCharges = 3;
	Guardian->CastArtifact();
	AWoTMashadarGuide* Guide = Guardian->CurrentGuide.Get();
	TestNotNull(TEXT("dedicated authoritative fit spawns a guide"), Guide);
	if (Guide)
	{
		TestEqual(TEXT("arena guide multiplayer max speed"), Guide->MaxSpeed, 400.0f);
		TestEqual(TEXT("arena guide acceleration"), Guide->Acceleration, 15.0f);
		TestEqual(TEXT("arena guide search radius"), Guide->SearchRadius, 1000.0f);
		TestTrue(TEXT("arena guide deploys from the authoritative fit"),
			Guide->IsDeployed());
	}
	TestEqual(TEXT("successful dedicated placement consumes one charge"),
		Guardian->CurCharges, 2);
	int32 CrackCount = 0;
	AWoTDecal* ServerCrack = nullptr;
	for (TActorIterator<AWoTDecal> It(World); It; ++It)
	{
		if (It->DecalType == TEXT("MashadarCrack"))
		{
			++CrackCount;
			ServerCrack = *It;
		}
	}
	TestEqual(TEXT("dedicated authority retains one replicated crack payload"),
		CrackCount, 1);
	AWoTDecal* ClientCrack = nullptr;
	TestTrue(TEXT("connected client receives the replicated crack payload"),
		Worlds.TickUntil([&Worlds, &ClientCrack]()
		{
			for (TActorIterator<AWoTDecal> It(Worlds.Clients[0].GetWorld()); It; ++It)
			{
				if (It->DecalType == TEXT("MashadarCrack"))
				{
					ClientCrack = *It;
					return It->IsPlaced();
				}
			}
			return false;
		}, 120));
	if (ServerCrack && ClientCrack)
	{
		TestTrue(TEXT("client crack keeps the authority validated location"),
			ClientCrack->GetActorLocation().Equals(
				Guardian->LastValidatedCrackTransformForTest.GetLocation(), 0.001f));
		TestTrue(TEXT("client crack keeps the authority validated rotation without reroll"),
			ClientCrack->GetActorRotation().Equals(
				Guardian->LastValidatedCrackTransformForTest.Rotator(), 0.001f));
		TestEqual(TEXT("client crack keeps the authority fitted scale without a second fit"),
			ClientCrack->FinalScale, Guardian->LastValidatedCrackScaleForTest, 0.001f);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33MashadarOncePerTickDamageSpec,
	"WoT.RC33.PresentationA.M3.MashadarOncePerTickDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33MashadarOncePerTickDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FTransform Spawn(FRotator::ZeroRotator, FVector(0, 0, 200));
	AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), Spawn, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("production Mashadar guide spawns"), Guide))
	{
		return false;
	}
	Guide->FinishSpawning(Spawn);
	Guide->Team = 1;
	Guide->Deploy();
	AWoTTestNPCCharacter* Target = SpawnRC33NPC(
		World, FVector(100, 0, 200), 2);
	Guide->NotifySeePawn(Target);
	// Moving the live guide away from its authored lair makes UpdateChain append
	// the production body segment that a radial explosion can overlap with its head.
	Guide->SetActorLocation(FVector(200, 0, 200));
	WoTTest::TickWorld(World, 0.05f);
	AWoTMashadarSegment* Head = Guide->GetMasterSegment();
	AWoTMashadarSegment* Body = Head
		? Cast<AWoTMashadarSegment>(Head->ChildTrailer) : nullptr;
	if (!TestNotNull(TEXT("production chain has a head"), Head)
		|| !TestNotNull(TEXT("production chain has a second segment"), Body))
	{
		return false;
	}
	const int32 Before = Guide->MashadarHealth;
	FDamageEvent Burst;
	Head->TakeDamage(12.0f, Burst, nullptr, Guide);
	Body->TakeDamage(12.0f, Burst, nullptr, Guide);
	TestEqual(TEXT("one same-frame explosion over two segments drains one hit"),
		Before - Guide->MashadarHealth, 12);
	WoTTest::TickWorld(World, 0.05f);
	Body->TakeDamage(12.0f, Burst, nullptr, Guide);
	TestEqual(TEXT("guide clears the latch on the following world frame"),
		Before - Guide->MashadarHealth, 24);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33TeleporterNoiseSpec,
	"WoT.RC33.G22.TeleporterMakeNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33TeleporterNoiseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* SourceListener = SpawnRC33NPC(
		World, FVector(500, 0, 100), 1);
	AWoTTestNPCCharacter* DestinationListener = SpawnRC33NPC(
		World, FVector(6500, 0, 100), 1);
	AWoTTeleporter* Source = World->SpawnActor<AWoTTeleporter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, RC33AlwaysSpawn());
	AWoTTeleporter* Dest = World->SpawnActor<AWoTTeleporter>(
		FVector(6000, 0, 100), FRotator::ZeroRotator, RC33AlwaysSpawn());
	Source->URL = TEXT("Exit");
	Dest->TeleTag = TEXT("Exit");
	AWoTPlayerController* PC = nullptr;
	AWoTCharacter* Traveler = SpawnRC33Player(World, FVector(5000, 0, 100), 2, PC);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* SourceController =
		CastChecked<AWoTNPCController>(SourceListener->GetController());
	AWoTNPCController* DestinationController =
		CastChecked<AWoTNPCController>(DestinationListener->GetController());
	SourceController->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	DestinationController->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	SourceListener->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
	DestinationListener->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
	WoTTest::PlaceWithoutOverlapEvents(Traveler, Source->GetActorLocation());
	Source->NotifyActorBeginOverlap(Traveler);
	TestTrue(TEXT("source listener hears MakeNoise before Accept"),
		SourceListener->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestFalse(TEXT("destination-only listener more than 4000uu away hears nothing"),
		DestinationListener->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("source noise acquires the traveler"),
		SourceListener->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
		static_cast<AActor*>(Traveler));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33HardLandingDisguiseNetSpec,
	"WoT.RC33.G6G10.HardLandingDisguiseNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33HardLandingDisguiseNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("RC33 hard landing prediction and MP disguise"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("autonomous client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTPlayerController* ClientPC = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* ServerPawn = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	AWoTCharacter* ClientPawn = ClientPC
		? Cast<AWoTCharacter>(ClientPC->GetPawn()) : nullptr;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	if (!TestNotNull(TEXT("authority production pawn"), ServerPawn)
		|| !TestNotNull(TEXT("autonomous production pawn"), ClientPawn))
	{
		return false;
	}
	TestEqual(TEXT("fixture uses a real dedicated server"),
		Worlds.Server.GetWorld()->GetNetMode(), NM_DedicatedServer);
	TestEqual(TEXT("owner is an autonomous proxy"),
		ClientPawn->GetLocalRole(), ROLE_AutonomousProxy);

	FHitResult FloorHit;
	FloorHit.Normal = FVector::UpVector;
	ClientPawn->GetCharacterMovement()->Velocity = FVector(0, 0, -1099.0f);
	ClientPawn->Landed(FloorHit);
	TestFalse(TEXT("autonomous impact just above the retail boundary is not frozen"),
		ClientPawn->IsHardLanding());
	ServerPawn->GetCharacterMovement()->Velocity = FVector(0, 0, -1099.0f);
	ServerPawn->Landed(FloorHit);
	TestFalse(TEXT("authority impact just above the retail boundary is not frozen"),
		ServerPawn->IsHardLanding());

	AWoTAngrealInvLightning* Lightning = Cast<AWoTAngrealInvLightning>(
		ServerPawn->InventoryComponent->FindInventoryType(
			AWoTAngrealInvLightning::StaticClass()));
	if (!Lightning)
	{
		Lightning = ServerWorld->SpawnActor<AWoTAngrealInvLightning>(RC33AlwaysSpawn());
		Lightning->CurCharges = 20;
		ServerPawn->InventoryComponent->AddInventory(Lightning);
	}
	if (Lightning) { Lightning->CurCharges = 20; }
	ServerPawn->InventoryComponent->SelectItem(Lightning);
	Worlds.TickAll(2);
	ServerPawn->InventoryComponent->UseAngreal();
	TestTrue(TEXT("production sustained lightning is active before landing"),
		Lightning && Lightning->bCasting);

	ClientPawn->GetCharacterMovement()->Velocity = FVector(0, 0, -1100.0f);
	ClientPawn->Landed(FloorHit); // local prediction first
	TestTrue(TEXT("autonomous impact at the exact retail boundary predicts freeze"),
		ClientPawn->IsHardLanding());
	TestEqual(TEXT("predicted landing changes only the effective latch"),
		static_cast<int32>(ClientPawn->PlayerMovementConstraint),
		static_cast<int32>(EWoTPlayerMovementConstraint::None));
	TestEqual(TEXT("predicted landing latch is LandingFreeze"),
		static_cast<int32>(ClientPawn->GetEffectivePlayerMovementConstraint()),
		static_cast<int32>(EWoTPlayerMovementConstraint::LandingFreeze));
	TestEqual(TEXT("predicted hard landing has zero movement speed"),
		ClientPawn->GetCharacterMovement()->GetMaxSpeed(), 0.0f);
	ClientPC->Jump();
	TestFalse(TEXT("predicted hard landing rejects jump"), ClientPawn->bPressedJump);

	ServerPawn->GetCharacterMovement()->Velocity = FVector(0, 0, -1100);
	ServerPawn->Landed(FloorHit);
	TestFalse(TEXT("authority landing immediately ceases the sustained angreal"),
		Lightning && Lightning->bCasting);
	const int32 LightningChargesAfterLanding = Lightning
		? Lightning->CurCharges : INDEX_NONE;
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("authority and owner share the hard-landing interval"),
		Worlds.TickUntil([ServerPawn, ClientPawn]()
		{
			return ServerPawn->IsHardLanding() && ClientPawn->IsHardLanding()
				&& ClientPawn->PlayerMovementConstraint
					== EWoTPlayerMovementConstraint::LandingFreeze;
		}, 120));
	TestEqual(TEXT("authority represents the freeze in the shared enum"),
		static_cast<int32>(ServerPawn->PlayerMovementConstraint),
		static_cast<int32>(EWoTPlayerMovementConstraint::LandingFreeze));
	TestEqual(TEXT("owner receives LandingFreeze through the shared enum"),
		static_cast<int32>(ClientPawn->PlayerMovementConstraint),
		static_cast<int32>(EWoTPlayerMovementConstraint::LandingFreeze));
	ServerPawn->ConsumeMovementInputVector();
	ClientPawn->ConsumeMovementInputVector();
	ServerPawn->AddForwardInputForTest(1.0f);
	ClientPawn->AddForwardInputForTest(1.0f);
	TestTrue(TEXT("authority movement is blocked for the shared landing interval"),
		ServerPawn->GetPendingMovementInputVector().IsNearlyZero());
	TestTrue(TEXT("owning-client movement is blocked for the shared landing interval"),
		ClientPawn->GetPendingMovementInputVector().IsNearlyZero());
	const AWoTAngrealInventory* Selected = Cast<AWoTAngrealInventory>(
		ServerPawn->InventoryComponent->SelectedItem.Get());
	TestNotNull(TEXT("production pawn has a selected angreal for fire gate"), Selected);
	const int32 ChargesBefore = Selected ? Selected->CurCharges : INDEX_NONE;
	ServerPawn->ServerUseAngreal_Implementation(FRotator::ZeroRotator);
	TestEqual(TEXT("forged authority cast is rejected for the entire landing"),
		Selected ? Selected->CurCharges : INDEX_NONE, ChargesBefore);
	ClientPawn->UseAngreal();
	Worlds.TickAll(4);
	TestEqual(TEXT("owning-client fire input is latched off while frozen"),
		Selected ? Selected->CurCharges : INDEX_NONE, ChargesBefore);
	TestEqual(TEXT("ceased sustained cast consumes no further charge while frozen"),
		Lightning ? Lightning->CurCharges : INDEX_NONE, LightningChargesAfterLanding);
	const float FirstLandSeconds =
		ServerPawn->LastHardLandingCompletionSecondsForTest;
	ServerPawn->ChanceOfHitAnim = 1.0f;
	ServerPawn->PlayHitReactAnim(10, FName(TEXT("Punched")));
	const float ShortHitSeconds =
		ServerPawn->LastHardLandingCompletionSecondsForTest;
	TestTrue(*FString::Printf(TEXT("soft TakeHit replaces Land with a shorter "
		"production interval (land=%.3f hit=%.3f table=%s sequence=%s)"),
		FirstLandSeconds, ShortHitSeconds, *ServerPawn->AnimTableName.ToString(),
		*ServerPawn->RepAnim.Sequence.ToString()),
		ShortHitSeconds > 0.0f && ShortHitSeconds < FirstLandSeconds);
	const double ShortStart = ServerWorld->GetTimeSeconds();
	TestTrue(TEXT("shorter TakeHit end releases authority and owner before Land would"),
		Worlds.TickUntil([ServerPawn, ClientPawn]()
		{
			return !ServerPawn->IsHardLanding() && !ClientPawn->IsHardLanding();
		}, 240));
	TestTrue(TEXT("short replacement owns the authoritative release clock"),
		ServerWorld->GetTimeSeconds() - ShortStart < FirstLandSeconds);

	ServerPawn->GetCharacterMovement()->Velocity = FVector(0, 0, -1200);
	ServerPawn->Landed(FloorHit);
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("second authority landing freeze reaches the owner"),
		Worlds.TickUntil([ServerPawn, ClientPawn]()
		{
			return ServerPawn->IsHardLanding() && ClientPawn->IsHardLanding();
		}, 120));
	const float SecondLandSeconds =
		ServerPawn->LastHardLandingCompletionSecondsForTest;
	ServerPawn->PlayHitReactAnim(30, FName(TEXT("Punched")));
	const float LongHitSeconds =
		ServerPawn->LastHardLandingCompletionSecondsForTest;
	TestTrue(*FString::Printf(TEXT("hard TakeHit replaces Land with its distinct "
		"positive production interval (land=%.3f hit=%.3f table=%s sequence=%s)"),
		SecondLandSeconds, LongHitSeconds, *ServerPawn->AnimTableName.ToString(),
		*ServerPawn->RepAnim.Sequence.ToString()),
		LongHitSeconds > 0.0f
			&& !FMath::IsNearlyEqual(LongHitSeconds, SecondLandSeconds, 0.001f));
	const double LongStart = ServerWorld->GetTimeSeconds();
	TestTrue(TEXT("hard TakeHit owns the freeze until its production interval ends"),
		Worlds.TickUntil([ServerPawn, LongHitSeconds, LongStart, ServerWorld]()
		{
			return ServerWorld->GetTimeSeconds() - LongStart
				>= FMath::Max(0.0f, LongHitSeconds - 0.03f)
				&& ServerPawn->IsHardLanding();
		}, 240));
	TestTrue(TEXT("longer TakeHit end releases authority and owner"),
		Worlds.TickUntil([ServerPawn, ClientPawn]()
		{
			return !ServerPawn->IsHardLanding() && !ClientPawn->IsHardLanding();
		}, 240));

	// Exercise the production CMC fall path rather than manufacturing only a
	// Landed callback. Both worlds get equivalent non-network test geometry.
	const FVector FloorLocation(0.0f, 12000.0f, 0.0f);
	auto SpawnFallFloor = [](UWorld* World, const FVector& Location)
	{
		AWoTCollisionBlocker* Floor = World->SpawnActorDeferred<AWoTCollisionBlocker>(
			AWoTCollisionBlocker::StaticClass(), FTransform(Location));
		Floor->Configure(EWoTCollisionBlockerMode::All, 3000.0f, 20.0f);
		Floor->FinishSpawning(FTransform(Location));
		return Floor;
	};
	SpawnFallFloor(Worlds.Server.GetWorld(), FloorLocation);
	SpawnFallFloor(Worlds.Clients[0].GetWorld(), FloorLocation);
	// The interval checks above manufacture three independent landings on one
	// pawn. Restore health so their cumulative fall damage cannot turn this
	// separate production-CMC fall scenario into a death-before-PlayLanded case.
	ServerPawn->Health = ServerPawn->MaxHealth = 100000;
	const FVector FallStart = FloorLocation + FVector(0.0f, 0.0f, 1600.0f);
	ServerPawn->SetActorLocation(FallStart, false, nullptr,
		ETeleportType::TeleportPhysics);
	ServerPawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	ServerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	ServerPawn->LastLandingImpactVelocityForTest = 0.0f;
	const int32 HardLandingEntriesBeforeFall =
		ServerPawn->HardLandingEnterCountForTest;
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("production authority fall crosses threshold and enters LandingFreeze"),
		Worlds.TickUntil([ServerPawn, HardLandingEntriesBeforeFall]()
		{
			return ServerPawn->LastLandingImpactVelocityForTest <= -1100.0f
				&& ServerPawn->HardLandingEnterCountForTest
					> HardLandingEntriesBeforeFall;
		}, 360));
	TestTrue(TEXT("production fall travels a threshold-scale distance"),
		FallStart.Z - ServerPawn->GetActorLocation().Z > 600.0f);
	TestTrue(TEXT("authority production impact is at or below -1100"),
		ServerPawn->LastLandingImpactVelocityForTest <= -1100.0f);
	TestTrue(TEXT("production fall uses a positive Land animation interval"),
		ServerPawn->LastHardLandingCompletionSecondsForTest > 0.0f);
	const bool bProductionLandingReleased =
		Worlds.TickUntil([ServerPawn, ClientPawn]()
		{
			return !ServerPawn->IsHardLanding() && !ClientPawn->IsHardLanding();
		}, 240);
	TestTrue(*FString::Printf(TEXT("production Land animation releases both roles "
		"(server=%d client=%d serverState=%d clientState=%d entries=%d interval=%.3f)"),
		ServerPawn->IsHardLanding() ? 1 : 0,
		ClientPawn->IsHardLanding() ? 1 : 0,
		static_cast<int32>(ServerPawn->PlayerMovementConstraint),
		static_cast<int32>(ClientPawn->PlayerMovementConstraint),
		ServerPawn->HardLandingEnterCountForTest,
		ServerPawn->LastHardLandingCompletionSecondsForTest),
		bProductionLandingReleased);

	const float ServerSpeed = ServerPawn->GetCharacterMovement()->MaxWalkSpeed;
	const float ClientSpeed = ClientPawn->GetCharacterMovement()->MaxWalkSpeed;
	AWoTCharacter* Mimic = Worlds.Server.GetWorld()->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	Mimic->GetCharacterMovement()->MaxWalkSpeed = 233.0f;
	TestTrue(TEXT("authority applies the production disguise state"),
		ServerPawn->ApplyDisguiseFrom(Mimic));
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("replicated disguise reaches the real owning client"),
		Worlds.TickUntil([ClientPawn]() { return ClientPawn->IsDisguised(); }, 120));
	TestEqual(TEXT("dedicated authority does not copy disguise speed"),
		ServerPawn->GetCharacterMovement()->MaxWalkSpeed, ServerSpeed);
	TestEqual(TEXT("multiplayer owner does not copy disguise speed"),
		ClientPawn->GetCharacterMovement()->MaxWalkSpeed, ClientSpeed);
	ServerPawn->RevertDisguise();
	ServerPawn->ForceNetUpdate();
	TestTrue(TEXT("disguise revert replicates"),
		Worlds.TickUntil([ClientPawn]() { return !ClientPawn->IsDisguised(); }, 120));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33TrapCreditWardAuthorityNetSpec,
	"WoT.RC33.G18G25.TrapCreditWardAuthorityNet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33TrapCreditWardAuthorityNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	// Net test worlds load /Engine/Maps/Entry directly, outside the normal WoT
	// bootstrap. Hold the standard gameplay-spec activation while the dedicated
	// world is constructed so its production-gated zone index comes online.
	UWorld* ActivationWorld = WoTTest::CreateSpecWorld();
	struct FActivationWorldGuard
	{
		UWorld* World = nullptr;
		~FActivationWorldGuard() { WoTTest::DestroySpecWorld(World); }
	} ActivationGuard{ActivationWorld};
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTBattleGameMode"));
	Worlds.SetDescription(TEXT("RC33 trap credit and ExpWard authority"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	// Entry has no authored BattleInfo. Install the production game-mode fixture
	// before either login because giMPBattle::PostLogin immediately assigns a team.
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	AWoTBattleGameMode* BattleMode =
		ServerWorld->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* Battle = ServerWorld->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 3;
	Battle->TeamClass[0] = AWoTCharacter::StaticClass();
	Battle->TeamClass[1] = AWoTCharacter::StaticClass();
	Battle->TeamClass[2] = AWoTCharacter::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	Battle->bTeleportersDisabled = true;
	BattleMode->Battle = Battle;
	if (!TestTrue(TEXT("killer client connects"), Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("victim client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* KillerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTPlayerController* VictimPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(1));
	AWoTCharacter* Killer = KillerPC ? Cast<AWoTCharacter>(KillerPC->GetPawn()) : nullptr;
	AWoTCharacter* Victim = VictimPC ? Cast<AWoTCharacter>(VictimPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("trap placer pawn"), Killer)
		|| !TestNotNull(TEXT("trap victim pawn"), Victim))
	{
		return false;
	}
	AWoTPlayerState* KillerPS = KillerPC->GetPlayerState<AWoTPlayerState>();
	AWoTPlayerState* VictimPS = VictimPC->GetPlayerState<AWoTPlayerState>();
	KillerPS->Team = 1;
	VictimPS->Team = 2;
	auto SpawnTeamStart = [&](uint8 Team, const FVector& Location)
	{
		AWoTPlayerStart* Start = ServerWorld->SpawnActor<AWoTPlayerStart>(
			Location, FRotator::ZeroRotator, RC33AlwaysSpawn());
		Start->TeamNumber = Team;
		return Start;
	};
	SpawnTeamStart(1, Killer->GetActorLocation() + FVector(2000, 0, 0));
	SpawnTeamStart(2, Victim->GetActorLocation() + FVector(-2000, 0, 0));
	const int32 KillsBefore = KillerPS->Kills;
	AWoTCharacter* DisconnectVictim = nullptr;
	int32 VictimKillsBeforeDisconnectBurn = 0;
	int32 VictimSuicidesBeforeDisconnectBurn = 0;

	// Deploy the production manager before the placer dies.  The manager creates
	// its real slabs and propagates the persistent controller identity to each one.
	const FVector ViewerLocation = KillerPC->GetCitadelViewerLocation();
	const FVector WallStart = ViewerLocation + FVector(200.0f, 0.0f, 50.0f);
	SpawnRC33Wall(ServerWorld, WallStart - FVector(0.0f, 0.0f, 50.0f),
		FVector(1000.0f, 1000.0f, 10.0f));
	SpawnRC33Wall(ServerWorld, WallStart + FVector(600.0f, 0.0f, 50.0f),
		FVector(10.0f, 100.0f, 200.0f));
	AWoTBudgetInfo* Budget=ServerWorld->SpawnActor<AWoTBudgetInfo>(RC33AlwaysSpawn());
	Budget->Team=1; FWoTBudgetResource FireBudget; FireBudget.ResourcePackageName=TEXT("WoTTraps");
	FireBudget.ResourceAdapterName=TEXT("FireWallInventory"); FireBudget.ItemCount=1;
	Budget->Budget.Add(FireBudget);
	const FTransform ZoneX(FRotator::ZeroRotator,ViewerLocation);
	AWoTZoneVolume* EditZone=ServerWorld->SpawnActorDeferred<AWoTZoneVolume>(
		AWoTZoneVolume::StaticClass(),ZoneX);
	EditZone->EditTeam=1; EditZone->bAllowEditing=true;
	EditZone->Collision->SetBoxExtent(FVector(5000)); EditZone->FinishSpawning(ZoneX);
	Worlds.TickAll(2);
	auto SpawnAltar=[&](uint8 Team,const FVector& Location)
	{
		const FTransform X(FRotator::ZeroRotator,Location);
		AWoTSealAltar* Altar=ServerWorld->SpawnActorDeferred<AWoTSealAltar>(
			AWoTSealAltar::StaticClass(),X); Altar->Team=Team; Altar->FinishSpawning(X);
	};
	SpawnAltar(1,WallStart); SpawnAltar(2,FVector(50000,0,0));
	BattleMode->Battle = Battle;
	Battle->bTeleportersDisabled = true;
	TestEqual(TEXT("real controller carries the selected team"), KillerPS->Team,
		uint8(1));
	TestFalse(TEXT("battle fixture remains in the citadel edit phase"),
		BattleMode->CanTeleport());
	TestEqual(TEXT("team budget is uniquely discoverable"),
		BattleMode->FindTeamBudget(1), Budget);
	AWoTZoneVolume* ViewerZone = AWoTZoneVolume::EditZoneAt(
		ServerWorld, KillerPC->GetCitadelViewerLocation());
	TestEqual(TEXT("real pawn is inside its production edit zone"), ViewerZone,
		EditZone);
	KillerPC->CitadelBeginEdit();
	TestTrue(TEXT("real citadel edit session opens"),KillerPC->IsEditingCitadel());
	KillerPC->ServerCreateResource(
		TEXT("FireWallInventory"),WallStart,FVector::ForwardVector);
	AWoTFireWall* FireWall=nullptr;
	for(TActorIterator<AWoTFireWall> It(ServerWorld);It;++It)
		if(IsValid(*It)){FireWall=*It;break;}
	if (!TestNotNull(TEXT("ServerCreateResource creates the production fire-wall manager"), FireWall))
	{
		return false;
	}
	TestEqual(TEXT("real creation path spends the fire-wall budget"),
		Budget->GetItemCount(Budget->GetIndex(TEXT("FireWallInventory"))),0);
	// Resource creation happens during the frozen edit phase. The deployed trap
	// becomes damaging only after the real session end calls EndEditingResource.
	KillerPC->CitadelEndEdit();
	TestFalse(TEXT("real citadel edit session closes before combat"),
		KillerPC->IsEditingCitadel());
	BattleMode->EnableTeleporters();
	TestTrue(TEXT("production battle phase permits trap damage"),
		BattleMode->CanTeleport());
	AWoTFireWallSlab* Slab = nullptr;
	for (TActorIterator<AWoTFireWallSlab> It(ServerWorld); It; ++It)
	{
		if (IsValid(*It) && It->GetOwner() == FireWall)
		{
			Slab = *It;
			break;
		}
	}
	if (TestNotNull(TEXT("production deployment creates a fire-wall slab"), Slab))
	{
		TestTrue(TEXT("activated retail data supplies fire damage"),
			Slab->DamageAmount > 0.0f);
		AWoTCharacter* PlacingPawn = Killer;
		Killer->TakeRawDamage(5000, TEXT("hurt"), VictimPC,
			Killer->GetActorLocation());
		KillerPC->ServerRestartPlayerWoT_Implementation();
		TestTrue(TEXT("placer death and production respawn replace the pawn"),
			Worlds.TickUntil([KillerPC, PlacingPawn]()
			{
				return KillerPC->GetPawn() && KillerPC->GetPawn() != PlacingPawn;
			}, 240));
		TestEqual(TEXT("respawn retains the placing controller's PlayerState"),
			KillerPC->GetPlayerState<AWoTPlayerState>(), KillerPS);
		TestEqual(TEXT("deployed slab retains the placing controller identity"),
			Slab->ResolveDamageInstigatorController(),
			static_cast<AController*>(KillerPC));
		TestFalse(TEXT("deployed slab is active against the enemy team"),
			Slab->IsInactive(Victim));
		TestTrue(TEXT("deployed slab has live fire fuel"), Slab->Health > 0);
		TestFalse(TEXT("deployed slab presentation is not hidden"), Slab->IsHidden());
		Victim->Health = 1;
		for (int32 Burn = 0; Burn < 20 && !Victim->bDead; ++Burn)
		{
			Slab->DamageActor(Victim);
		}
		TestTrue(TEXT("production fire-wall damage kills the one-health victim"),
			Victim->bDead);
		TestEqual(TEXT("pre-respawn deployment credits the respawned placer"),
			KillerPS->Kills, KillsBefore + 1);

		VictimPC->ServerRestartPlayerWoT_Implementation();
		TestTrue(TEXT("first wall victim respawns for disconnect coverage"),
			Worlds.TickUntil([VictimPC, Victim]()
			{
				return VictimPC->GetPawn() && VictimPC->GetPawn() != Victim;
			}, 240));
		DisconnectVictim = Cast<AWoTCharacter>(VictimPC->GetPawn());
		VictimKillsBeforeDisconnectBurn = VictimPS->Kills;
		VictimSuicidesBeforeDisconnectBurn = VictimPS->Suicides;
	}

	FActorSpawnParameters WardParams = RC33AlwaysSpawn();
	APawn* WardInstigator = VictimPC->GetPawn();
	WardParams.Instigator = WardInstigator;
	AWoTExpWardProjectile* ServerWard =
		ServerWorld->SpawnActor<AWoTExpWardProjectile>(
			WardInstigator
				? WardInstigator->GetActorLocation() + FVector(0, 0, 100)
				: FVector(0, 0, 500),
			FRotator::ZeroRotator, WardParams);
	TestNotNull(TEXT("authority ward spawns"), ServerWard);
	if (ServerWard)
	{
		ServerWard->ForceNetUpdate();
	}
	AWoTExpWardProjectile* ClientWard = nullptr;
	TestTrue(TEXT("ward replicates into a real client world"),
		Worlds.TickUntil([&Worlds, &ClientWard]()
		{
			TActorIterator<AWoTExpWardProjectile> It(
				Worlds.Clients[1].GetWorld());
			ClientWard = It ? *It : nullptr;
			return ClientWard != nullptr;
		}, 120));
	if (ClientWard && ServerWard)
	{
		ClientWard->WoTTrigger(nullptr, WardInstigator);
		Worlds.TickAll(4);
		TestTrue(TEXT("simulated-proxy trigger cannot detonate the authority ward"),
			IsValid(ClientWard) && IsValid(ServerWard));
		ServerWard->WoTTrigger(nullptr, WardInstigator);
		TestTrue(TEXT("authority trigger destroys the ward on server and client"),
			Worlds.TickUntil([ServerWard, ClientWard]()
			{
				return !IsValid(ServerWard) && !IsValid(ClientWard);
			}, 120));
	}

	if (TestNotNull(TEXT("disconnect coverage has a live network victim"),
		DisconnectVictim) && TestNotNull(TEXT("disconnect coverage retains the wall"), Slab))
	{
		AWoTGameMode* Mode = ServerWorld->GetAuthGameMode<AWoTGameMode>();
		Mode->Logout(KillerPC);
		DisconnectVictim->Health = 1;
		Slab->DamageActor(DisconnectVictim);
		TestEqual(TEXT("disconnected placer receives no surviving kill credit"),
			VictimPS->Kills, VictimKillsBeforeDisconnectBurn);
		TestEqual(TEXT("a trap whose placer PRI is gone scores as an accident"),
			VictimPS->Suicides, VictimSuicidesBeforeDisconnectBurn + 1);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33CosmeticRNGIsolationSpec,
	"WoT.RC33.F1b.CosmeticRNGIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33CosmeticRNGIsolationSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Listen;
	Listen.Server = CreateRC33ListenServer();
	Listen.SetDescription(TEXT("F1b listen-host cosmetic RNG isolation"));
	if (!TestTrue(TEXT("real listen host loads"), Listen.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* HostWorld = Listen.Server.GetWorld();
	TestEqual(TEXT("RNG fixture is a real listen host"),
		HostWorld->GetNetMode(), NM_ListenServer);
	AWoTBeamEffect* Beam = HostWorld->SpawnActor<AWoTBeamEffect>(RC33AlwaysSpawn());
	if (!TestNotNull(TEXT("production lightning streak"), Beam))
	{
		return false;
	}
	Beam->Config = TEXT("LightningStreak02");
	Beam->Start = FVector(0, 0, 100);
	Beam->End = FVector(384, 0, 100);

	auto PreservesGlobalRNG = [this](const FString& Label,
		TFunctionRef<void()> Exercise)
	{
		constexpr int32 Seed = 0x4c535246;
		int32 Expected[4];
		FMath::RandInit(Seed);
		for (int32& Value : Expected) Value = FMath::Rand();
		FMath::RandInit(Seed);
		Exercise();
		bool bPreserved = true;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Expected); ++Index)
		{
			bPreserved &= TestEqual(FString::Printf(
				TEXT("%s preserves global RNG sample %d"), *Label, Index),
				FMath::Rand(), Expected[Index]);
		}
		return bPreserved;
	};

	PreservesGlobalRNG(TEXT("LightningStreak beam"), [Beam]()
	{
		for (int32 Update = 0; Update < 8; ++Update)
		{
			Beam->Tick(1.0f / 60.0f);
		}
	});
	TestTrue(TEXT("listen-host streak exercised per-segment cosmetic picks"),
		Beam->GetStreakTextureAssignmentCount() > 0);

	WoTResetTextureAnimationResourceCountsForTesting();
	const FTransform SprayerAt(FRotator::ZeroRotator, FVector(0, 0, 5000));
	AWoTParticleSprayer* Sprayer =
		HostWorld->SpawnActorDeferred<AWoTParticleSprayer>(
			AWoTParticleSprayer::StaticClass(), SprayerAt, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (TestNotNull(TEXT("listen-host production sprayer"), Sprayer))
	{
		FWoTParticleTemplate Template;
		Template.LifeSpan = 1.0f;
		Template.Weight = 1.0f;
		Template.MinInitialVelocity = 100.0f;
		Template.MaxInitialVelocity = 200.0f;
		Template.MinDrawScale = 0.5f;
		Template.MaxDrawScale = 1.5f;
		Template.MinScaleGlow = 0.5f;
		Template.MaxScaleGlow = 1.0f;
		Template.GrowPhase = 1;
		Template.MinGrowRate = 0.1f;
		Template.MaxGrowRate = 0.2f;
		Template.FadePhase = 1;
		Template.MinFadeRate = -0.2f;
		Template.MaxFadeRate = -0.1f;
		Sprayer->Templates.Add(Template);
		Sprayer->Volume = 120.0f;
		Sprayer->VisibilityRadius = 0.0f;
		Sprayer->VisibilityHeight = 0.0f;
		Sprayer->DecalTypeName = TEXT("F1bMissingDecalProbe");
		Sprayer->DecalPercent = 1.0f;
		Sprayer->SkipPriming();
		Sprayer->FinishSpawning(SprayerAt);
		PreservesGlobalRNG(TEXT("particle sprayer tick/spawn/decal"), [Sprayer]()
		{
			Sprayer->Tick(1.0f / 60.0f);
		});
		const FWoTTextureAnimationResourceCounts Counts =
			WoTGetTextureAnimationResourceCountsForTesting();
		TestTrue(TEXT("listen-host sprayer exercised production particle substeps"),
			Counts.ParticleSubsteps > 0);
	}

	AWoTEarthTremorProjectile* Earth =
		HostWorld->SpawnActor<AWoTEarthTremorProjectile>(FVector(3000, 0, 5000),
			FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (TestNotNull(TEXT("listen-host EarthTremor projectile"), Earth))
	{
		Earth->SetRole(ROLE_SimulatedProxy);
		PreservesGlobalRNG(TEXT("EarthTremor explosion selection"), [Earth]()
		{
			Earth->Explode(Earth->GetActorLocation(), FVector::UpVector);
		});
		TestTrue(TEXT("EarthTremor production explosion chose its retail visual"),
			Earth->ExplosionEffectName == TEXT("FireballExplode")
				|| Earth->ExplosionEffectName == TEXT("FireballExplode2"));
	}

	AWoTDecayProjectile* Decay =
		HostWorld->SpawnActor<AWoTDecayProjectile>(FVector(4000, 0, 5000),
			FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (TestNotNull(TEXT("listen-host Decay projectile"), Decay))
	{
		Decay->SetRole(ROLE_SimulatedProxy);
		PreservesGlobalRNG(TEXT("Decay drip cadence"), [Decay]()
		{
			Decay->Tick(2.1f);
		});
		TestEqual(TEXT("Decay exercised the production drip path"),
			Decay->LastDripConfig, FName(TEXT("DecaySplatter")));
	}

	const FWoTMeshEffectRow* ImpactRow = WoTMeshFX::Find(TEXT("LSImpact"));
	AWoTMeshEffect* Impact = HostWorld->SpawnActor<AWoTMeshEffect>(
		FVector(5000, 0, 5000), FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (TestNotNull(TEXT("production LSImpact row"), ImpactRow)
		&& TestNotNull(TEXT("listen-host LSImpact"), Impact))
	{
		PreservesGlobalRNG(TEXT("mesh-effect configure"), [Impact, ImpactRow]()
		{
			Impact->Configure(TEXT("LSImpact"), *ImpactRow, nullptr);
		});
	}
	AWoTMeshEffect* Ring = WoTMeshFX::SpawnLocal(*HostWorld, TEXT("LSRing"),
		FVector(6000, 0, 5000));
	if (TestNotNull(TEXT("listen-host LSRing"), Ring))
	{
		PreservesGlobalRNG(TEXT("mesh-effect random skin"), [Ring]()
		{
			Ring->Tick(1.0f / 60.0f);
		});
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33BalefireCoronaPawnViewRotationSpec,
	"WoT.RC33.F1b.BalefireCoronaPawnViewRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33BalefireCoronaPawnViewRotationSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Listen;
	Listen.Server = CreateRC33ListenServer();
	Listen.SetDescription(TEXT("Balefire corona pawn view rotation"));
	if (!TestTrue(TEXT("Balefire rotation listen host loads"),
		Listen.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* World = Listen.Server.GetWorld();
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = SpawnRC33Player(
		World, FVector(100.0, 200.0, 300.0), 0, Controller);
	if (Controller)
	{
		Controller->SetAsLocalPlayerController();
		Controller->SetControlRotation(FRotator(0.0, 2.0, 0.0));
	}
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(
		FVector(-1000.0, 500.0, 900.0), FRotator(-83.5, -3.0, 0.0),
		RC33AlwaysSpawn());
	if (!TestNotNull(TEXT("controlled Balefire pawn"), Pawn)
		|| !TestNotNull(TEXT("controlled Balefire controller"), Controller)
		|| !TestNotNull(TEXT("detached Balefire camera"), Camera))
	{
		return false;
	}
	Controller->SetViewTarget(Camera);
	if (Controller->PlayerCameraManager)
	{
		Controller->PlayerCameraManager->bUseClientSideCameraUpdates = false;
		Controller->PlayerCameraManager->UpdateCamera(0.0f);
	}
	auto EyeOf = [](const APawn& Subject)
	{
		return Subject.GetActorLocation()
			+ FVector(0.0, 0.0, Subject.BaseEyeHeight);
	};
	const FVector RelativeOffset(50.0, 0.0, -30.0);
	const FRotator PawnView(0.0, 2.0, 0.0);
	const FVector ExpectedControlled = EyeOf(*Pawn)
		+ PawnView.RotateVector(RelativeOffset);
	AWoTBalefireCorona* Controlled = WoTBalefireFX::SpawnGather(*World, Pawn);
	if (!TestNotNull(TEXT("controlled Balefire corona spawns"), Controlled))
	{
		return false;
	}
	TestTrue(TEXT("controlled corona initially follows pawn ViewRotation"),
		Controlled->GetActorLocation().Equals(ExpectedControlled, 0.01));

	Camera->SetActorRotation(FRotator(37.0, 121.0, 0.0));
	if (Controller->PlayerCameraManager)
	{
		Controller->PlayerCameraManager->UpdateCamera(0.0f);
	}
	TestTrue(TEXT("camera-only change preserves pawn control rotation"),
		Controller->GetControlRotation().Equals(PawnView, 0.01));
	Controlled->Tick(0.0f);
	TestTrue(TEXT("changing only the detached camera rotation does not move corona"),
		Controlled->GetActorLocation().Equals(ExpectedControlled, 0.01));
	AWoTParticleSprayer* Ejected = Controlled->Sprayers.IsEmpty()
		? nullptr : Controlled->Sprayers[0].Get();
	Controlled->Destroy();
	if (TestNotNull(TEXT("Balefire destroy has a sprayer to eject"), Ejected))
	{
		TestTrue(TEXT("destroyed corona ejects along pawn ViewRotation"),
			Ejected->Gravity.Equals(PawnView.Vector() * 10000.0f, 0.01));
	}

	AWoTCharacter* Proxy = World->SpawnActor<AWoTCharacter>(
		FVector(1000.0, -200.0, 500.0), FRotator(0.0, 41.0, 0.0),
		RC33AlwaysSpawn());
	if (!TestNotNull(TEXT("controllerless Balefire proxy"), Proxy))
	{
		return false;
	}
	Proxy->SetRole(ROLE_SimulatedProxy);
	Proxy->SetRemoteViewPitch(23.0);
	TestNull(TEXT("simulated Balefire proxy has no controller"),
		Proxy->GetController());
	const FVector ExpectedProxy = EyeOf(*Proxy)
		+ Proxy->GetBaseAimRotation().RotateVector(RelativeOffset);
	AWoTBalefireCorona* ProxyCorona = WoTBalefireFX::SpawnGather(*World, Proxy);
	if (TestNotNull(TEXT("simulated-proxy Balefire corona spawns"), ProxyCorona))
	{
		TestTrue(TEXT("controllerless proxy uses actor yaw and RemoteViewPitch"),
			ProxyCorona->GetActorLocation().Equals(ExpectedProxy, 0.01));
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33ProductionAnimationNetConsumersSpec,
	"WoT.RC33.F1b.ProductionAnimationNetConsumers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33ProductionAnimationNetConsumersSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	auto Exercise = [this](FTestWorlds& Worlds,
		UWoTSpriteBillboardComponent* Sprite, const FString& Label)
	{
		if (!TestNotNull(Label + TEXT(" production billboard"), Sprite)
			|| !TestTrue(Label + TEXT(" has decoded frames"),
				Sprite && Sprite->HasConfiguredFrames())
			|| !TestTrue(Label + TEXT(" has at least two decoded frames"),
				Sprite && !Sprite->GetCurrentFrameIdentityForTesting().IsEmpty()))
		{
			return false;
		}
		const FString InitialIdentity = Sprite->GetCurrentFrameIdentityForTesting();
		UTexture* InitialTexture = Sprite->GetBoundTextureForTesting();
		if (!TestNotNull(Label + TEXT(" initial production MID texture"),
			InitialTexture))
		{
			return false;
		}
		bool bAdvanced = false;
		for (int32 Tick = 0; Tick < 60 && !bAdvanced; ++Tick)
		{
			Sprite->MarkDrawnForTesting();
			Worlds.TickAll(1);
			bAdvanced = Sprite->GetCurrentFrameIdentityForTesting()
				!= InitialIdentity;
		}
		if (!TestTrue(Label + TEXT(" advances through its production tick"),
			bAdvanced))
		{
			return false;
		}
		const FString CurrentIdentity =
			Sprite->GetCurrentFrameIdentityForTesting();
		int32 Dot = INDEX_NONE;
		const FString ExpectedTextureName =
			CurrentIdentity.FindLastChar(TEXT('.'), Dot)
			? CurrentIdentity.RightChop(Dot + 1) : FString();
		UTexture* Bound = Sprite->GetBoundTextureForTesting();
		return TestNotNull(Label + TEXT(" advanced MID texture"), Bound)
			&& TestTrue(Label + TEXT(" changes the actual bound MID texture"),
				Bound != InitialTexture)
			&& TestEqual(Label + TEXT(" binds the decoded current frame"),
				Bound->GetName(), ExpectedTextureName);
	};

	FTestWorlds Dedicated(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Dedicated.SetDescription(TEXT("F1b production animation clients"));
	Dedicated.SetTickInSeconds(1.0f / 30.0f);
	UWorld* ServerWorld = Dedicated.Server.GetWorld();
	if (!TestTrue(TEXT("production-consumer dedicated server loads"),
		Dedicated.Server.IsLoaded()) || !ServerWorld)
	{
		return false;
	}
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-10000, -1000, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	ServerWorld->SpawnActor<AWoTPlayerStart>(FVector(-10000, 1000, 5000),
		FRotator::ZeroRotator, RC33AlwaysSpawn());
	if (!TestTrue(TEXT("owning rendering client connects"),
		Dedicated.CreateAndConnectClient())
		|| !TestTrue(TEXT("simulated-proxy rendering client connects"),
			Dedicated.CreateAndConnectClient()))
	{
		return false;
	}
	APlayerController* ServerController = ServerWorld->GetFirstPlayerController();
	APawn* ServerPawn = ServerController ? ServerController->GetPawn() : nullptr;
	APawn* OwningPawn = ServerPawn
		? Dedicated.FindReplicatedObjectOnClient(ServerPawn, 0) : nullptr;
	APawn* SimulatedPawn = ServerPawn
		? Dedicated.FindReplicatedObjectOnClient(ServerPawn, 1) : nullptr;
	if (!TestNotNull(TEXT("authority pawn for animation consumers"), ServerPawn)
		|| !TestNotNull(TEXT("owning-client pawn"), OwningPawn)
		|| !TestNotNull(TEXT("simulated-proxy pawn"), SimulatedPawn))
	{
		return false;
	}
	TestEqual(TEXT("first client exercises its autonomous pawn"),
		OwningPawn->GetLocalRole(), ROLE_AutonomousProxy);
	TestEqual(TEXT("second client exercises a simulated pawn"),
		SimulatedPawn->GetLocalRole(), ROLE_SimulatedProxy);

	FActorSpawnParameters ProjectileParams = RC33AlwaysSpawn();
	ProjectileParams.Owner = ServerPawn;
	ProjectileParams.Instigator = ServerPawn;
	AWoTSeekerProjectile* ServerSeeker =
		ServerWorld->SpawnActor<AWoTSeekerProjectile>(FVector(0, 0, 5000),
			FRotator::ZeroRotator, ProjectileParams);
	AWoTEarthTremorProjectile* ServerEarth =
		ServerWorld->SpawnActor<AWoTEarthTremorProjectile>(FVector(5000, 0, 5000),
			FRotator::ZeroRotator, ProjectileParams);
	if (!TestNotNull(TEXT("production Seeker spawns on authority"), ServerSeeker)
		|| !TestNotNull(TEXT("production EarthTremor spawns on authority"),
			ServerEarth))
	{
		return false;
	}
	ServerSeeker->SetActorEnableCollision(false);
	ServerEarth->SetActorEnableCollision(false);
	ServerSeeker->bAlwaysRelevant = true;
	ServerEarth->bAlwaysRelevant = true;
	ServerSeeker->ForceNetUpdate();
	ServerEarth->ForceNetUpdate();
	AWoTSeekerProjectile* OwningSeeker = nullptr;
	AWoTEarthTremorProjectile* SimulatedEarth = nullptr;
	TestTrue(TEXT("production projectiles reach both real client worlds"),
		Dedicated.TickUntil([&]()
		{
			TActorIterator<AWoTSeekerProjectile> SeekerIt(
				Dedicated.Clients[0].GetWorld());
			OwningSeeker = SeekerIt ? *SeekerIt : nullptr;
			TActorIterator<AWoTEarthTremorProjectile> EarthIt(
				Dedicated.Clients[1].GetWorld());
			SimulatedEarth = EarthIt ? *EarthIt : nullptr;
			return OwningSeeker && OwningSeeker->Globe && SimulatedEarth;
		}, 180));
	AWoTBalefireCorona* OwningHand = WoTBalefireFX::SpawnGather(
		*Dedicated.Clients[0].GetWorld(), OwningPawn);
	AWoTBalefireCorona* SimulatedHand = WoTBalefireFX::SpawnGather(
		*Dedicated.Clients[1].GetWorld(), SimulatedPawn);
	if (!TestNotNull(TEXT("Balefire hand on owning client"), OwningHand)
		|| !TestNotNull(TEXT("Balefire hand on simulated proxy"), SimulatedHand)
		|| !OwningSeeker || !OwningSeeker->Globe || !SimulatedEarth)
	{
		return false;
	}
	TestTrue(TEXT("Seeker globe advances on owning client"), Exercise(Dedicated,
		OwningSeeker->Globe->Sprite, TEXT("owning-client Seeker globe")));
	TestTrue(TEXT("Balefire hand advances on owning client"), Exercise(Dedicated,
		OwningHand->HandSprite, TEXT("owning-client Balefire hand")));
	TestTrue(TEXT("EarthTremor advances on simulated proxy"), Exercise(Dedicated,
		SimulatedEarth->Sprite, TEXT("simulated-proxy EarthTremor")));
	TestTrue(TEXT("Balefire hand advances on simulated proxy"), Exercise(Dedicated,
		SimulatedHand->HandSprite, TEXT("simulated-proxy Balefire hand")));

	FTestWorlds Listen;
	Listen.Server = CreateRC33ListenServer();
	Listen.SetDescription(TEXT("F1b production animation listen host"));
	Listen.SetTickInSeconds(1.0f / 30.0f);
	if (!TestTrue(TEXT("production-consumer listen host loads"),
		Listen.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* HostWorld = Listen.Server.GetWorld();
	AWoTPlayerController* HostController = nullptr;
	AWoTCharacter* HostPawn = SpawnRC33Player(HostWorld,
		FVector(0, 0, 5000), 0, HostController);
	if (HostController) HostController->SetAsLocalPlayerController();
	ProjectileParams.Owner = HostPawn;
	ProjectileParams.Instigator = HostPawn;
	AWoTSeekerProjectile* HostSeeker =
		HostWorld->SpawnActor<AWoTSeekerProjectile>(FVector(1000, 0, 5000),
			FRotator::ZeroRotator, ProjectileParams);
	AWoTEarthTremorProjectile* HostEarth =
		HostWorld->SpawnActor<AWoTEarthTremorProjectile>(FVector(5000, 0, 5000),
			FRotator::ZeroRotator, ProjectileParams);
	AWoTBalefireCorona* HostHand = HostPawn
		? WoTBalefireFX::SpawnGather(*HostWorld, HostPawn) : nullptr;
	if (!TestEqual(TEXT("fixture is a real listen host"),
		HostWorld->GetNetMode(), NM_ListenServer)
		|| !TestNotNull(TEXT("listen-host Seeker"), HostSeeker)
		|| !TestNotNull(TEXT("listen-host EarthTremor"), HostEarth)
		|| !TestNotNull(TEXT("listen-host Balefire hand"), HostHand)
		|| !HostSeeker->Globe)
	{
		return false;
	}
	HostSeeker->SetActorEnableCollision(false);
	HostEarth->SetActorEnableCollision(false);
	TestTrue(TEXT("Seeker globe advances on listen host"), Exercise(Listen,
		HostSeeker->Globe->Sprite, TEXT("listen-host Seeker globe")));
	TestTrue(TEXT("Balefire hand advances on listen host"), Exercise(Listen,
		HostHand->HandSprite, TEXT("listen-host Balefire hand")));
	TestTrue(TEXT("EarthTremor advances on listen host"), Exercise(Listen,
		HostEarth->Sprite, TEXT("listen-host EarthTremor")));
#endif
	return !HasAnyErrors();
}

#endif
