#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTRetailFamiliesRestoreScope.h"
#include "../AI/WoTNPCWeaponData.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "../WoTCreatureChunk.h"
#include "../WoTUE1LightRegistry.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../Payload/WoTRuntimeWorldResource.h"

namespace
{
constexpr EAutomationTestFlags WoTRetailBuilderB2Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTRetailBuildContext WoTRetailBuilderB2Context(
	const FWoTRuntimeClassDefaults& Defaults)
{
	return FWoTRetailBuildContext{
		Defaults,
		[](FStringView, FWoTRetailTextureFacts&) { return false; },
		[](FStringView, TArray<FWoTVertexMeshSequence>&) { return false; }};
}

struct FWoTRetailBuilderB2LightSpec
{
	const TCHAR* RowName;
	const TCHAR* ClassIdentity;
};

constexpr FWoTRetailBuilderB2LightSpec GWoTRetailBuilderB2LightSpecs[] = {
	{TEXT("AngrealFireballProjectile"), TEXT("Angreal.AngrealFireballProjectile")},
	{TEXT("AngrealDartProjectile"), TEXT("Angreal.AngrealDartProjectile")},
	{TEXT("AngrealDecayProjectile"), TEXT("Angreal.AngrealDecayProjectile")},
	{TEXT("AngrealEarthTremorProjectile"), TEXT("Angreal.AngrealEarthTremorProjectile")},
	{TEXT("AngrealExpWardProjectile"), TEXT("Angreal.AngrealExpWardProjectile")},
	{TEXT("AngrealIceProjectile"), TEXT("Angreal.AngrealIceProjectile")},
	{TEXT("AngrealSeekerProjectile"), TEXT("Angreal.AngrealSeekerProjectile")},
	{TEXT("AngrealSoulBarbProjectile"), TEXT("Angreal.AngrealSoulBarbProjectile")},
	{TEXT("AngrealWallOfAirProjectile"), TEXT("Angreal.AngrealWallOfAirProjectile")},
	{TEXT("Explosion"), TEXT("WOT.Explosion")},
	{TEXT("DartExplosion"), TEXT("Angreal.DartExplosion")},
	{TEXT("LSImpact"), TEXT("Angreal.LSImpact")},
	{TEXT("SoulBarbImpact"), TEXT("WOT.SoulBarbImpact")},
	{TEXT("BurningChunk"), TEXT("Angreal.BurningChunk")},
	{TEXT("BFExpSprayer"), TEXT("Angreal.BFExpSprayer")},
	{TEXT("LightningSkinEffect"), TEXT("Angreal.LightningSkinEffect")},
	{TEXT("ReflectSkinEffect"), TEXT("Angreal.ReflectSkinEffect")},
	{TEXT("ShieldParticleMesh"), TEXT("Angreal.ShieldParticleMesh")},
	{TEXT("TaintVisual"), TEXT("Angreal.TaintVisual")},
	{TEXT("TaintExpTorus"), TEXT("Angreal.TaintExpTorus")},
	{TEXT("WhirlwindLeech"), TEXT("Angreal.WhirlwindLeech")},
	{TEXT("AMAPearl"), TEXT("Angreal.AMAPearl")},
	{TEXT("PixieSprayer"), TEXT("Angreal.PixieSprayer")},
	{TEXT("DeflectSprayer"), TEXT("Angreal.DeflectSprayer")},
	{TEXT("BalefireCorona"), TEXT("Angreal.BalefireCorona")},
	{TEXT("ETSoundProxy"), TEXT("Angreal.ETSoundProxy")},
};

constexpr const TCHAR* GWoTRetailBuilderB2WeaponNames[] = {
	TEXT("WoTPawns.WarderSword"),
	TEXT("WoTPawns.ArcherSword"),
	TEXT("WoTPawns.MyrddraalSword"),
	TEXT("WoTPawns.MyrddraalBow"),
	TEXT("WoTPawns.TrollocStrongAxeProxy"),
	TEXT("WoTPawns.BATHalberd"),
	TEXT("WoTPawns.SoldierShield"),
	TEXT("WoTPawns.QuestionerShield"),
};

template <typename ValueType>
FWoTRuntimeWorldProperty WoTRetailBuilderB2Value(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, ValueType Value, uint32 ArrayIndex = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.ArrayIndex = ArrayIndex;
	Property.Value.SetNumUninitialized(sizeof(Value));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(Value));
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderB2Object(const TCHAR* Name,
	FName Identity, uint32 ArrayIndex = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Object;
	Property.ArrayIndex = ArrayIndex;
	Property.ObjectIdentity = Identity.ToString();
	return Property;
}

FWoTRuntimeWorldProperty WoTRetailBuilderB2String(const TCHAR* Name,
	const FString& Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::String;
	if (Value.IsEmpty())
	{
		Property.Value.Add(0);
		return Property;
	}
	FTCHARToUTF8 Utf8(*Value);
	uint64 Remaining = static_cast<uint64>(Utf8.Length()) + 1u;
	uint8 First = static_cast<uint8>(Remaining & 0x3fu);
	Remaining >>= 6;
	if (Remaining != 0) First |= 0x40u;
	Property.Value.Add(First);
	while (Remaining != 0)
	{
		uint8 Next = static_cast<uint8>(Remaining & 0x7fu);
		Remaining >>= 7;
		if (Remaining != 0) Next |= 0x80u;
		Property.Value.Add(Next);
	}
	Property.Value.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Property.Value.Add(0);
	return Property;
}

FWoTRuntimeClassDefaultRecord* WoTRetailBuilderB2FindRecord(
	TArray<FWoTRuntimeClassDefaultRecord>& Records, FStringView Identity)
{
	return Records.FindByPredicate([Identity](const FWoTRuntimeClassDefaultRecord& Record)
	{
		return FStringView(Record.Identity).Equals(
			Identity, ESearchCase::CaseSensitive);
	});
}

void WoTRetailBuilderB2AddRecord(
	TArray<FWoTRuntimeClassDefaultRecord>& Records, const TCHAR* Identity,
	const TCHAR* SuperclassIdentity = nullptr)
{
	FWoTRuntimeClassDefaultRecord Record;
	Record.Identity = Identity;
	Record.ExportIndex = static_cast<uint32>(Records.Num() + 1);
	if (SuperclassIdentity != nullptr)
		Record.SuperclassIdentity = SuperclassIdentity;
	Records.Add(MoveTemp(Record));
}

uint8 WoTRetailBuilderB2LightShape(EWoTUE1LightShape Shape)
{
	switch (Shape)
	{
	case EWoTUE1LightShape::None: return 0;
	case EWoTUE1LightShape::NonIncidence: return 13;
	case EWoTUE1LightShape::Shell: return 14;
	default: return 0;
	}
}

uint8 WoTRetailBuilderB2LightType(EWoTDynLightType Type)
{
	switch (Type)
	{
	case EWoTDynLightType::Steady: return 1;
	case EWoTDynLightType::Pulse: return 2;
	case EWoTDynLightType::Blink: return 3;
	case EWoTDynLightType::Flicker: return 4;
	case EWoTDynLightType::SubtlePulse: return 7;
	default: return 0;
	}
}

FWoTUE1LightSet WoTRetailBuilderB2FixtureLights()
{
	FWoTUE1LightSet Result;
	for (int32 Index = 0;
		Index < UE_ARRAY_COUNT(GWoTRetailBuilderB2LightSpecs); ++Index)
	{
		FWoTUE1LightRow Row;
		Row.Brightness = static_cast<uint8>(17 + Index);
		Row.Hue = static_cast<uint8>(Index * 7);
		Row.Saturation = static_cast<uint8>(200 - Index);
		Row.Radius = static_cast<float>((Index % 12) + 1);
		Row.Shape = Index % 3 == 0 ? EWoTUE1LightShape::NonIncidence
			: EWoTUE1LightShape::None;
		Row.Type = EWoTDynLightType::Steady;
		Row.Period = static_cast<uint8>(Index);
		Row.Phase = static_cast<uint8>(Index + 1);
		Result.Rows.Add(GWoTRetailBuilderB2LightSpecs[Index].RowName, Row);
	}
	return Result;
}

bool WoTRetailBuilderB2BuildLights(const FWoTUE1LightSet& Fixture,
	FWoTUE1LightSet& Out, FString& OutError)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	for (const FWoTRetailBuilderB2LightSpec& Spec : GWoTRetailBuilderB2LightSpecs)
	{
		WoTRetailBuilderB2AddRecord(Records, Spec.ClassIdentity);
		FWoTRuntimeClassDefaultRecord& Record = Records.Last();
		const FWoTUE1LightRow& Row = Fixture.Rows.FindChecked(Spec.RowName);
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightBrightness"), EWoTRuntimeWorldPropertyType::Byte,
			Row.Brightness));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightHue"), EWoTRuntimeWorldPropertyType::Byte, Row.Hue));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightSaturation"), EWoTRuntimeWorldPropertyType::Byte,
			Row.Saturation));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightRadius"), EWoTRuntimeWorldPropertyType::Byte,
			static_cast<uint8>(Row.Radius)));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightEffect"), EWoTRuntimeWorldPropertyType::Byte,
			WoTRetailBuilderB2LightShape(Row.Shape)));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightType"), EWoTRuntimeWorldPropertyType::Byte,
			WoTRetailBuilderB2LightType(Row.Type)));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightPeriod"), EWoTRuntimeWorldPropertyType::Byte, Row.Period));
		Record.Properties.Add(WoTRetailBuilderB2Value(
			TEXT("LightPhase"), EWoTRuntimeWorldPropertyType::Byte, Row.Phase));
	}
	FWoTRuntimeClassDefaults Defaults;
	return Defaults.LoadFixture(MoveTemp(Records), OutError)
		&& FWoTUE1LightRegistryBuilder::Build(
			WoTRetailBuilderB2Context(Defaults), Out, OutError);
}

FWoTNPCWeaponSet WoTRetailBuilderB2FixtureWeapons()
{
	FWoTNPCWeaponSet Result;
	for (int32 Index = 0;
		Index < UE_ARRAY_COUNT(GWoTRetailBuilderB2WeaponNames); ++Index)
	{
		const FName Name(GWoTRetailBuilderB2WeaponNames[Index]);
		FWoTNPCWeaponConfig Row;
		Row.WeaponType = Name;
		Row.MinMeleeRange = 10.0f + Index;
		Row.MaxMeleeRange = 20.0f + Index;
		Row.MeleeEffectiveness = 0.25f + Index * 0.01f;
		Row.MinProjectileRange = 30.0f + Index;
		Row.MaxProjectileRange = 40.0f + Index;
		Row.ProjectileEffectiveness = 0.5f + Index * 0.01f;
		Row.bRequireTargetVisibility = Index % 2 == 0;
		Row.MissOdds = Index * 0.02f;
		if (Index % 2 == 0) Row.TypeCapableOfPreparation = TEXT("Archer");
		Row.Mesh = FName(*FString::Printf(TEXT("Fixture.Mesh%d"), Index));
		Row.DrawScale = 1.0f + Index * 0.1f;
		Row.Health = 50 + Index;
		Row.DamageSkin0 = FName(*FString::Printf(TEXT("Fixture.Skin%d"), Index));
		Row.DestroyedSound = FName(*FString::Printf(TEXT("Fixture.Sound%d"), Index));
		Row.ScaleGlow = 0.75f;
		Result.Rows.Add(Name, MoveTemp(Row));
	}
	return Result;
}

void WoTRetailBuilderB2AddWeaponProperties(FWoTRuntimeClassDefaultRecord& Record,
	const FWoTNPCWeaponConfig& Row)
{
#define WOT_RETAIL_B2_WEAPON_FLOAT(Member, PropertyName) \
	Record.Properties.Add(WoTRetailBuilderB2Value(PropertyName, \
		EWoTRuntimeWorldPropertyType::Float, Row.Member))
	WOT_RETAIL_B2_WEAPON_FLOAT(MinMeleeRange, TEXT("MinMeleeRange"));
	WOT_RETAIL_B2_WEAPON_FLOAT(MaxMeleeRange, TEXT("MaxMeleeRange"));
	WOT_RETAIL_B2_WEAPON_FLOAT(MeleeEffectiveness, TEXT("MeleeEffectiveness"));
	WOT_RETAIL_B2_WEAPON_FLOAT(MinProjectileRange, TEXT("MinProjectileRange"));
	WOT_RETAIL_B2_WEAPON_FLOAT(MaxProjectileRange, TEXT("MaxProjectileRange"));
	WOT_RETAIL_B2_WEAPON_FLOAT(ProjectileEffectiveness, TEXT("ProjectileEffectiveness"));
	WOT_RETAIL_B2_WEAPON_FLOAT(MissOdds, TEXT("MissOdds"));
	WOT_RETAIL_B2_WEAPON_FLOAT(DrawScale, TEXT("DrawScale"));
	WOT_RETAIL_B2_WEAPON_FLOAT(ScaleGlow, TEXT("ScaleGlow"));
	WOT_RETAIL_B2_WEAPON_FLOAT(MPTossedLifeSpan, TEXT("MPTossedLifeSpan"));
#undef WOT_RETAIL_B2_WEAPON_FLOAT
	Record.Properties.Add(WoTRetailBuilderB2Value(TEXT("bRequireTargetVisibility"),
		EWoTRuntimeWorldPropertyType::Boolean,
		static_cast<uint8>(Row.bRequireTargetVisibility)));
	Record.Properties.Add(WoTRetailBuilderB2Value(TEXT("Health"),
		EWoTRuntimeWorldPropertyType::Integer, Row.Health));
	if (Row.bStyleMasked)
	{
		Record.Properties.Add(WoTRetailBuilderB2Value(TEXT("Style"),
			EWoTRuntimeWorldPropertyType::Byte, uint8(2)));
	}
	FName PreparationIdentity = NAME_None;
	if (!Row.TypeCapableOfPreparation.IsNone())
	{
		FString Identity = FString(TEXT("WoTPawns."))
			+ Row.TypeCapableOfPreparation.ToString();
		// Real binary object identities may carry an export locator. The decoded
		// gameplay class is the package/class leaf, never the locator suffix.
		if (Row.TypeCapableOfPreparation == TEXT("Archer"))
			Identity += TEXT("#export-00000435");
		else if (Row.TypeCapableOfPreparation == TEXT("Myrddraal"))
			Identity += TEXT("#export-00000226");
		PreparationIdentity = FName(*Identity);
	}
	Record.Properties.Add(WoTRetailBuilderB2Object(
		TEXT("TypeCapableOfPreparation"), PreparationIdentity));
	Record.Properties.Add(WoTRetailBuilderB2Object(TEXT("Mesh"), Row.Mesh));
	Record.Properties.Add(WoTRetailBuilderB2Object(TEXT("DamageSkin0"), Row.DamageSkin0));
	Record.Properties.Add(WoTRetailBuilderB2Object(TEXT("DamageSkin1"), Row.DamageSkin1));
	Record.Properties.Add(WoTRetailBuilderB2Object(TEXT("DamageSkin2"), Row.DamageSkin2));
	for (int32 Index = 0; Index < Row.SkinOverrides.Num(); ++Index)
	{
		Record.Properties.Add(WoTRetailBuilderB2Object(TEXT("MultiSkins"),
			Row.SkinOverrides[Index], static_cast<uint32>(Index)));
	}
	Record.Properties.Add(WoTRetailBuilderB2String(
		TEXT("DestroyedSoundString"), Row.DestroyedSound.ToString()));
	Record.Properties.Add(WoTRetailBuilderB2String(
		TEXT("TossedDecorationTypeString"), Row.TossedDecorationType.ToString()));
}

bool WoTRetailBuilderB2BuildWeapons(const FWoTNPCWeaponSet& Fixture,
	FWoTNPCWeaponSet& Out, FString& OutError)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WotWeapon"));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WotThrowableWeapon"),
		TEXT("WOT.WotWeapon"));
	for (const TCHAR* Name : GWoTRetailBuilderB2WeaponNames)
	{
		const bool bThrowable = FStringView(Name).Equals(TEXT("WoTPawns.BATHalberd"))
			|| FStringView(Name).Equals(TEXT("WoTPawns.QuestionerShield"));
		WoTRetailBuilderB2AddRecord(Records, Name,
			bThrowable ? TEXT("WOT.WotThrowableWeapon") : TEXT("WOT.WotWeapon"));
		WoTRetailBuilderB2AddWeaponProperties(
			Records.Last(), Fixture.Rows.FindChecked(FName(Name)));
	}
	FWoTRuntimeClassDefaults Defaults;
	return Defaults.LoadFixture(MoveTemp(Records), OutError)
		&& FWoTNPCWeaponDataBuilder::Build(
			WoTRetailBuilderB2Context(Defaults), Out, OutError);
}

FString WoTRetailBuilderB2RetailMesh(FName Name)
{
	return Name.IsNone() ? FString() : FString(TEXT("WOT.")) + Name.ToString();
}

FString WoTRetailBuilderB2RetailSkin(FName Name)
{
	return Name.IsNone()
		? FString() : FString(TEXT("WOT.Skins.")) + Name.ToString();
}

FWoTRuntimeWorldProperty WoTRetailBuilderB2BodyPart(
	uint32 ArrayIndex, const FWoTBodyPart& Part)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("BodyParts");
	Property.Type = EWoTRuntimeWorldPropertyType::Struct;
	Property.ArrayIndex = ArrayIndex;
	Property.StructName = TEXT("BodyPartT");
	Property.StructMembers.Add(WoTRetailBuilderB2String(
		TEXT("MeshName"), WoTRetailBuilderB2RetailMesh(Part.MeshName)));
	Property.StructMembers.Add(WoTRetailBuilderB2String(
		TEXT("SkinName"), WoTRetailBuilderB2RetailSkin(Part.SkinTexName)));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("SkinSlot"),
		EWoTRuntimeWorldPropertyType::Integer,
		Part.SkinTexName.IsNone() ? int32(0) : int32(1)));
	FString BounceSound = Part.BounceSound.ToString();
	BounceSound.ReplaceCharInline(TEXT('/'), TEXT('.'));
	Property.StructMembers.Add(WoTRetailBuilderB2String(
		TEXT("BounceSoundStr"), BounceSound));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("bNoBlood"),
		EWoTRuntimeWorldPropertyType::Boolean, static_cast<uint8>(Part.bNoBlood)));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("Odds"),
		EWoTRuntimeWorldPropertyType::Float, Part.Odds));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("bNoDamage"),
		EWoTRuntimeWorldPropertyType::Boolean, static_cast<uint8>(Part.bNoDamage)));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("bZeroYaw"),
		EWoTRuntimeWorldPropertyType::Boolean, static_cast<uint8>(Part.bZeroYaw)));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("bZeroRoll"),
		EWoTRuntimeWorldPropertyType::Boolean, static_cast<uint8>(Part.bZeroRoll)));
	Property.StructMembers.Add(WoTRetailBuilderB2Value(TEXT("bZeroPitch"),
		EWoTRuntimeWorldPropertyType::Boolean, static_cast<uint8>(Part.bZeroPitch)));
	return Property;
}

bool WoTRetailBuilderB2BuildSyntheticChunks(
	FWoTCreatureChunkSet& Out, FString& OutError)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records;
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WOTCreatureChunk"));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("GoreLevel"),
		EWoTRuntimeWorldPropertyType::Byte, static_cast<uint8>(3)));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("MinBounceSpeed"),
		EWoTRuntimeWorldPropertyType::Float, 44.0f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("DeleteChunkInitialDelaySecs"),
		EWoTRuntimeWorldPropertyType::Float, 12.0f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("DeleteChunkCheckAgainDelaySecs"),
		EWoTRuntimeWorldPropertyType::Float, 3.0f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("BaseSinkRatePerSecMin"),
		EWoTRuntimeWorldPropertyType::Float, 7.0f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("BaseSinkRatePerSecMax"),
		EWoTRuntimeWorldPropertyType::Float, 13.0f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("BaseInitForVelocity"),
		EWoTRuntimeWorldPropertyType::Float, 99.0f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("PainZoneDamageScale"),
		EWoTRuntimeWorldPropertyType::Float, 0.25f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("bShouldSink"),
		EWoTRuntimeWorldPropertyType::Boolean, true));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.Gibs"));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("MinBleedTime"),
		EWoTRuntimeWorldPropertyType::Float, 2.25f));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("MaxBleedTime"),
		EWoTRuntimeWorldPropertyType::Float, 4.75f));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WOTReplacementChunk"));
	Records.Last().Properties.Add(WoTRetailBuilderB2Value(TEXT("GoreLevel"),
		EWoTRuntimeWorldPropertyType::Byte, static_cast<uint8>(2)));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WOTCarcass"));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WOTCarcassHumanoid"),
		TEXT("WOT.WOTCarcass"));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WOT.WOTCarcassBlack"),
		TEXT("WOT.WOTCarcass"));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WoTPawns.WOTCarcassLegion"),
		TEXT("WOT.WOTCarcass"));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WoTPawns.WOTCarcassMyrddraal"),
		TEXT("WOT.WOTCarcassBlack"));
	WoTRetailBuilderB2AddRecord(Records, TEXT("WoTPawns.WOTCarcassMinion"),
		TEXT("WOT.WOTCarcassBlack"));

	FWoTBodyPart ParentPart;
	ParentPart.MeshName = TEXT("FixtureParentMesh");
	ParentPart.SkinTexName = TEXT("FixtureParentSkin");
	ParentPart.Odds = 0.25f;
	ParentPart.ZOffset = 12.5f;
	ParentPart.bNoDamage = true;
	ParentPart.bZeroPitch = true;
	FWoTRuntimeClassDefaultRecord* Parent = WoTRetailBuilderB2FindRecord(
		Records, TEXT("WOT.WOTCarcass"));
	Parent->Properties.Add(WoTRetailBuilderB2BodyPart(0, ParentPart));
	Parent->Properties.Add(WoTRetailBuilderB2Value(TEXT("ZOffsets"),
		EWoTRuntimeWorldPropertyType::Float, ParentPart.ZOffset, 0));

	FWoTBodyPart ChildPart = ParentPart;
	ChildPart.Odds = 0.75f;
	ChildPart.bZeroRoll = true;
	FWoTRuntimeWorldProperty ChildDelta =
		WoTRetailBuilderB2BodyPart(0, ChildPart);
	ChildDelta.StructMembers.RemoveAll([](const FWoTRuntimeWorldProperty& Member)
	{
		return Member.Name != TEXT("Odds") && Member.Name != TEXT("bZeroRoll");
	});
	WoTRetailBuilderB2FindRecord(Records,
		TEXT("WOT.WOTCarcassHumanoid"))->Properties.Add(MoveTemp(ChildDelta));

	FWoTRuntimeClassDefaults Defaults;
	return Defaults.LoadFixture(MoveTemp(Records), OutError)
		&& FWoTCreatureChunkBuilder::Build(
			WoTRetailBuilderB2Context(Defaults), Out, OutError);
}

bool WoTRetailBuilderB2ExactBytes(FAutomationTestBase& Test,
	const TCHAR* Label, const TArray<uint8>& Actual, const ANSICHAR* Expected)
{
	const int32 ExpectedLength = FCStringAnsi::Strlen(Expected);
	return Test.TestEqual(FString(Label) + TEXT(" byte count"),
		Actual.Num(), ExpectedLength)
		&& Test.TestTrue(FString(Label) + TEXT(" exact bytes"),
			FMemory::Memcmp(Actual.GetData(), Expected, ExpectedLength) == 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB2FixtureSemanticsSpec,
	"WoT.RetailBuilders.B2.FixtureSemantics", WoTRetailBuilderB2Flags)

bool FWoTRetailBuilderB2FixtureSemanticsSpec::RunTest(const FString&)
{
	FWoTUE1LightRegistryBuilder::Reset();
	FWoTNPCWeaponDataBuilder::Reset();
	FWoTCreatureChunkBuilder::Reset();
	FString Error;
	FWoTUE1LightSet Lights;
	FWoTNPCWeaponSet Weapons;
	FWoTCreatureChunkSet Chunks;
	if (!TestTrue(TEXT("lights build"), WoTRetailBuilderB2BuildLights(
		WoTRetailBuilderB2FixtureLights(), Lights, Error)))
	{
		AddError(Error); return false;
	}
	if (!TestTrue(TEXT("weapons build"), WoTRetailBuilderB2BuildWeapons(
		WoTRetailBuilderB2FixtureWeapons(), Weapons, Error)))
	{
		AddError(Error); return false;
	}
	if (!TestTrue(TEXT("synthetic chunks build"),
		WoTRetailBuilderB2BuildSyntheticChunks(Chunks, Error)))
	{
		AddError(Error); return false;
	}
	TestEqual(TEXT("light row count"), Lights.Rows.Num(), 26);
	TestEqual(TEXT("weapon row count"), Weapons.Rows.Num(), 8);
	TestEqual(TEXT("recipe count"), Chunks.Recipes.Num(), 5);
	int32 PartCount = 0;
	for (const TPair<FName, TArray<FWoTBodyPart>>& Pair : Chunks.Recipes)
		PartCount += Pair.Value.Num();
	TestEqual(TEXT("ordered body-part slot count"), PartCount, 62);

	const FWoTBodyPart& Humanoid =
		Chunks.Recipes.FindChecked(TEXT("WOT.WOTCarcassHumanoid"))[0];
	TestEqual(TEXT("child-authored outer member overrides"), Humanoid.Odds, 0.75f);
	TestTrue(TEXT("child-authored boolean overrides"), Humanoid.bZeroRoll);
	TestEqual(TEXT("omitted mesh member remains inherited"), Humanoid.MeshName,
		FName(TEXT("FixtureParentMesh")));
	TestTrue(TEXT("omitted boolean member remains inherited"),
		Humanoid.bNoDamage && Humanoid.bZeroPitch);
	TestEqual(TEXT("separate inherited Z-offset remains intact"),
		Humanoid.ZOffset, 12.5f);
	TestEqual(TEXT("decoded skin is applied only to its authored slot"),
		Humanoid.SkinSlot, 1);
	TestEqual(TEXT("decoded chunk cleanup initial delay follows fixture"),
		Chunks.Defaults.DeleteInitialDelay, 12.0f);
	TestEqual(TEXT("decoded chunk cleanup recheck follows fixture"),
		Chunks.Defaults.DeleteRecheckDelay, 3.0f);
	TestEqual(TEXT("decoded chunk sink minimum follows fixture"),
		Chunks.Defaults.SinkRateMin, 7.0f);
	TestEqual(TEXT("decoded chunk sink maximum follows fixture"),
		Chunks.Defaults.SinkRateMax, 13.0f);
	TestEqual(TEXT("decoded chunk pain scale follows fixture"),
		Chunks.Defaults.PainZoneDamageScale, 0.25f);
	TestEqual(TEXT("decoded gib trail minimum follows fixture"),
		Chunks.Defaults.MinBleedTime, 2.25f);
	TestEqual(TEXT("decoded gib trail maximum follows fixture"),
		Chunks.Defaults.MaxBleedTime, 4.75f);
	TestTrue(TEXT("decoded chunk sinking gate follows fixture"),
		Chunks.Defaults.bShouldSink);
	const FName* MinionRecipe = Chunks.RecipeClassByCarcassClass.Find(
		TEXT("WoTPawns.WOTCarcassMinion"));
	if (TestNotNull(TEXT("Minion has an exact derived route"), MinionRecipe))
	{
		TestEqual(TEXT("Minion resolves through WOTCarcassBlack ancestry"),
			*MinionRecipe, FName(TEXT("WOT.WOTCarcassBlack")));
	}
	FWoTCreatureChunkSet WrongRoute = Chunks;
	WrongRoute.RecipeClassByCarcassClass.FindChecked(
		TEXT("WoTPawns.WOTCarcassMinion")) = TEXT("WOT.WOTCarcass");
	TestFalse(TEXT("route-map differences fail exact equivalence"),
		FWoTCreatureChunkBuilder::AreEquivalent(Chunks, WrongRoute, Error));
	TestTrue(TEXT("route mismatch identifies the exact class"),
		Error.Contains(TEXT("WoTPawns.WOTCarcassMinion")));

	TestEqual(TEXT("Myrddraal bolt native binding"),
		Weapons.Rows.FindChecked(TEXT("WoTPawns.MyrddraalBow")).ProjectileClass.Get(),
		AWoTMyrddraalBolt::StaticClass());
	TestEqual(TEXT("Trolloc axe native binding"),
		Weapons.Rows.FindChecked(TEXT("WoTPawns.TrollocStrongAxeProxy")).ProjectileClass.Get(),
		AWoTTrollocAxeProjectile::StaticClass());
	TestEqual(TEXT("BAT halberd native binding"),
		Weapons.Rows.FindChecked(TEXT("WoTPawns.BATHalberd")).ProjectileClass.Get(),
		AWoTBATHalberdProjectile::StaticClass());
	TestEqual(TEXT("Questioner shield native binding"),
		Weapons.Rows.FindChecked(TEXT("WoTPawns.QuestionerShield")).ProjectileClass.Get(),
		AWoTQuestShieldProjectile::StaticClass());
	TestTrue(TEXT("throwable classification comes from retail ancestry"),
		Weapons.Rows.FindChecked(TEXT("WoTPawns.BATHalberd")).bThrowable
		&& Weapons.Rows.FindChecked(TEXT("WoTPawns.QuestionerShield")).bThrowable);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB2FailureSpec,
	"WoT.RetailBuilders.B2.MissingClassAndWrongType", WoTRetailBuilderB2Flags)

bool FWoTRetailBuilderB2FailureSpec::RunTest(const FString&)
{
	FString Error;
	TArray<FWoTRuntimeClassDefaultRecord> MissingLightRecords;
	for (int32 Index = 1; Index < UE_ARRAY_COUNT(GWoTRetailBuilderB2LightSpecs); ++Index)
	{
		WoTRetailBuilderB2AddRecord(
			MissingLightRecords, GWoTRetailBuilderB2LightSpecs[Index].ClassIdentity);
	}
	FWoTRuntimeClassDefaults MissingLightDefaults;
	TestTrue(TEXT("missing-light fixture loads"), MissingLightDefaults.LoadFixture(
		MoveTemp(MissingLightRecords), Error));
	FWoTUE1LightSet LightSentinel;
	LightSentinel.Rows.Add(TEXT("sentinel"), FWoTUE1LightRow());
	TestFalse(TEXT("missing required light class fails"),
		FWoTUE1LightRegistryBuilder::Build(
			WoTRetailBuilderB2Context(MissingLightDefaults),
			LightSentinel, Error));
	TestTrue(TEXT("failed light build is transactional"),
		LightSentinel.Rows.Contains(TEXT("sentinel")));

	const FWoTNPCWeaponSet FixtureWeapons = WoTRetailBuilderB2FixtureWeapons();
	TArray<FWoTRuntimeClassDefaultRecord> WeaponRecords;
	WoTRetailBuilderB2AddRecord(WeaponRecords, TEXT("WOT.WotWeapon"));
	for (const TCHAR* Name : GWoTRetailBuilderB2WeaponNames)
	{
		WoTRetailBuilderB2AddRecord(
			WeaponRecords, Name, TEXT("WOT.WotWeapon"));
		WoTRetailBuilderB2AddWeaponProperties(
			WeaponRecords.Last(), FixtureWeapons.Rows.FindChecked(FName(Name)));
	}
	FWoTRuntimeClassDefaultRecord* Warder = WoTRetailBuilderB2FindRecord(
		WeaponRecords, TEXT("WoTPawns.WarderSword"));
	FWoTRuntimeWorldProperty* WrongRange = Warder->Properties.FindByPredicate(
		[](const FWoTRuntimeWorldProperty& Property)
		{
			return Property.Name == TEXT("MaxMeleeRange");
		});
	WrongRange->Type = EWoTRuntimeWorldPropertyType::Integer;
	FWoTRuntimeClassDefaults WrongWeaponDefaults;
	TestTrue(TEXT("wrong-weapon fixture loads"), WrongWeaponDefaults.LoadFixture(
		MoveTemp(WeaponRecords), Error));
	FWoTNPCWeaponSet WrongWeapons;
	TestFalse(TEXT("wrong scalar type fails closed"),
		FWoTNPCWeaponDataBuilder::Build(
			WoTRetailBuilderB2Context(WrongWeaponDefaults), WrongWeapons, Error));
	TestTrue(TEXT("wrong scalar identifies its property"),
		Error.Contains(TEXT("MaxMeleeRange")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB2CanonicalSpec,
	"WoT.RetailBuilders.B2.CanonicalStability", WoTRetailBuilderB2Flags)

bool FWoTRetailBuilderB2CanonicalSpec::RunTest(const FString&)
{
	TestEqual(TEXT("light family name"),
		FString(FWoTUE1LightRegistryBuilder::FamilyName), TEXT("ue1-lights"));
	TestEqual(TEXT("weapon family name"),
		FString(FWoTNPCWeaponDataBuilder::FamilyName), TEXT("npc-weapons"));
	TestEqual(TEXT("chunk family name"),
		FString(FWoTCreatureChunkBuilder::FamilyName), TEXT("creature-chunks"));

	FWoTUE1LightSet LightLeft;
	FWoTUE1LightSet LightRight;
	LightLeft.Rows.Add(TEXT("Zulu"), {1, 2, 3, 4.0f});
	LightLeft.Rows.Add(TEXT("Alpha"), {5, 6, 7, 8.0f});
	LightRight.Rows.Add(TEXT("Alpha"), {5, 6, 7, 8.0f});
	LightRight.Rows.Add(TEXT("Zulu"), {1, 2, 3, 4.0f});
	const FString LightDigest =
		FWoTUE1LightRegistryBuilder::CanonicalDigest(LightLeft);
	TestEqual(TEXT("light map order is canonical"), LightDigest,
		FWoTUE1LightRegistryBuilder::CanonicalDigest(LightRight));
	TArray<uint8> LightBytes;
	FWoTUE1LightRegistryBuilder::SerializeCanonical(LightLeft, LightBytes);
	static constexpr ANSICHAR ExpectedLight[] =
		"wot-ue1-lights-v1\nrows 2\n"
		"row 5:alpha 5 6 7 0x41000000 0 0 0 0\n"
		"row 4:zulu 1 2 3 0x40800000 0 0 0 0\n";
	WoTRetailBuilderB2ExactBytes(*this, TEXT("light canonical"),
		LightBytes, ExpectedLight);
	TestEqual(TEXT("light canonical SHA-256"), LightDigest,
		FString(TEXT("94d8e4a64f083755edecb4f45f9d3d8825ed8041a233124d7f3eb11396d77e51")));

	FWoTNPCWeaponSet WeaponLeft;
	FWoTNPCWeaponSet WeaponRight;
	FWoTNPCWeaponConfig AlphaWeapon;
	AlphaWeapon.WeaponType = TEXT("Alpha");
	AlphaWeapon.MaxMeleeRange = 65.0f;
	FWoTNPCWeaponConfig ZuluWeapon = AlphaWeapon;
	ZuluWeapon.WeaponType = TEXT("Zulu");
	WeaponLeft.Rows.Add(TEXT("Zulu"), ZuluWeapon);
	WeaponLeft.Rows.Add(TEXT("Alpha"), AlphaWeapon);
	WeaponRight.Rows.Add(TEXT("Alpha"), AlphaWeapon);
	WeaponRight.Rows.Add(TEXT("Zulu"), ZuluWeapon);
	const FString WeaponDigest =
		FWoTNPCWeaponDataBuilder::CanonicalDigest(WeaponLeft);
	TestEqual(TEXT("weapon map order is canonical"), WeaponDigest,
		FWoTNPCWeaponDataBuilder::CanonicalDigest(WeaponRight));
	TArray<uint8> WeaponBytes;
	FWoTNPCWeaponDataBuilder::SerializeCanonical(WeaponLeft, WeaponBytes);
	static constexpr ANSICHAR ExpectedWeapon[] =
		"wot-npc-weapons-v1\nrows 2\n"
		"row 5:alpha 0x00000000 0x42820000 0x00000000 0x00000000 "
		"0x00000000 0x00000000 0: 1 0x00000000 4:none 4:none "
		"0x3f800000 0 0 0x3f800000 0 4:none 4:none 4:none 4:none "
		"4:none 0x42700000 0 0\n"
		"row 4:zulu 0x00000000 0x42820000 0x00000000 0x00000000 "
		"0x00000000 0x00000000 0: 1 0x00000000 4:none 4:none "
		"0x3f800000 0 0 0x3f800000 0 4:none 4:none 4:none 4:none "
		"4:none 0x42700000 0 0\n";
	WoTRetailBuilderB2ExactBytes(*this, TEXT("weapon canonical"),
		WeaponBytes, ExpectedWeapon);
	TestEqual(TEXT("weapon canonical SHA-256"), WeaponDigest,
		FString(TEXT("e8da8dc1980811e9db11d022e5fdb1f03c3e0e6a68b5a25882a8661d9c4ee9f1")));

	FWoTCreatureChunkSet ChunkLeft;
	FWoTCreatureChunkSet ChunkRight;
	FWoTBodyPart Part;
	Part.MeshName = TEXT("ChunkA");
	Part.Odds = 0.5f;
	ChunkLeft.Recipes.Add(TEXT("Zulu"), {Part});
	ChunkLeft.Recipes.Add(TEXT("Alpha"), {Part});
	ChunkLeft.RecipeClassByCarcassClass.Add(TEXT("ZLeaf"), TEXT("Zulu"));
	ChunkLeft.RecipeClassByCarcassClass.Add(TEXT("ALeaf"), TEXT("Alpha"));
	ChunkRight.Recipes.Add(TEXT("Alpha"), {Part});
	ChunkRight.Recipes.Add(TEXT("Zulu"), {Part});
	ChunkRight.RecipeClassByCarcassClass.Add(TEXT("ALeaf"), TEXT("Alpha"));
	ChunkRight.RecipeClassByCarcassClass.Add(TEXT("ZLeaf"), TEXT("Zulu"));
	const FString ChunkDigest =
		FWoTCreatureChunkBuilder::CanonicalDigest(ChunkLeft);
	TestEqual(TEXT("recipe and route map order is canonical"), ChunkDigest,
		FWoTCreatureChunkBuilder::CanonicalDigest(ChunkRight));
	TArray<uint8> ChunkBytes;
	FWoTCreatureChunkBuilder::SerializeCanonical(ChunkLeft, ChunkBytes);
	static constexpr ANSICHAR ExpectedChunk[] =
		"wot-creature-chunks-v2\ndefaults 0x00000000 0x00000000 0x00000000 "
		"0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 "
		"0x00000000 0\nrecipes 2\n"
		"recipe 5:alpha 1\n"
		"part 6:chunka 4:none 0 0x3f000000 0x00000000 0 0 4:none 0 0 0 0\n"
		"recipe 4:zulu 1\n"
		"part 6:chunka 4:none 0 0x3f000000 0x00000000 0 0 4:none 0 0 0 0\n"
		"routes 2\nroute 5:aleaf 5:alpha\nroute 5:zleaf 4:zulu\n";
	WoTRetailBuilderB2ExactBytes(*this, TEXT("chunk canonical"),
		ChunkBytes, ExpectedChunk);
	TestEqual(TEXT("chunk canonical SHA-256"), ChunkDigest,
		FString(TEXT("cc6f3927c09c3718bc44ee0ea5511cccbb50db9472798090a1e50c79ce9e1df1")));
	ChunkRight.Recipes.FindChecked(TEXT("Alpha"))[0].Odds = 0.75f;
	TestTrue(TEXT("a changed float cell changes the digest"), ChunkDigest
		!= FWoTCreatureChunkBuilder::CanonicalDigest(ChunkRight));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBuilderB2InstallResetSpec,
	"WoT.RetailBuilders.B2.InstallResetInjection", WoTRetailBuilderB2Flags)

bool FWoTRetailBuilderB2InstallResetSpec::RunTest(const FString&)
{
	FWoTRetailFamiliesRestoreScope RestoreFamilies;
	FWoTUE1LightRegistryBuilder::Reset();
	FWoTNPCWeaponDataBuilder::Reset();
	FWoTCreatureChunkBuilder::Reset();
	TestNull(TEXT("light is unavailable before install under final switch"),
		WoTUE1LightRegistry::Find(TEXT("AngrealFireballProjectile")));
	TestNull(TEXT("weapon is unavailable before install under final switch"),
		WoTNPCWeaponData::Find(TEXT("WoTPawns.WarderSword")));
	TestNull(TEXT("chunk recipe is unavailable before install under final switch"),
		WoTGoreData::FindRecipe(TEXT("WOT.WOTCarcass")));

	FWoTUE1LightSet Lights;
	FWoTUE1LightRow FixtureLight;
	FixtureLight.Brightness = 17;
	Lights.Rows.Add(TEXT("FixtureLight"), FixtureLight);
	FWoTUE1LightRegistryBuilder::Install(MoveTemp(Lights));
	TestNotNull(TEXT("installed light is visible"),
		WoTUE1LightRegistry::Find(TEXT("FixtureLight")));
	TestNull(TEXT("uninstalled light row remains hidden"),
		WoTUE1LightRegistry::Find(TEXT("AngrealFireballProjectile")));

	FWoTNPCWeaponSet Weapons;
	FWoTNPCWeaponConfig FixtureWeapon;
	FixtureWeapon.WeaponType = TEXT("FixtureWeapon");
	Weapons.Rows.Add(FixtureWeapon.WeaponType, FixtureWeapon);
	FWoTNPCWeaponDataBuilder::Install(MoveTemp(Weapons));
	TestNotNull(TEXT("installed weapon is visible"),
		WoTNPCWeaponData::Find(TEXT("FixtureWeapon")));

	FWoTCreatureChunkSet Chunks;
	Chunks.Recipes.Add(TEXT("FixtureRecipe"), {FWoTBodyPart()});
	Chunks.RecipeClassByCarcassClass.Add(
		TEXT("FixtureCarcass"), TEXT("FixtureRecipe"));
	FWoTCreatureChunkBuilder::Install(MoveTemp(Chunks));
	TestNotNull(TEXT("installed exact carcass route is visible"),
		WoTGoreData::FindRecipe(TEXT("FixtureCarcass")));
	TestNull(TEXT("installed lookup has no substring fallback"),
		WoTGoreData::FindRecipe(TEXT("SomeFixtureCarcassSuffix")));

	FWoTUE1LightRegistryBuilder::Reset();
	FWoTNPCWeaponDataBuilder::Reset();
	FWoTCreatureChunkBuilder::Reset();
	TestNull(TEXT("light Reset returns to unavailable"),
		WoTUE1LightRegistry::Find(TEXT("AngrealFireballProjectile")));
	TestNull(TEXT("weapon Reset returns to unavailable"),
		WoTNPCWeaponData::Find(TEXT("WoTPawns.WarderSword")));
	TestNull(TEXT("chunk Reset returns to unavailable"),
		WoTGoreData::FindRecipe(TEXT("WOT.WOTCarcass")));
	return !HasAnyErrors();
}

#endif
