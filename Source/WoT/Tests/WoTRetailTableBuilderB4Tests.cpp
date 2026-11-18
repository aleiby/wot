#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTRetailFamiliesRestoreScope.h"
#include "../RetailData/WoTRetailFamilies.h"
#include "../WoTDecal.h"
#include "../WoTMeshFX.h"
#include "../WoTWeaveFX.h"
#include "../World/WoTRuntimeClassDefaults.h"

namespace
{
constexpr EAutomationTestFlags WoTRetailBuilderB4Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FString B4Bytes(const TArray<uint8>& Bytes)
{
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return FString(Text.Length(), Text.Get());
}

template <typename T>
FWoTRuntimeWorldProperty B4Value(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, T Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.Value.SetNumUninitialized(sizeof(T));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(T));
	return Property;
}

FWoTRuntimeWorldProperty B4Name(const TCHAR* Name, const TCHAR* Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Name;
	Property.NameValue = Value;
	return Property;
}

FWoTRuntimeWorldProperty B4ParticleTemplate()
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("Templates");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.StructName = TEXT("Particle");
	Property.Value.SetNumZeroed(50);
	const float Values[] = {1.0f, 1.0f, 0.0f, 0.0f,
		1.0f, 1.0f, 1.0f, 1.0f};
	FMemory::Memcpy(Property.Value.GetData(), Values, sizeof(Values));
	// GrowPhase byte at 32; grow rates at 33/37; FadePhase at 41; fade rates at 42/46.
	return Property;
}

bool B4DecodeMyrddraal(float Duration, FWoTSprayerBundleRow& Out,
	FString& Error, FName InitialState = TEXT("TriggerTimed"),
	bool bInitiallyOn = false, bool bOn = true)
{
	FWoTRuntimeClassDefaultRecord Record;
	Record.Identity = TEXT("ParticleSystems.MyrddraalTeleportSprayer2");
	Record.Properties.Add(B4Value(TEXT("NumTemplates"),
		EWoTRuntimeWorldPropertyType::Byte, uint8(1)));
	Record.Properties.Add(B4ParticleTemplate());
	FWoTRuntimeWorldProperty Texture;
	Texture.Name = TEXT("Particles");
	Texture.Type = EWoTRuntimeWorldPropertyType::Object;
	Texture.ObjectIdentity = TEXT("ParticleSystems.Appear.PurpleSpark");
	Record.Properties.Add(MoveTemp(Texture));
	Record.Properties.Add(B4Name(TEXT("InitialState"),
		*InitialState.ToString()));
	Record.Properties.Add(B4Value(TEXT("TimerDuration"),
		EWoTRuntimeWorldPropertyType::Float, Duration));
	Record.Properties.Add(B4Value(TEXT("bInitiallyOn"),
		EWoTRuntimeWorldPropertyType::Boolean, uint8(bInitiallyOn ? 1 : 0)));
	Record.Properties.Add(B4Value(TEXT("bOn"),
		EWoTRuntimeWorldPropertyType::Boolean, uint8(bOn ? 1 : 0)));
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	Records.Add(MoveTemp(Record));
	FWoTRuntimeClassDefaults Defaults;
	if (!Defaults.LoadFixture(MoveTemp(Records), Error)) return false;
	FWoTRetailBuildContext Context{Defaults,
		[](FStringView, FWoTRetailTextureFacts& Facts)
		{
			Facts.USize = Facts.VSize = 16;
			return true;
		}, {}};
	return FWoTWeaveFXBuilder::DecodeBundleForTesting(Context,
		TEXT("MyrddraalTeleportSprayer2"),
		TEXT("ParticleSystems.MyrddraalTeleportSprayer2"), Out, Error);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB4CanonicalPinsSpec,
	"WoT.RetailBuilders.B4.CanonicalPins",
	WoTRetailBuilderB4Flags)

bool FWoTRetailBuilderB4CanonicalPinsSpec::RunTest(const FString&)
{
	TArray<uint8> Bytes;
	const FWoTWeaveFXSet Weave;
	FWoTWeaveFXBuilder::SerializeCanonical(Weave, Bytes);
	TestEqual(TEXT("weave canonical bytes"), B4Bytes(Bytes),
		FString(TEXT("wot-weave-fx-v2\nbundles 0\nexplosions 0\n")));
	TestEqual(TEXT("weave SHA-256"), FWoTWeaveFXBuilder::CanonicalDigest(Weave),
		FString(TEXT("f67af874e54850d1b68b779bbbf7f039b51c86c966740b4baf48d6427dca3cb2")));

	const FWoTMeshFXSet Mesh;
	FWoTMeshFXBuilder::SerializeCanonical(Mesh, Bytes);
	TestEqual(TEXT("mesh canonical bytes"), B4Bytes(Bytes),
		FString(TEXT("wot-mesh-fx-v2\nmeshes 0\noverlays 0\nsprites 0\nchains 0\n"
			"animations 0\nclass-textures 0\nlightning-streak 0\n")));
	TestEqual(TEXT("mesh SHA-256"), FWoTMeshFXBuilder::CanonicalDigest(Mesh),
		FString(TEXT("ca58a7984a1602bfa102b884f726fe2f35444396a8717ad3b6f74ab0bc3ad412")));

	const FWoTDecalSet Decal;
	FWoTDecalBuilder::SerializeCanonical(Decal, Bytes);
	TestEqual(TEXT("decal canonical bytes"), B4Bytes(Bytes),
		FString(TEXT("wot-decal-v2\nconfigs 0\n")));
	TestEqual(TEXT("decal SHA-256"), FWoTDecalBuilder::CanonicalDigest(Decal),
		FString(TEXT("697976f6d05c5e17715e7d89136aaa2393186c31de26a9327ae31ab3c6b9221b")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB4SprayerTriggerMutationSpec,
	"WoT.RetailBuilders.B4.SprayerTriggerMutation",
	WoTRetailBuilderB4Flags)

bool FWoTRetailBuilderB4SprayerTriggerMutationSpec::RunTest(const FString&)
{
	FWoTSprayerBundleRow First;
	FWoTSprayerBundleRow DurationMutation;
	FWoTSprayerBundleRow StateMutation;
	FWoTSprayerBundleRow InitiallyOnMutation;
	FWoTSprayerBundleRow OnMutation;
	FString Error;
	if (!TestTrue(TEXT("Myrddraal sprayer trigger defaults decode"),
		B4DecodeMyrddraal(4.0f, First, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("mutated sprayer trigger defaults decode"),
		B4DecodeMyrddraal(7.25f, DurationMutation, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("InitialState flows through the sprayer family"),
		First.InitialState, FName(TEXT("TriggerTimed")));
	TestFalse(TEXT("bInitiallyOn flows through the sprayer family"),
		First.bInitiallyOn);
	TestTrue(TEXT("bOn flows through the sprayer family"), First.bOn);
	TestEqual(TEXT("TimerDuration flows through the sprayer family"),
		First.TimerDuration, 4.0f);
	TestEqual(TEXT("mutating decoded duration changes the family row"),
		DurationMutation.TimerDuration, 7.25f);
	TestTrue(TEXT("mutated InitialState source decodes independently"),
		B4DecodeMyrddraal(4.0f, StateMutation, Error,
			TEXT("TriggerControl"), false, true));
	TestEqual(TEXT("InitialState mutation changes only the decoded trigger mode"),
		StateMutation.InitialState, FName(TEXT("TriggerControl")));
	TestEqual(TEXT("InitialState mutation retains duration"),
		StateMutation.TimerDuration, 4.0f);
	TestTrue(TEXT("mutated bInitiallyOn source decodes independently"),
		B4DecodeMyrddraal(4.0f, InitiallyOnMutation, Error,
			TEXT("TriggerTimed"), true, true));
	TestTrue(TEXT("bInitiallyOn mutation reaches the decoded row"),
		InitiallyOnMutation.bInitiallyOn);
	TestEqual(TEXT("bInitiallyOn mutation retains InitialState"),
		InitiallyOnMutation.InitialState, FName(TEXT("TriggerTimed")));
	TestTrue(TEXT("mutated bOn source decodes independently"),
		B4DecodeMyrddraal(4.0f, OnMutation, Error,
			TEXT("TriggerTimed"), false, false));
	TestFalse(TEXT("bOn mutation reaches the decoded row"), OnMutation.bOn);
	TestFalse(TEXT("bOn mutation retains bInitiallyOn"),
		OnMutation.bInitiallyOn);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB4InstallResetSpec,
	"WoT.RetailBuilders.B4.InstallResetInjection",
	WoTRetailBuilderB4Flags)

bool FWoTRetailBuilderB4InstallResetSpec::RunTest(const FString&)
{
	FWoTRetailFamiliesRestoreScope RestoreFamilies;
	FWoTWeaveFXBuilder::Reset(); FWoTMeshFXBuilder::Reset(); FWoTDecalBuilder::Reset();
	TestNull(TEXT("weave unavailable before install"),
		WoTWeaveFX::Find(TEXT("InjectedWeave")));
	TestNull(TEXT("mesh unavailable before install"),
		WoTMeshFX::Find(TEXT("InjectedMesh")));
	TestNull(TEXT("decal unavailable before install"),
		WoTDecalFX::Find(TEXT("InjectedDecal")));

	FWoTWeaveFXSet Weave;
	Weave.Bundles.Add(TEXT("InjectedWeave"), FWoTSprayerBundleRow{});
	FWoTWeaveFXBuilder::Install(MoveTemp(Weave));
	TestNotNull(TEXT("installed weave visible"), WoTWeaveFX::Find(TEXT("InjectedWeave")));
	TestNull(TEXT("uninstalled weave row remains hidden"),
		WoTWeaveFX::Find(TEXT("WaterSplash")));

	FWoTMeshFXSet Mesh;
	Mesh.MeshEffects.Add(TEXT("InjectedMesh"), FWoTMeshEffectRow{});
	Mesh.PawnOverlays.Add(TEXT("InjectedOverlay"), FWoTPawnOverlayRow{});
	Mesh.SpriteEffects.Add(TEXT("InjectedSprite"), FWoTSpriteEffectRow{});
	FWoTMeshFXBuilder::Install(MoveTemp(Mesh));
	TestNotNull(TEXT("installed mesh visible"), WoTMeshFX::Find(TEXT("InjectedMesh")));
	TestNotNull(TEXT("installed overlay visible"),
		WoTMeshFX::FindPawnOverlay(TEXT("InjectedOverlay")));
	TestNotNull(TEXT("installed sprite visible"),
		WoTMeshFX::FindSprite(TEXT("InjectedSprite")));

	FWoTDecalSet Decal;
	Decal.Configs.Add(TEXT("InjectedDecal"), FWoTDecalConfig{});
	FWoTDecalBuilder::Install(MoveTemp(Decal));
	TestNotNull(TEXT("installed decal visible"), WoTDecalFX::Find(TEXT("InjectedDecal")));

	FWoTWeaveFXBuilder::Reset(); FWoTMeshFXBuilder::Reset(); FWoTDecalBuilder::Reset();
	TestNull(TEXT("weave unavailable after reset"), WoTWeaveFX::Find(TEXT("WaterSplash")));
	TestNull(TEXT("mesh unavailable after reset"), WoTMeshFX::Find(TEXT("SoulBarbSkull")));
	TestNull(TEXT("decal unavailable after reset"), WoTDecalFX::Find(TEXT("BloodDecalRed")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB4FailuresSpec,
	"WoT.RetailBuilders.B4.MissingInputsAreTransactional",
	WoTRetailBuilderB4Flags)

bool FWoTRetailBuilderB4FailuresSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaults Empty;
	const FWoTRetailBuildContext Context{Empty, {}, {}};
	FString Error;
	FWoTWeaveFXSet Weave; Weave.Bundles.Add(TEXT("sentinel"), {});
	FWoTMeshFXSet Mesh; Mesh.MeshEffects.Add(TEXT("sentinel"), {});
	FWoTDecalSet Decal; Decal.Configs.Add(TEXT("sentinel"), {});
	TestFalse(TEXT("weave refuses unloaded defaults"),
		FWoTWeaveFXBuilder::Build(Context, Weave, Error));
	TestTrue(TEXT("weave output is transactional"), Weave.Bundles.Contains(TEXT("sentinel")));
	TestFalse(TEXT("mesh refuses unloaded defaults"),
		FWoTMeshFXBuilder::Build(Context, Mesh, Error));
	TestTrue(TEXT("mesh output is transactional"), Mesh.MeshEffects.Contains(TEXT("sentinel")));
	TestFalse(TEXT("decal refuses unloaded defaults"),
		FWoTDecalBuilder::Build(Context, Decal, Error));
	TestTrue(TEXT("decal output is transactional"), Decal.Configs.Contains(TEXT("sentinel")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB4FamilyListSpec,
	"WoT.RetailBuilders.B4.FamilyListEntries",
	WoTRetailBuilderB4Flags)

bool FWoTRetailBuilderB4FamilyListSpec::RunTest(const FString&)
{
	for (const TCHAR* Name : {FWoTWeaveFXBuilder::FamilyName,
		FWoTMeshFXBuilder::FamilyName, FWoTDecalBuilder::FamilyName})
	{
		int32 Count = 0;
		for (const FWoTRetailFamily& Family : WoTRetailFamilies())
			if (FStringView(Family.Name).Equals(Name, ESearchCase::CaseSensitive)) ++Count;
		TestEqual(*FString::Printf(TEXT("one %s family entry"), Name), Count, 1);
	}
	return !HasAnyErrors();
}

#endif
