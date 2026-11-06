// Headless gates for Arena's login-time faction selection and the shared faction identity.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTVertexAnimComponent.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif

#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	UWorld* PlayerClassTests_CreateWorld(bool bBattleMode = false)
	{
		return bBattleMode
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();
	}

	FActorSpawnParameters PlayerClassTests_AlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerClassOptionSpec,
	"WoT.Rules.PlayerClass.Options",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPlayerClassOptionSpec::RunTest(const FString&)
{
	auto Resolved = [](const TCHAR* Options)
	{
		return AWoTGameMode::ResolvePlayerClassOption(Options).Get();
	};
	TestEqual(TEXT("retail package-qualified form"),
		Resolved(TEXT("?Class=WOTPawns.Forsaken")),
		static_cast<UClass*>(AWoTForsaken::StaticClass()));
	TestEqual(TEXT("bare class form"), Resolved(TEXT("?Class=Hound")),
		static_cast<UClass*>(AWoTHound::StaticClass()));
	TestEqual(TEXT("class option is case-insensitive"),
		Resolved(TEXT("?Class=wOtpAwNs.wHiTeClOaK")),
		static_cast<UClass*>(AWoTWhitecloak::StaticClass()));
	TestEqual(TEXT("class option key is case-insensitive"),
		Resolved(TEXT("?class=Hound")),
		static_cast<UClass*>(AWoTHound::StaticClass()));
	TestEqual(TEXT("explicit AesSedai form"),
		Resolved(TEXT("?Class=WOTPawns.AesSedai")),
		static_cast<UClass*>(AWoTAesSedai::StaticClass()));
	TestEqual(TEXT("unknown class falls back to AesSedai"),
		Resolved(TEXT("?Class=WOTPawns.Myrddraal")),
		static_cast<UClass*>(AWoTAesSedai::StaticClass()));
	TestEqual(TEXT("known bare name in an unknown package is rejected"),
		Resolved(TEXT("?Class=OtherPawns.Forsaken")),
		static_cast<UClass*>(AWoTAesSedai::StaticClass()));
	TestEqual(TEXT("absent class falls back to AesSedai"),
		Resolved(TEXT("?Name=Rand")),
		static_cast<UClass*>(AWoTAesSedai::StaticClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRestartPersonalOptionsSpec,
	"WoT.Rules.PlayerClass.RestartPersonalOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRestartPersonalOptionsSpec::RunTest(const FString&)
{
	const FString HostURL = TEXT("/Game/WoT/Maps/Arena_01?Restart")
		TEXT("?Class=WOTPawns.Hound?Skin=HoundSkins.Hou_Red?Face=Scar")
		TEXT("?Team=2?Name=Lan?FragLimit=20#PlayerStart");
	const FString RemoteURL =
		AWoTGameMode::StripPersonalOptionsFromTravelURL(HostURL);
	TestEqual(TEXT("remote restart keeps only shared travel options"), RemoteURL,
		FString(TEXT("/Game/WoT/Maps/Arena_01?Restart?FragLimit=20#PlayerStart")));
	TestTrue(TEXT("listen-host URL remains available with its own Class option"),
		HostURL.Contains(TEXT("?Class=WOTPawns.Hound")));
	TestEqual(TEXT("unmerged restart request stays relative for remote defaults"),
		AWoTGameMode::StripPersonalOptionsFromTravelURL(
			TEXT("?Restart?Class=WOTPawns.Hound?Name=Lan")),
		FString(TEXT("?Restart")));

	TestEqual(TEXT("personal keys strip case-insensitively"),
		AWoTGameMode::StripPersonalOptionsFromTravelURL(
			TEXT("Arena_02?cLaSs=Forsaken?sKiN=ForsakenSkins.For_Black")
			TEXT("?fAcE=None?tEaM=1?nAmE=Ishamael?Cycle=1")),
		FString(TEXT("Arena_02?Cycle=1")));
	TestEqual(TEXT("similarly named shared options are retained"),
		AWoTGameMode::StripPersonalOptionsFromTravelURL(
			TEXT("Arena_03?ClassLimit=4?TeamDamage=0")),
		FString(TEXT("Arena_03?ClassLimit=4?TeamDamage=0")));
	TestEqual(TEXT("personal FURL removal directives are not broadcast"),
		AWoTGameMode::StripPersonalOptionsFromTravelURL(
			TEXT("?Restart?-Class?-Skin?TimeLimit=20")),
		FString(TEXT("?Restart?TimeLimit=20")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerClassPawnSelectionSpec,
	"WoT.Rules.PlayerClass.PawnSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPlayerClassPawnSelectionSpec::RunTest(const FString&)
{
	UWorld* World = PlayerClassTests_CreateWorld();
	AWoTGameMode* Mode = World->GetAuthGameMode<AWoTGameMode>();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	World->SpawnActor<APlayerStart>(FVector(500, 0, 100), FRotator::ZeroRotator);
	AWoTPlayerState* PS = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Mode || !Controller || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const FUniqueNetIdRepl InvalidUniqueId;
	const FString LoginError = Mode->InitNewPlayer(Controller, InvalidUniqueId,
		TEXT("?Name=Egwene?Class=WOTPawns.Whitecloak"));
	TestTrue(TEXT("fixture login initializes without an error"), LoginError.IsEmpty());
	TestEqual(TEXT("login stores the chosen replicated faction before spawn"),
		PS->FactionClass.Get(), static_cast<UClass*>(AWoTWhitecloak::StaticClass()));
	TestEqual(TEXT("login stores the matching PlayerType"), PS->PlayerType, uint8(3));
	TestEqual(TEXT("base mode selects the player's login-time faction class"),
		Mode->GetDefaultPawnClassForController(Controller),
		static_cast<UClass*>(AWoTWhitecloak::StaticClass()));
	PS->FactionClass = nullptr;
	TestEqual(TEXT("unset replicated identity uses DefaultPlayerClass fallback"),
		Mode->GetDefaultPawnClassForController(Controller),
		static_cast<UClass*>(AWoTAesSedai::StaticClass()));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerTypeMappingSpec,
	"WoT.Rules.PlayerClass.PlayerTypeMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPlayerTypeMappingSpec::RunTest(const FString&)
{
	UWorld* World = PlayerClassTests_CreateWorld();
	FActorSpawnParameters Params = PlayerClassTests_AlwaysSpawnParams();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	AWoTPlayerState* PS = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Controller || !Pawn || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->Possess(Pawn);

	struct FCase
	{
		TSubclassOf<AWoTCharacter> Class;
		uint8 PlayerType;
		EWoTCreatureFaction SummonFaction;
	};
	const FCase Cases[] = {
		{ AWoTAesSedai::StaticClass(), 0, EWoTCreatureFaction::AesSedai },
		{ AWoTForsaken::StaticClass(), 1, EWoTCreatureFaction::Forsaken },
		{ AWoTHound::StaticClass(), 2, EWoTCreatureFaction::Hound },
		{ AWoTWhitecloak::StaticClass(), 3, EWoTCreatureFaction::Whitecloak }
	};
	for (const FCase& Case : Cases)
	{
		Pawn->SetAppearanceClass(Case.Class);
		TestEqual(*FString::Printf(TEXT("%s replicated faction class"),
			*Case.Class->GetName()), PS->FactionClass.Get(), Case.Class.Get());
		TestEqual(*FString::Printf(TEXT("%s PlayerType"), *Case.Class->GetName()),
			PS->PlayerType, Case.PlayerType);
		TestEqual(*FString::Printf(TEXT("%s summon faction"), *Case.Class->GetName()),
			static_cast<uint8>(Pawn->GetSummonFaction()),
			static_cast<uint8>(Case.SummonFaction));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerClassSeamlessCopySpec,
	"WoT.Rules.PlayerClass.SeamlessCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPlayerClassSeamlessCopySpec::RunTest(const FString&)
{
	UWorld* World = PlayerClassTests_CreateWorld();
	AWoTPlayerState* Source = World->SpawnActor<AWoTPlayerState>();
	AWoTPlayerState* Destination = World->SpawnActor<AWoTPlayerState>();
	if (!Source || !Destination)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Source->SetFactionClass(AWoTWhitecloak::StaticClass());
	Source->CopyProperties(Destination);
	TestEqual(TEXT("seamless replacement retains FactionClass"),
		Destination->FactionClass.Get(),
		static_cast<UClass*>(AWoTWhitecloak::StaticClass()));
	TestEqual(TEXT("seamless replacement retains PlayerType"),
		Destination->PlayerType, uint8(3));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleNeutralFactionSpec,
	"WoT.Rules.PlayerClass.BattleNeutralFaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTBattleNeutralFactionSpec::RunTest(const FString&)
{
	UWorld* World = PlayerClassTests_CreateWorld(true);
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTForsaken::StaticClass();
	Battle->TeamClass[1] = AWoTWhitecloak::StaticClass();
	Battle->bTeleportersDisabled = true;
	Battle->FinishSpawning(FTransform::Identity);
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	World->SpawnActor<APlayerStart>(FVector(500, 0, 100), FRotator::ZeroRotator);
	AWoTPlayerState* PS = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Mode || !Battle || !Controller || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mode->Battle = Battle;

	const FUniqueNetIdRepl InvalidUniqueId;
	Mode->InitNewPlayer(Controller, InvalidUniqueId, TEXT("?Class=Hound"));
	TestEqual(TEXT("neutral Battle identity retains incoming SpawnClass"),
		PS->FactionClass.Get(), static_cast<UClass*>(AWoTHound::StaticClass()));
	TestEqual(TEXT("neutral Battle PlayerType matches the URL faction"),
		PS->PlayerType, uint8(2));
	TestEqual(TEXT("neutral Battle spawn selection falls through to URL faction"),
		Mode->GetDefaultPawnClassForController(Controller),
		static_cast<UClass*>(AWoTHound::StaticClass()));

	Mode->RestartPlayer(Controller);
	AWoTCharacter* NeutralPawn = Cast<AWoTCharacter>(Controller->GetPawn());
	TestTrue(TEXT("neutral ?Class=Hound player spawns a Hound pawn"),
		NeutralPawn && NeutralPawn->IsA(AWoTHound::StaticClass()));
	TestTrue(TEXT("neutral URL-faction pawn receives the Battle ghost state"),
		NeutralPawn && NeutralPawn->IsNeutralGhost());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleTeamFactionSpec,
	"WoT.Rules.PlayerClass.BattleTeamWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTBattleTeamFactionSpec::RunTest(const FString&)
{
	UWorld* World = PlayerClassTests_CreateWorld(true);
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTForsaken::StaticClass();
	Battle->TeamClass[1] = AWoTWhitecloak::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	World->SpawnActor<APlayerStart>(FVector(500, 0, 100), FRotator::ZeroRotator);
	AWoTPlayerState* PS = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Mode || !Battle || !Controller || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mode->Battle = Battle;

	// Start with the inherited URL-selected SpawnClass, then prove Battle's team-derived
	// class becomes the more-derived spawn decision after JoinTeam.
	const FUniqueNetIdRepl InvalidUniqueId;
	Mode->InitNewPlayer(Controller, InvalidUniqueId, TEXT("?Class=Hound"));
	TestEqual(TEXT("inherited login seam observed the Arena-style option"),
		PS->FactionClass.Get(), static_cast<UClass*>(AWoTHound::StaticClass()));
	TestTrue(TEXT("Battle accepts the team assignment"), Mode->ChangeTeam(Controller, 0));
	TestEqual(TEXT("Battle JoinTeam replaces the inherited faction identity"),
		PS->FactionClass.Get(), static_cast<UClass*>(AWoTForsaken::StaticClass()));
	TestEqual(TEXT("Battle JoinTeam updates PlayerType through the morph seam"),
		PS->PlayerType, uint8(1));
	TestEqual(TEXT("Battle TeamClass wins over the Arena class option"),
		Mode->GetDefaultPawnClassForController(Controller),
		static_cast<UClass*>(AWoTForsaken::StaticClass()));

	WoTTest::DestroySpecWorld(World);
	return true;
}

namespace
{
	// The body override material's name on the slot named SlotName, or the mesh
	// asset's own slot material name when no override is applied.
	FString PlayerClassTests_SlotMaterialName(const AWoTCharacter* Pawn,
		const TCHAR* SlotName)
	{
		const USkeletalMeshComponent* Body = Pawn->GetMesh();
		const USkeletalMesh* Mesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
		if (!Mesh)
		{
			return FString(TEXT("<no mesh>"));
		}
		const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			if (Slots[Slot].MaterialSlotName == FName(SlotName))
			{
				const UMaterialInterface* Material = Body->GetMaterial(Slot);
				return Material ? Material->GetName() : FString(TEXT("<null>"));
			}
		}
		return FString(TEXT("<no slot>"));
	}

	FString PlayerClassTests_SlotSkinPath(const AWoTCharacter* Pawn,
		const TCHAR* SlotName)
	{
		const UWoTVertexAnimComponent* Body =
			Cast<UWoTVertexAnimComponent>(Pawn->GetMesh());
		const USkeletalMesh* Mesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
		if (!Mesh)
		{
			return FString(TEXT("<no mesh>"));
		}
		const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			if (Slots[Slot].MaterialSlotName == FName(SlotName))
			{
				const FSoftObjectPath* Skin =
					Body->GetRuntimeSkinOverrides().Find(Slot);
				return Skin ? Skin->ToString() : FString(TEXT("<no override>"));
			}
		}
		return FString(TEXT("<no slot>"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerDecodedHealthDefaultSpec,
	"WoT.Rules.PlayerClass.DecodedInheritedHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPlayerDecodedHealthDefaultSpec::RunTest(const FString&)
{
	FString ProductionError;
	const FWoTRuntimeClassDefaults* ProductionDefaults = nullptr;
#if !WOT_GUARDED_BOOTSTRAP
	WoTDevRetailFamiliesEnsureActivated();
	ProductionDefaults = WoTDevRetailClassDefaults();
	if (ProductionDefaults == nullptr)
		ProductionError = TEXT("development retail class-default closure is unavailable");
#else
	ProductionDefaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(ProductionError);
#endif
	if (!TestNotNull(TEXT("production class-default closure is active"),
		ProductionDefaults))
	{
		AddError(ProductionError);
		return false;
	}
	int32 ProductionHealth = 0;
	TestTrue(TEXT("production WOTPlayer inherited Health is reachable"),
		WoTDecodePlayerHealthDefault(
			*ProductionDefaults, ProductionHealth, ProductionError));
	TestTrue(TEXT("production inherited Health is positive"), ProductionHealth > 0);
	auto HealthProperty = [](const int32 Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = TEXT("Health");
		Property.Type = EWoTRuntimeWorldPropertyType::Integer;
		Property.Value.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
		return Property;
	};
	auto RunMutation = [&](const int32 DecodedHealth)
	{
		FWoTRuntimeClassDefaultRecord Pawn;
		Pawn.Identity = TEXT("Engine.Pawn");
		Pawn.Properties.Add(HealthProperty(DecodedHealth));
		FWoTRuntimeClassDefaultRecord WotPawn;
		WotPawn.Identity = TEXT("WOT.WOTPawn");
		WotPawn.SuperclassIdentity = TEXT("Engine.Pawn");
		FWoTRuntimeClassDefaultRecord Player;
		Player.Identity = TEXT("WOT.WOTPlayer");
		Player.SuperclassIdentity = TEXT("WOT.WOTPawn");
		FWoTRuntimeClassDefaults Defaults;
		FString Error;
		if (!TestTrue(TEXT("class-default mutation fixture loads"),
			Defaults.LoadFixture({Pawn, WotPawn, Player}, Error))) return;
		int32 Value = 0;
		TestTrue(TEXT("WOTPlayer resolves inherited Engine.Pawn Health"),
			WoTDecodePlayerHealthDefault(Defaults, Value, Error));
		TestEqual(TEXT("decoded inherited value follows fixture mutation"),
			Value, DecodedHealth);
		UWorld* World = PlayerClassTests_CreateWorld();
		AWoTCharacter* Subject = World->SpawnActor<AWoTCharacter>();
		if (TestNotNull(TEXT("player subject"), Subject))
		{
			TestTrue(TEXT("player accepts authenticated health defaults"),
				Subject->ApplyRetailPlayerHealthDefaults(Defaults, Error));
			TestEqual(TEXT("starting Health follows decoded default"),
				Subject->Health, DecodedHealth);
			TestEqual(TEXT("MaxHealth follows decoded default"),
				Subject->MaxHealth, DecodedHealth);
			TestEqual(TEXT("PlayHit reference follows decoded default"),
				Subject->HitReferenceHealth, static_cast<float>(DecodedHealth));
			TestFalse(TEXT("exact quarter does not qualify (strict retail test)"),
				Subject->PlayerHitQualifies(DecodedHealth / 4, 0.0f));
			TestTrue(TEXT("damage above decoded quarter qualifies"),
				Subject->PlayerHitQualifies(DecodedHealth / 4 + 1, 0.0f));
		}
		// A late joiner's proxy already holds the server's replicated Health
		// when BeginPlay applies the defaults; it must keep that value.
		AWoTCharacter* Proxy = World->SpawnActor<AWoTCharacter>();
		if (TestNotNull(TEXT("replicated proxy subject"), Proxy))
		{
			Proxy->SetRole(ROLE_SimulatedProxy);
			Proxy->Health = 37;
			TestTrue(TEXT("proxy accepts authenticated health defaults"),
				Proxy->ApplyRetailPlayerHealthDefaults(Defaults, Error));
			TestEqual(TEXT("proxy keeps the replicated Health"), Proxy->Health, 37);
			TestEqual(TEXT("proxy MaxHealth follows decoded default"),
				Proxy->MaxHealth, DecodedHealth);
		}
		WoTTest::DestroySpecWorld(World);
	};
	RunMutation(100);
	RunMutation(140);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPersonalSkinSpec,
	"WoT.Rules.PersonalSkin.LoginGatesAndClassify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPersonalSkinSpec::RunTest(const FString&)
{
	// WOTPlayer.SetSkin's PRI.Color classifier (uc:2752-2757): substring, first match.
	TestEqual(TEXT("Black classifies 1"),
		AWoTCharacter::ClassifySkinColor(TEXT("Aes_Black")), uint8(1));
	TestEqual(TEXT("Blue classifies 2"),
		AWoTCharacter::ClassifySkinColor(TEXT("Aes_Blue")), uint8(2));
	TestEqual(TEXT("Red classifies 3"),
		AWoTCharacter::ClassifySkinColor(TEXT("For_Red")), uint8(3));
	TestEqual(TEXT("Green classifies 4"),
		AWoTCharacter::ClassifySkinColor(TEXT("Hou_Green")), uint8(4));
	TestEqual(TEXT("Base classifies 0"),
		AWoTCharacter::ClassifySkinColor(TEXT("Whi_Base")), uint8(0));
	TestEqual(TEXT("first substring match wins (retail if-else order)"),
		AWoTCharacter::ClassifySkinColor(TEXT("BlueBlack")), uint8(1));

	UWorld* World = PlayerClassTests_CreateWorld();
	AWoTGameMode* Mode = World->GetAuthGameMode<AWoTGameMode>();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerState* PS = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Mode || !Controller || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// GameInfo.Login: the ?Skin= option is parsed at login and carried on the
	// controller for every possession (UE1 applied it once to the persistent pawn).
	const FUniqueNetIdRepl InvalidUniqueId;
	Mode->InitNewPlayer(Controller, InvalidUniqueId,
		TEXT("?Class=WOTPawns.AesSedai?Skin=AesSedaiSkins.Aes_Blue"));
	TestEqual(TEXT("login stores the Skin option on the controller"),
		Controller->PersonalSkinURL, FString(TEXT("AesSedaiSkins.Aes_Blue")));

	FActorSpawnParameters Params = PlayerClassTests_AlwaysSpawnParams();
	AWoTAesSedai* Pawn = World->SpawnActor<AWoTAesSedai>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->Possess(Pawn);
	TestEqual(TEXT("possession applies the login skin (SetMultiSkin accepts)"),
		Pawn->PersonalSkin, FName(TEXT("Aes_Blue")));
	TestEqual(TEXT("acceptance classifies PRI.Color Blue"), PS->Color, uint8(2));
	const FString BlueSkinPath =
		TEXT("/Game/WoT/Textures/AesSedaiSkins/Aes_Blue.Aes_Blue");
	TestEqual(TEXT("the opaque body slot uses the B3 variant texture"),
		PlayerClassTests_SlotSkinPath(Pawn, TEXT("Aes_Base")), BlueSkinPath);
	TestEqual(TEXT("the two-sided body slot uses the same B3 variant texture"),
		PlayerClassTests_SlotSkinPath(Pawn, TEXT("Aes_Base_2S")), BlueSkinPath);

	// Left(SkinName, Len(MeshName)) ~= MeshName (uc:4216): a foreign faction's skin
	// package fails the prefix gate and keeps the CURRENT skin.
	Pawn->ServerSetPersonalSkin(TEXT("ForsakenSkins.For_Red"));
	TestEqual(TEXT("foreign-mesh skin is rejected (skin kept)"),
		Pawn->PersonalSkin, FName(TEXT("Aes_Blue")));
	TestEqual(TEXT("rejection leaves PRI.Color"), PS->Color, uint8(2));
	// DynamicLoadObject validates the complete package-qualified object
	// (WOTPlayer.uc:4212-4218), so a missing package must not borrow an existing
	// same-leaf MI from AesSedaiSkins.
	Pawn->ServerSetPersonalSkin(TEXT("AesSedaiSkinsBogus.Aes_Blue"));
	TestEqual(TEXT("missing package cannot alias a same-leaf skin MIC"),
		Pawn->PersonalSkin, FName(NAME_None));
	TestEqual(TEXT("same-leaf package miss resets Color 0"), PS->Color, uint8(0));

	// A prefix-passing name that fails to resolve is SetSkin(None): reset to the
	// class-default Base skin (uc:2738-2741), Color 0.
	Pawn->ServerSetPersonalSkin(TEXT("AesSedaiSkins.NotATexture"));
	TestEqual(TEXT("unresolvable skin resets to the class default"),
		Pawn->PersonalSkin, FName(NAME_None));
	TestEqual(TEXT("reset classifies Color 0"), PS->Color, uint8(0));
	TestEqual(TEXT("the opaque slot returns to the authored Base material"),
		PlayerClassTests_SlotMaterialName(Pawn, TEXT("Aes_Base")),
		FString(TEXT("MI_Aes_Base")));

	// GameInfo.bCanChangeSkin gates both the login fold and ServerChangeSkin
	// (Engine.PlayerPawn.ServerChangeSkin, WOTPlayer.uc:4216).
	Mode->bCanChangeSkin = false;
	Pawn->ServerSetPersonalSkin(TEXT("AesSedaiSkins.Aes_Red"));
	TestEqual(TEXT("bCanChangeSkin=false refuses the change"),
		Pawn->PersonalSkin, FName(NAME_None));
	Mode->bCanChangeSkin = true;

	// The menu path (PlayerPawn.ServerChangeSkin -> SetMultiSkin).
	Controller->ServerChangeSkin(TEXT("AesSedaiSkins.Aes_Green"), TEXT(""), 255);
	TestEqual(TEXT("ServerChangeSkin applies to the live pawn"),
		Pawn->PersonalSkin, FName(TEXT("Aes_Green")));
	TestEqual(TEXT("ServerChangeSkin classifies Green"), PS->Color, uint8(4));
	TestEqual(TEXT("the controller remembers the change for future spawns"),
		Controller->PersonalSkinURL, FString(TEXT("AesSedaiSkins.Aes_Green")));

	// ChangeClassModelStuff ends with SetSkin(new class default) (uc:4123): a faction
	// morph resets the personal axis.
	Pawn->SetAppearanceClass(AWoTForsaken::StaticClass());
	TestEqual(TEXT("a faction morph resets the personal skin"),
		Pawn->PersonalSkin, FName(NAME_None));
	TestEqual(TEXT("a faction morph resets PRI.Color"), PS->Color, uint8(0));
	TestTrue(TEXT("a faction morph clears the controller's stored selection"),
		Controller->PersonalSkinURL.IsEmpty());
	// The morphed pawn's mesh now gates against the NEW mesh name.
	Pawn->ServerSetPersonalSkin(TEXT("ForsakenSkins.For_Black"));
	TestEqual(TEXT("the morphed mesh accepts its own skin package"),
		Pawn->PersonalSkin, FName(TEXT("For_Black")));
	TestEqual(TEXT("morphed acceptance classifies Black"), PS->Color, uint8(1));

	// ChangeClassModelStuff's default-skin reset (WOTPlayer.uc:4109-4123) also
	// clears the UE5 controller-side persistence seam: a later matching-faction
	// possession must not resurrect the selection that predated the morph.
	AWoTAesSedai* RepossessedPawn = World->SpawnActor<AWoTAesSedai>(
		FVector(300, 0, 100), FRotator::ZeroRotator, Params);
	if (TestNotNull(TEXT("post-morph repossession pawn"), RepossessedPawn))
	{
		Controller->Possess(RepossessedPawn);
		TestEqual(TEXT("post-morph repossession stays on the class default"),
			RepossessedPawn->PersonalSkin, FName(NAME_None));
	}

	WoTTest::DestroySpecWorld(World);

	// giMPBattle.Login rebuilds Options with Name/Team/Password only before
	// Super.Login (giMPBattle.uc:403-415), so its entrants ignore login ?Skin=.
	UWorld* BattleWorld = PlayerClassTests_CreateWorld(true);
	AWoTBattleGameMode* BattleMode =
		BattleWorld->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTPlayerController* BattleController =
		BattleWorld->SpawnActor<AWoTPlayerController>();
	if (!BattleMode || !BattleController)
	{
		WoTTest::DestroySpecWorld(BattleWorld);
		return false;
	}
	BattleMode->InitNewPlayer(BattleController, InvalidUniqueId,
		TEXT("?Name=Rand?Skin=AesSedaiSkins.Aes_Blue"));
	TestTrue(TEXT("Battle login drops the Skin option"),
		BattleController->PersonalSkinURL.IsEmpty());
	WoTTest::DestroySpecWorld(BattleWorld);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerColorSpec,
	"WoT.Rules.PlayerColor.FactionResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPlayerColorSpec::RunTest(const FString&)
{
	// The class-fixed PlayerColor identity (WOTPlayer.uc:280 + per-class defaults),
	// resolved through the single WoTPlayerColorFX seam and translated to the plain
	// color names consumer rows are keyed by (PC_Gold -> Yellow, the retail
	// AngrealInvDart/ReflectSkinEffect switches).
	UWorld* World = PlayerClassTests_CreateWorld();
	FActorSpawnParameters Params = PlayerClassTests_AlwaysSpawnParams();
	const struct { UClass* Class; const TCHAR* Expected; } Cases[] = {
		{AWoTAesSedai::StaticClass(), TEXT("Blue")},
		{AWoTForsaken::StaticClass(), TEXT("Red")},
		{AWoTHound::StaticClass(), TEXT("Purple")},
		{AWoTWhitecloak::StaticClass(), TEXT("Yellow")},
	};
	for (const auto& Case : Cases)
	{
		AWoTPlayerController* Controller =
			World->SpawnActor<AWoTPlayerController>();
		AWoTCharacter* Pawn = Cast<AWoTCharacter>(World->SpawnActor(
			Case.Class, &FVector::ZeroVector, &FRotator::ZeroRotator, Params));
		if (!Controller || !Pawn)
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		Controller->Possess(Pawn);
		TestEqual(FString::Printf(TEXT("%s player resolves %s"),
				*Case.Class->GetName(), Case.Expected),
			WoTPlayerColorFX::Resolve(Pawn, TEXT("Green")).ToString(),
			FString(Case.Expected));
	}

	// A Battle morph moves the identity with the appearance class (uc:4112's copy).
	AWoTPlayerController* MorphController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTAesSedai* MorphPawn = World->SpawnActor<AWoTAesSedai>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	if (MorphController && MorphPawn)
	{
		MorphController->Possess(MorphPawn);
		MorphPawn->SetAppearanceClass(AWoTWhitecloak::StaticClass());
		TestEqual(TEXT("a morphed player resolves the appearance class color"),
			WoTPlayerColorFX::Resolve(MorphPawn, TEXT("Green")).ToString(),
			FString(TEXT("Yellow")));
	}

	// Non-players keep the consumer-authored fallback (retail's per-consumer
	// default cases for a non-WOTPlayer owner).
	AWoTCharacter* NPC = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, Params);
	if (NPC)
	{
		TestEqual(TEXT("a non-player keeps the consumer fallback"),
			WoTPlayerColorFX::Resolve(NPC, TEXT("Green")).ToString(),
			FString(TEXT("Green")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
