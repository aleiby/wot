// Headless config-rot gate for the runtime actor screenshot gallery.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

namespace
{
// The gallery configs hold retail map coordinates, so they are private
// reference inputs: a public clone reaches them through WOT_REFERENCE_ROOT
// (the same rule as WoTReferenceRoot() in WoTRetailReader's ReferencePaths.h,
// restated here so this spec adds no include), and without one the specs skip.
FString WoTGalleryReferenceConfig(const TCHAR* Relative)
{
	const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_REFERENCE_ROOT"));
	return FPaths::ConvertRelativePathToFull(
		(Root.IsEmpty() ? FPaths::ProjectDir() : Root) / Relative);
}

bool WoTGalleryConfigMissing(FAutomationTestBase& Test, const TCHAR* Relative,
	const FString& Path)
{
	if (FPaths::FileExists(Path))
	{
		return false;
	}
	Test.AddWarning(FString::Printf(TEXT(
		"private reference input %s is absent; set WOT_REFERENCE_ROOT; spec skipped"),
		Relative));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGalleryConfigResolvesSpec,
	"WoT.Gallery.ConfigResolves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGalleryConfigResolvesSpec::RunTest(const FString&)
{
	const TCHAR* Relative = TEXT("Tools/test/gallery/gallery.json");
	const FString Path = WoTGalleryReferenceConfig(Relative);
	if (WoTGalleryConfigMissing(*this, Relative, Path))
	{
		return true;
	}
	FString Raw;
	if (!TestTrue(TEXT("gallery config can be read"),
		FFileHelper::LoadFileToString(Raw, *Path)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("gallery config parses as JSON"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) &&
		Root.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!TestTrue(TEXT("gallery config has entries"),
		Root->TryGetArrayField(TEXT("entries"), Entries) && Entries && !Entries->IsEmpty()))
	{
		return false;
	}

	TSet<FString> Names;
	TMap<FString, FString> PickupConfigs;
	TSet<FString> PickupNames;
	int32 ResolvedCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (!TestTrue(TEXT("each gallery entry is an object"), Entry.IsValid()))
		{
			continue;
		}
		FString Name;
		if (!TestTrue(TEXT("each gallery entry has a name"),
			Entry->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty()))
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("gallery entry name is unique: %s"), *Name),
			Names.Contains(Name));
		Names.Add(Name);
		FString Kind;
		Entry->TryGetStringField(TEXT("kind"), Kind);
		if (Kind.Equals(TEXT("pickup"), ESearchCase::IgnoreCase))
		{
			FString Config;
			TestTrue(*FString::Printf(TEXT("%s pickup has a retail class config"), *Name),
				Entry->TryGetStringField(TEXT("config"), Config) && !Config.IsEmpty());
			TestFalse(*FString::Printf(TEXT("pickup config is unique: %s"), *Config),
				PickupConfigs.Contains(Config));
			PickupNames.Add(Name);
			PickupConfigs.Add(Config, Name);
			double AmbientGlow = -1.0;
			if (Entry->TryGetNumberField(TEXT("ambient_glow"), AmbientGlow))
			{
				TestTrue(*FString::Printf(TEXT("%s ambient glow is a byte"), *Name),
					AmbientGlow >= 0.0 && AmbientGlow <= 255.0
					&& AmbientGlow == FMath::RoundToDouble(AmbientGlow));
			}
		}

		FString Skip;
		if (Entry->TryGetStringField(TEXT("skip"), Skip) && !Skip.IsEmpty())
		{
			continue;
		}
		FString ClassPath;
		if (!TestTrue(*FString::Printf(TEXT("%s has a class path"), *Name),
			Entry->TryGetStringField(TEXT("class"), ClassPath) && !ClassPath.IsEmpty()))
		{
			continue;
		}
		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
		if (!TestNotNull(*FString::Printf(TEXT("%s class resolves"), *Name), Class))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s is an AActor class"), *Name),
			Class->IsChildOf(AActor::StaticClass()));
		TestFalse(*FString::Printf(TEXT("%s is concrete and spawnable"), *Name),
			Class->HasAnyClassFlags(
				CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists));
		++ResolvedCount;
	}
	TestTrue(TEXT("gallery has non-skip entries"), ResolvedCount > 0);
	TestEqual(TEXT("gallery has exactly 34 pickup entries"), PickupNames.Num(), 34);
	TestEqual(TEXT("gallery has exactly 34 unique pickup configs"),
		PickupConfigs.Num(), 34);

	static const TCHAR* const RequiredPickupConfigs[] = {
		TEXT("Angreal.AndilayRoot"),
		TEXT("Angreal.AngrealInvAMA"),
		TEXT("Angreal.AngrealInvAbsorb"),
		TEXT("Angreal.AngrealInvAirShield"),
		TEXT("Angreal.AngrealInvBalefire"),
		TEXT("Angreal.AngrealInvChampion"),
		TEXT("Angreal.AngrealInvDart"),
		TEXT("Angreal.AngrealInvDecay"),
		TEXT("Angreal.AngrealInvDisguise"),
		TEXT("Angreal.AngrealInvEarthShield"),
		TEXT("Angreal.AngrealInvEarthTremor"),
		TEXT("Angreal.AngrealInvExpWard"),
		TEXT("Angreal.AngrealInvFireShield"),
		TEXT("Angreal.AngrealInvFireball"),
		TEXT("Angreal.AngrealInvFork"),
		TEXT("Angreal.AngrealInvGuardian"),
		TEXT("Angreal.AngrealInvHeal"),
		TEXT("Angreal.AngrealInvIce"),
		TEXT("Angreal.AngrealInvLightning"),
		TEXT("Angreal.AngrealInvMinion"),
		TEXT("Angreal.AngrealInvReflect"),
		TEXT("Angreal.AngrealInvRemoveCurse"),
		TEXT("Angreal.AngrealInvSeeker"),
		TEXT("Angreal.AngrealInvShield"),
		TEXT("Angreal.AngrealInvShift"),
		TEXT("Angreal.AngrealInvSoulBarb"),
		TEXT("Angreal.AngrealInvSpiritShield"),
		TEXT("Angreal.AngrealInvSwapPlaces"),
		TEXT("Angreal.AngrealInvTaint"),
		TEXT("Angreal.AngrealInvTarget"),
		TEXT("Angreal.AngrealInvTracer"),
		TEXT("Angreal.AngrealInvWaterShield"),
		TEXT("Angreal.AngrealInvWhirlwind"),
		TEXT("WOTPawns.LegionInvSeeker"),
	};
	for (const TCHAR* RequiredConfig : RequiredPickupConfigs)
	{
		TestTrue(*FString::Printf(TEXT("map-used pickup is in the gallery: %s"),
			RequiredConfig), PickupConfigs.Contains(RequiredConfig));
	}
	const FString* RootEntryName = PickupConfigs.Find(TEXT("Angreal.AndilayRoot"));
	if (TestNotNull(TEXT("Andilay root gallery entry exists"), RootEntryName))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			FString Name;
			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("name"), Name)
				|| Name != *RootEntryName) continue;
			double AmbientGlow = -1.0;
			TestTrue(TEXT("Andilay root gallery reproduces Arena_01 instance glow"),
				Entry->TryGetNumberField(TEXT("ambient_glow"), AmbientGlow)
				&& AmbientGlow == 64.0);
			bool bExisting = false;
			TestTrue(TEXT("Andilay root gallery observes the projected map actor"),
				Entry->TryGetBoolField(TEXT("existing"), bExisting) && bExisting);
			const TArray<TSharedPtr<FJsonValue>>* Anchor = nullptr;
			TestTrue(TEXT("Andilay root gallery pins the Arena_01 placement"),
				Entry->TryGetArrayField(TEXT("anchor"), Anchor) && Anchor
				&& Anchor->Num() == 3);
		}
	}
	const FString* LegionEntryName =
		PickupConfigs.Find(TEXT("WOTPawns.LegionInvSeeker"));
	if (TestNotNull(TEXT("Legion seeker gallery entry exists"), LegionEntryName))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			FString Name;
			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("name"), Name)
				|| Name != *LegionEntryName) continue;
			bool bExpectNoMesh = false;
			TestTrue(TEXT("Legion seeker records its retail no-mesh presentation"),
				Entry->TryGetBoolField(TEXT("expect_no_mesh"), bExpectNoMesh)
				&& bExpectNoMesh);
			TestFalse(TEXT("Legion seeker does not invent a development mesh"),
				Entry->HasField(TEXT("dev_mesh")));
		}
	}

	const TSharedPtr<FJsonObject>* Diff = nullptr;
	if (TestTrue(TEXT("gallery has a diff gate"),
		Root->TryGetObjectField(TEXT("diff"), Diff) && Diff && Diff->IsValid()))
	{
		double Crop = 0.0;
		double DefaultMad = -1.0;
		TestTrue(TEXT("center crop fraction is numeric"),
			(*Diff)->TryGetNumberField(TEXT("center_crop_fraction"), Crop));
		TestTrue(TEXT("center crop fraction is in (0,1]"), Crop > 0.0 && Crop <= 1.0);
		TestTrue(TEXT("default center MAD is numeric"),
			(*Diff)->TryGetNumberField(TEXT("default_max_center_mad"), DefaultMad));
		TestTrue(TEXT("default center MAD is non-negative"), DefaultMad >= 0.0);

		const TSharedPtr<FJsonObject>* Overrides = nullptr;
		if (TestTrue(TEXT("per-entry threshold map exists"),
			(*Diff)->TryGetObjectField(TEXT("entries"), Overrides)
			&& Overrides && Overrides->IsValid()))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Overrides)->Values)
			{
				TestTrue(*FString::Printf(TEXT("diff override names an entry: %s"), *Pair.Key),
					Names.Contains(Pair.Key));
				TestTrue(*FString::Printf(TEXT("diff override is non-negative: %s"), *Pair.Key),
					Pair.Value.IsValid() && Pair.Value->Type == EJson::Number
					&& Pair.Value->AsNumber() >= 0.0);
			}
			for (const FString& PickupName : PickupNames)
			{
				TestTrue(*FString::Printf(TEXT("pickup has an explicit diff threshold: %s"),
					*PickupName), (*Overrides)->HasField(PickupName));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Known = nullptr;
		if (TestTrue(TEXT("known-difference ledger exists"),
			(*Diff)->TryGetArrayField(TEXT("known_differences"), Known) && Known))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Known)
			{
				const TSharedPtr<FJsonObject> Row = Value->AsObject();
				FString Entry;
				FString Reason;
				TestTrue(TEXT("known difference is an object"), Row.IsValid());
				if (!Row.IsValid()) continue;
				TestTrue(TEXT("known difference names an existing entry"),
					Row->TryGetStringField(TEXT("entry"), Entry) && Names.Contains(Entry));
				TestTrue(TEXT("known difference has a reason"),
					Row->TryGetStringField(TEXT("reason"), Reason) && !Reason.IsEmpty());
			}
		}
	}
	return true;
}

// The sky-pose ledger is a fixed-camera gate for the 16 sky maps.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGallerySkyPoseConfigSpec,
	"WoT.Gallery.SkyPoseConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGallerySkyPoseConfigSpec::RunTest(const FString&)
{
	const TCHAR* Relative = TEXT("Tools/test/gallery/runtime-map-t5-2-sky.json");
	const FString Path = WoTGalleryReferenceConfig(Relative);
	if (WoTGalleryConfigMissing(*this, Relative, Path))
	{
		return true;
	}
	FString Raw;
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("sky pose config can be read"),
			FFileHelper::LoadFileToString(Raw, *Path))
		|| !TestTrue(TEXT("sky pose config parses as JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root)
			&& Root.IsValid()))
	{
		return false;
	}
	// The 19-map set minus the three maps without a SkyZoneInfo (A03/A09/A12).
	static const TCHAR* const SkyMaps[] = {
		TEXT("Arena_01"), TEXT("Arena_02"), TEXT("Arena_04"), TEXT("Arena_05"),
		TEXT("Arena_06"), TEXT("Arena_07"), TEXT("Arena_08"), TEXT("Arena_11"),
		TEXT("Battle2_01"), TEXT("Battle2_02"), TEXT("Battle2_03"),
		TEXT("Battle2_04"), TEXT("Battle2_05"), TEXT("Battle3_01"),
		TEXT("Battle3_02"), TEXT("Battle4_01")};
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!TestTrue(TEXT("sky pose config has entries"),
		Root->TryGetArrayField(TEXT("entries"), Entries) && Entries != nullptr))
	{
		return false;
	}
	TestEqual(TEXT("one pose per sky map"), Entries->Num(),
		static_cast<int32>(UE_ARRAY_COUNT(SkyMaps)));
	TSet<FString> Names;
	TSet<FString> Maps;
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!TestTrue(TEXT("sky pose entry is an object"), Entry.IsValid())) continue;
		FString Name;
		FString Kind;
		FString Map;
		TestTrue(TEXT("sky pose entry has a name"),
			Entry->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s is kind pose"), *Name),
			Entry->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("pose"));
		TestTrue(*FString::Printf(TEXT("%s names its map"), *Name),
			Entry->TryGetStringField(TEXT("map"), Map) && !Map.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s is unique"), *Name), Names.Contains(Name));
		TestFalse(*FString::Printf(TEXT("%s map is unique"), *Map), Maps.Contains(Map));
		TestEqual(*FString::Printf(TEXT("%s name follows Sky_<Map>"), *Name),
			Name, FString(TEXT("Sky_")) + Map);
		Names.Add(Name);
		Maps.Add(Map);
		for (const TCHAR* Field : {TEXT("camera"), TEXT("rotation")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Triple = nullptr;
			bool bFinite = Entry->TryGetArrayField(Field, Triple) && Triple != nullptr
				&& Triple->Num() == 3;
			if (bFinite)
			{
				for (const TSharedPtr<FJsonValue>& Component : *Triple)
				{
					bFinite = bFinite && Component.IsValid()
						&& Component->Type == EJson::Number
						&& FMath::IsFinite(Component->AsNumber());
				}
			}
			TestTrue(*FString::Printf(TEXT("%s %s is three finite numbers"),
				*Name, Field), bFinite);
		}
		double Estimate = -1.0;
		TestTrue(*FString::Printf(TEXT("%s records a positive sky estimate"), *Name),
			Entry->TryGetNumberField(TEXT("sky_fraction_estimate"), Estimate)
			&& Estimate > 0.0 && Estimate <= 1.0);
	}
	for (const TCHAR* SkyMap : SkyMaps)
	{
		TestTrue(*FString::Printf(TEXT("sky map has a pose: %s"), SkyMap),
			Maps.Contains(SkyMap));
	}
	const TSharedPtr<FJsonObject>* Diff = nullptr;
	if (TestTrue(TEXT("sky pose config has a diff gate"),
		Root->TryGetObjectField(TEXT("diff"), Diff) && Diff && Diff->IsValid()))
	{
		double Crop = 0.0;
		TestTrue(TEXT("sky gate compares the whole frame"),
			(*Diff)->TryGetNumberField(TEXT("center_crop_fraction"), Crop)
			&& Crop == 1.0);
		const TSharedPtr<FJsonObject>* Limits = nullptr;
		if (TestTrue(TEXT("sky gate has per-entry limits"),
			(*Diff)->TryGetObjectField(TEXT("entries"), Limits)
			&& Limits && Limits->IsValid()))
		{
			for (const FString& Name : Names)
			{
				double Limit = -1.0;
				TestTrue(*FString::Printf(TEXT("sky pose has an explicit limit: %s"), *Name),
					(*Limits)->TryGetNumberField(Name, Limit) && Limit >= 0.0);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Known = nullptr;
		if (TestTrue(TEXT("sky gate has a known-difference ledger"),
			(*Diff)->TryGetArrayField(TEXT("known_differences"), Known) && Known))
		{
			TSet<FString> KnownEntries;
			for (const TSharedPtr<FJsonValue>& Value : *Known)
			{
				const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
				FString KnownEntry;
				FString Reason;
				if (!TestTrue(TEXT("known sky difference is an object"), Row.IsValid()))
					continue;
				TestTrue(TEXT("known sky difference names a pose"),
					Row->TryGetStringField(TEXT("entry"), KnownEntry)
					&& Names.Contains(KnownEntry));
				TestTrue(TEXT("known sky difference has a reason"),
					Row->TryGetStringField(TEXT("reason"), Reason) && !Reason.IsEmpty());
				KnownEntries.Add(KnownEntry);
			}
			// The three sky-rest maps (A04, A07, B3_02) were once a residual;
			// the cache now carries the rest content (BSP v6) and draws it, so the ledger names only differences
			// that remain, each with its reason (checked above), and never the
			// closed residual.
			for (const TSharedPtr<FJsonValue>& Value : *Known)
			{
				const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Reason;
				if (Row.IsValid() && Row->TryGetStringField(TEXT("reason"), Reason))
				{
					TestFalse(TEXT("no known sky difference cites the closed sky-rest residual"),
						Reason.Contains(TEXT("sky-rest residual")));
				}
			}
		}
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
