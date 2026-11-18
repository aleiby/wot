// Atomic world-set specs pinned to the synthetic census and retail Arena_01.
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"

#if WITH_AUTOMATION_TESTS

bool FilterWorldShadowMaskForTesting(TConstArrayView<uint8> Bits,
	int32 MaskU, int32 VClamp, TArray<uint8>& Out);
bool ValidateEntryWeatherPairBudgetForTesting(
	TConstArrayView<uint64> PairKeys, TConstArrayView<uint32> PairByteCounts);
bool ResolveWorldLightDefaultsForTesting(FStringView LeafClass,
	uint8& OutLightType, uint8& OutEffect, uint8& OutBrightness,
	uint8& OutHue, uint8& OutSaturation, uint8& OutRadius,
	uint8& OutCone, bool& bOutSunLightRadius);
FString ClassifyWorldLightSkipForTesting(bool bActorPresent,
	bool bRecognizedClass, bool bLightningOff, uint8 LightType,
	uint8 Brightness, uint8 Effect, bool bMaskInRange);
double ProjectWorldLightLocationForTesting(double Value);
FWoTBootstrapStatus ValidateWorldAtlasCarrierForTesting(bool bAtlasComposed,
	FStringView MapIdentity, TConstArrayView<FString> PartIdentities);
bool EmptyWorldBSPEncodingFailsForComposedAtlasForTesting(bool bAtlasComposed);
bool WorldBSPSectionIdentityLengthsValidForTesting(
	TConstArrayView<int32> Lengths);
bool ComposeSyntheticMoverLightmapForTesting(
	uint8& OutFilteredShadow, uint8& OutIllumination, FIntVector& OutRGB7);
bool ComposeSyntheticMoverAnimationResponseForTesting(uint8 LightType,
	uint8& OutMeanRGB7, uint8& OutResponseRGB8, bool& bOutAnimated);
bool ComposeSyntheticMixedMoverResponseForTesting(
	uint8& OutAnimatedOnlyRGB8, uint8& OutWorldMixedRGB8,
	uint8& OutMoverMixedRGB8);
bool BuildLargeMoverAtlasForTesting(FIntPoint& OutSize);

namespace
{
constexpr EAutomationTestFlags WoTBSPWorldSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

FString WoTBSPWorldSpecRepoPath(const TCHAR* Relative)
{
	return WoTRepositoryOrReferencePath(Relative);
}

bool WoTBSPWorldSpecMapNames(TArray<FString>& OutNames)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *WoTBSPWorldSpecRepoPath(
		TEXT("Tools/dist/fixtures/map-set-v3-synthetic.json")))) return false;
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 4 * 1024 * 1024;
	Limits.MaxMembers = 1000000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Bytes, Limits);
	if (Parsed.IsError()) return false;
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	const FWoTCanonicalJsonValue* Maps = Root.FindMember(TEXT("maps"));
	if (Maps == nullptr || !Maps->IsArray()) return false;
	for (int32 Index = 0; Index < Maps->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Map = Maps->GetElement(Index);
		const FWoTCanonicalJsonValue* Identity = Map.FindMember(TEXT("identity"));
		if (Identity == nullptr
			|| Identity->GetKind() != EWoTCanonicalJsonKind::String) return false;
		OutNames.Add(Identity->GetString());
	}
	return OutNames.Num() == 19;
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTBSPWorldSpecFixture()
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *WoTBSPWorldSpecRepoPath(
		TEXT("Tools/dist/fixtures/map-set-v3-synthetic.json"))))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the synthetic map-set fixture is missing")));
	}
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 4 * 1024 * 1024;
	Limits.MaxMembers = 1000000;
	return ParseCanonicalJson(Bytes, Limits);
}

bool WoTBSPWorldSpecFixtureBytes(TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *WoTBSPWorldSpecRepoPath(
		TEXT("Tools/dist/fixtures/map-set-v3-synthetic.json")));
}

bool WoTBSPWorldSpecReplaceOnce(TArray<uint8>& Bytes,
	const ANSICHAR* Before, const ANSICHAR* After)
{
	const int32 BeforeLength = FCStringAnsi::Strlen(Before);
	const int32 AfterLength = FCStringAnsi::Strlen(After);
	for (int32 Offset = 0; Offset + BeforeLength <= Bytes.Num(); ++Offset)
	{
		if (FMemory::Memcmp(Bytes.GetData() + Offset, Before, BeforeLength) != 0)
			continue;
		TArray<uint8> Result;
		Result.Reserve(Bytes.Num() - BeforeLength + AfterLength);
		Result.Append(Bytes.GetData(), Offset);
		Result.Append(reinterpret_cast<const uint8*>(After), AfterLength);
		Result.Append(Bytes.GetData() + Offset + BeforeLength,
			Bytes.Num() - Offset - BeforeLength);
		Bytes = MoveTemp(Result);
		return true;
	}
	return false;
}

TOptional<FWoTBootstrapError> WoTBSPWorldSpecParityFixtureSchema(
	const FWoTCanonicalJsonValue& Root)
{
	auto Error = [](const FString& Message, const FString& Path)
	{
		return TOptional<FWoTBootstrapError>(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation, Message, Path));
	};
	auto Exact = [&Error](const FWoTCanonicalJsonValue& Value,
		TConstArrayView<const TCHAR*> Keys, const FString& Path)
		-> TOptional<FWoTBootstrapError>
	{
		if (TOptional<FWoTBootstrapError> Shape =
			RequireExactMembers(Value, Keys, *Path); Shape.IsSet())
		{
			return Error(TEXT("the BSP parity fixture has an invalid closed schema"),
				Path);
		}
		return {};
	};
	auto Integer = [&Error](const FWoTCanonicalJsonValue& Object,
		const TCHAR* Name, const FString& Path, int64 Minimum, int64 Maximum)
		-> TOptional<FWoTBootstrapError>
	{
		const FWoTCanonicalJsonValue* Value = Object.FindMember(Name);
		if (Value == nullptr
			|| Value->GetKind() != EWoTCanonicalJsonKind::Integer
			|| Value->GetInteger() < Minimum || Value->GetInteger() > Maximum)
		{
			return Error(FString::Printf(
				TEXT("the BSP parity fixture field '%s' is not a bounded integer"),
				Name), Path + TEXT(".") + Name);
		}
		return {};
	};
	auto String = [&Error](const FWoTCanonicalJsonValue& Object,
		const TCHAR* Name, const FString& Path, bool bMayBeEmpty)
		-> TOptional<FWoTBootstrapError>
	{
		const FWoTCanonicalJsonValue* Value = Object.FindMember(Name);
		if (Value == nullptr || Value->GetKind() != EWoTCanonicalJsonKind::String
			|| (!bMayBeEmpty && Value->GetString().IsEmpty()))
		{
			return Error(FString::Printf(
				TEXT("the BSP parity fixture field '%s' is not a valid string"), Name),
				Path + TEXT(".") + Name);
		}
		return {};
	};
	auto BitArray = [&Error](const FWoTCanonicalJsonValue* Value,
		int32 Count, const FString& Path) -> TOptional<FWoTBootstrapError>
	{
		if (Value == nullptr || !Value->IsArray() || Value->Num() != Count)
			return Error(TEXT("the BSP parity fixture sample has the wrong length"), Path);
		for (int32 Index = 0; Index < Value->Num(); ++Index)
		{
			const FWoTCanonicalJsonValue& Element = Value->GetElement(Index);
			if (Element.GetKind() != EWoTCanonicalJsonKind::Integer
				|| Element.GetInteger() < 0
				|| Element.GetInteger() > static_cast<int64>(MAX_uint32))
			{
				return Error(TEXT("the BSP parity fixture sample is not uint32"),
					FString::Printf(TEXT("%s[%d]"), *Path, Index));
			}
		}
		return {};
	};
	const TCHAR* RootKeys[] = {TEXT("format"), TEXT("maps")};
	if (TOptional<FWoTBootstrapError> Failure =
		Exact(Root, MakeArrayView(RootKeys), TEXT("$")); Failure.IsSet()) return Failure;
	if (TOptional<FWoTBootstrapError> Failure =
		String(Root, TEXT("format"), TEXT("$"), false); Failure.IsSet()) return Failure;
	if (Root.FindMember(TEXT("format"))->GetString()
		!= TEXT("wot-native-bsp-parity-v3"))
		return Error(TEXT("the BSP parity fixture format is unsupported"), TEXT("$.format"));
	const FWoTCanonicalJsonValue* Maps = Root.FindMember(TEXT("maps"));
	if (Maps == nullptr || !Maps->IsArray() || Maps->Num() != 19)
		return Error(TEXT("the BSP parity fixture must contain nineteen maps"),
			TEXT("$.maps"));
	for (int32 MapIndex = 0; MapIndex < Maps->Num(); ++MapIndex)
	{
		const FString MapPath = FString::Printf(TEXT("$.maps[%d]"), MapIndex);
		const FWoTCanonicalJsonValue& Map = Maps->GetElement(MapIndex);
		const TCHAR* MapKeys[] = {TEXT("identity"), TEXT("parts")};
		if (TOptional<FWoTBootstrapError> Failure =
			Exact(Map, MakeArrayView(MapKeys), MapPath); Failure.IsSet()) return Failure;
		if (TOptional<FWoTBootstrapError> Failure =
			String(Map, TEXT("identity"), MapPath, false); Failure.IsSet()) return Failure;
		const FWoTCanonicalJsonValue* Parts = Map.FindMember(TEXT("parts"));
		if (Parts == nullptr || !Parts->IsArray() || Parts->Num() == 0)
			return Error(TEXT("the BSP parity map has no part array"), MapPath + TEXT(".parts"));
		for (int32 PartIndex = 0; PartIndex < Parts->Num(); ++PartIndex)
		{
			const FString PartPath = FString::Printf(TEXT("%s.parts[%d]"),
				*MapPath, PartIndex);
			const FWoTCanonicalJsonValue& Part = Parts->GetElement(PartIndex);
			const TCHAR* PartKeys[] = {TEXT("index_count"), TEXT("part_identity"),
				TEXT("payload_block_sha256"), TEXT("payload_byte_length"),
				TEXT("payload_samples"), TEXT("procedural_material_slot"),
				TEXT("role"), TEXT("section_count"), TEXT("tangent_algorithm"),
				TEXT("triangle_count"), TEXT("vertex_count")};
			if (TOptional<FWoTBootstrapError> Failure =
				Exact(Part, MakeArrayView(PartKeys), PartPath); Failure.IsSet()) return Failure;
			for (const TCHAR* Name : {TEXT("part_identity"), TEXT("role"),
				TEXT("tangent_algorithm")})
				if (TOptional<FWoTBootstrapError> Failure =
					String(Part, Name, PartPath, false); Failure.IsSet()) return Failure;
			if (TOptional<FWoTBootstrapError> Failure = String(Part,
				TEXT("procedural_material_slot"), PartPath, true); Failure.IsSet()) return Failure;
			for (const TCHAR* Name : {TEXT("index_count"), TEXT("payload_byte_length"),
				TEXT("section_count"), TEXT("triangle_count"), TEXT("vertex_count")})
				if (TOptional<FWoTBootstrapError> Failure =
					Integer(Part, Name, PartPath, 1, MAX_int32); Failure.IsSet()) return Failure;

			const FWoTCanonicalJsonValue* Hashes =
				Part.FindMember(TEXT("payload_block_sha256"));
			const TCHAR* HashKeys[] = {TEXT("indices"), TEXT("section_table")};
			if (TOptional<FWoTBootstrapError> Failure = Exact(*Hashes,
				MakeArrayView(HashKeys), PartPath + TEXT(".payload_block_sha256"));
				Failure.IsSet()) return Failure;
			for (const TCHAR* Name : HashKeys)
			{
				const FWoTCanonicalJsonValue* Digest = Hashes->FindMember(Name);
				if (Digest == nullptr
					|| Digest->GetKind() != EWoTCanonicalJsonKind::String
					|| Digest->GetString().Len() != 64)
					return Error(TEXT("the BSP parity fixture hash is malformed"),
						PartPath + TEXT(".payload_block_sha256.") + Name);
				for (int32 CharacterIndex = 0;
					CharacterIndex < Digest->GetString().Len(); ++CharacterIndex)
				{
					const TCHAR Character = Digest->GetString()[CharacterIndex];
					if (!((Character >= TEXT('0') && Character <= TEXT('9'))
						|| (Character >= TEXT('a') && Character <= TEXT('f'))))
						return Error(TEXT("the BSP parity fixture hash is not lowercase hex"),
							PartPath + TEXT(".payload_block_sha256.") + Name);
				}
			}

			const FWoTCanonicalJsonValue* Samples =
				Part.FindMember(TEXT("payload_samples"));
			const TCHAR* SampleKeys[] = {TEXT("bounds"), TEXT("indices"),
				TEXT("normals"), TEXT("positions"), TEXT("sections"),
				TEXT("uv0"), TEXT("uv1")};
			if (TOptional<FWoTBootstrapError> Failure = Exact(*Samples,
				MakeArrayView(SampleKeys), PartPath + TEXT(".payload_samples"));
				Failure.IsSet()) return Failure;
			if (TOptional<FWoTBootstrapError> Failure = BitArray(
				Samples->FindMember(TEXT("bounds")), 6,
				PartPath + TEXT(".payload_samples.bounds")); Failure.IsSet()) return Failure;
			const int32 VertexCount = static_cast<int32>(
				Part.FindMember(TEXT("vertex_count"))->GetInteger());
			const int32 IndexCount = static_cast<int32>(
				Part.FindMember(TEXT("index_count"))->GetInteger());
			struct FSampleStream
			{
				const TCHAR* Name;
				int32 Width;
			};
			const FSampleStream Streams[] = {
				{TEXT("positions"), 3}, {TEXT("normals"), 3},
				{TEXT("uv0"), 2}, {TEXT("uv1"), 2}, {TEXT("indices"), 0}};
			for (const FSampleStream& Stream : Streams)
			{
				const FWoTCanonicalJsonValue* Edges = Samples->FindMember(Stream.Name);
				const TCHAR* EdgeKeys[] = {TEXT("end"), TEXT("head")};
				const FString StreamPath = PartPath + TEXT(".payload_samples.") + Stream.Name;
				if (TOptional<FWoTBootstrapError> Failure = Exact(*Edges,
					MakeArrayView(EdgeKeys), StreamPath); Failure.IsSet()) return Failure;
				const int32 Count = Stream.Width == 0
					? FMath::Min(IndexCount, 6)
					: FMath::Min(VertexCount * Stream.Width, Stream.Width * 3);
				for (const TCHAR* Edge : EdgeKeys)
					if (TOptional<FWoTBootstrapError> Failure = BitArray(
						Edges->FindMember(Edge), Count, StreamPath + TEXT(".") + Edge);
						Failure.IsSet()) return Failure;
			}
			const FWoTCanonicalJsonValue* Sections = Samples->FindMember(TEXT("sections"));
			if (Sections == nullptr || !Sections->IsArray()
				|| Sections->Num() != Part.FindMember(TEXT("section_count"))->GetInteger())
				return Error(TEXT("the BSP parity fixture section sample count is invalid"),
					PartPath + TEXT(".payload_samples.sections"));
			for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
			{
				const FString SectionPath = FString::Printf(TEXT("%s.payload_samples.sections[%d]"),
					*PartPath, SectionIndex);
				const FWoTCanonicalJsonValue& Section = Sections->GetElement(SectionIndex);
				const TCHAR* SectionKeys[] = {TEXT("canonical_name"),
					TEXT("canonical_package"), TEXT("collision_enabled"),
					TEXT("first_index"), TEXT("index_count"), TEXT("material_slot"),
					TEXT("max_vertex"), TEXT("min_vertex"),
					TEXT("pan_zone_actor_identity"), TEXT("source_texture_identity"),
					TEXT("surface_flags"), TEXT("triangle_count")};
				if (TOptional<FWoTBootstrapError> Failure = Exact(Section,
					MakeArrayView(SectionKeys), SectionPath); Failure.IsSet()) return Failure;
				if (Section.FindMember(TEXT("collision_enabled"))->GetKind()
					!= EWoTCanonicalJsonKind::Bool)
					return Error(TEXT("the BSP parity collision sample is not a bool"),
						SectionPath + TEXT(".collision_enabled"));
				for (const TCHAR* Name : {TEXT("canonical_name"),
					TEXT("material_slot"), TEXT("source_texture_identity")})
					if (TOptional<FWoTBootstrapError> Failure = String(Section,
						Name, SectionPath, false); Failure.IsSet()) return Failure;
				for (const TCHAR* Name : {TEXT("canonical_package"),
					TEXT("pan_zone_actor_identity")})
					if (TOptional<FWoTBootstrapError> Failure = String(Section,
						Name, SectionPath, true); Failure.IsSet()) return Failure;
				for (const TCHAR* Name : {TEXT("first_index"), TEXT("index_count"),
					TEXT("max_vertex"), TEXT("min_vertex"), TEXT("triangle_count")})
					if (TOptional<FWoTBootstrapError> Failure =
						Integer(Section, Name, SectionPath, 0, MAX_int32);
						Failure.IsSet()) return Failure;
				if (TOptional<FWoTBootstrapError> Failure = Integer(Section,
					TEXT("surface_flags"), SectionPath, 0, MAX_uint32);
					Failure.IsSet()) return Failure;
			}
		}
	}
	return {};
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTBSPWorldSpecParseParityFixture(
	TConstArrayView<uint8> Bytes)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 1024 * 1024;
	Limits.MaxMembers = 1000000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(Bytes, Limits);
	if (Parsed.IsError())
	{
		FWoTBootstrapError ParseError = MoveTemp(Parsed).GetError();
		return FResult::Error(MakeWoTBootstrapError(ParseError.Code,
			FString(TEXT("the BSP parity fixture is malformed: "))
				+ ParseError.Message, MoveTemp(ParseError.Path)));
	}
	if (TOptional<FWoTBootstrapError> Failure =
		WoTBSPWorldSpecParityFixtureSchema(Parsed.GetValue()); Failure.IsSet())
		return FResult::Error(MoveTemp(Failure.GetValue()));
	return Parsed;
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTBSPWorldSpecParityFixture()
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *WoTBSPWorldSpecRepoPath(
		TEXT("Tools/dist/fixtures/bsp-native-parity-v1.json"))))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the BSP parity fixture is missing")));
	}
	return WoTBSPWorldSpecParseParityFixture(Bytes);
}

void WoTBSPWorldSpecAddOutput(FWoTRequiredFact& Fact,
	const FString& LogicalId, const TCHAR* Schema)
{
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = LogicalId;
	Output.Schema = Schema;
	Output.MaxDecodedSize = 1024ull * 1024 * 1024;
}

TWoTBootstrapResult<FWoTRetailContract> WoTBSPWorldSpecContract(
	const TArray<FString>& Names)
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	FWoTRetailContract Contract;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parity =
		WoTBSPWorldSpecParityFixture();
	if (Parity.IsError())
		return FResult::Error(MoveTemp(Parity).GetError());
	const FWoTCanonicalJsonValue* ParityMaps =
		Parity.GetValue().FindMember(TEXT("maps"));
	if (ParityMaps == nullptr || !ParityMaps->IsArray()
		|| ParityMaps->Num() != Names.Num())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the BSP parity fixture does not contain the requested map set"),
			TEXT("$.maps")));
	}
	for (int32 MapIndex = 0; MapIndex < Names.Num(); ++MapIndex)
	{
		const FString& Name = Names[MapIndex];
		const FWoTCanonicalJsonValue& ParityMap =
			ParityMaps->GetElement(MapIndex);
		if (!ParityMap.IsObject())
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP parity map entry is not an object"),
				FString::Printf(TEXT("$.maps[%d]"), MapIndex)));
		}
		const FWoTCanonicalJsonValue* Parts =
			ParityMap.FindMember(TEXT("parts"));
		const FWoTCanonicalJsonValue* ParityIdentity =
			ParityMap.FindMember(TEXT("identity"));
		if (Parts == nullptr || !Parts->IsArray() || Parts->Num() == 0
			|| ParityIdentity == nullptr
			|| ParityIdentity->GetKind() != EWoTCanonicalJsonKind::String
			|| ParityIdentity->GetString() != Name)
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP parity map entry has invalid identity or parts"),
				FString::Printf(TEXT("$.maps[%d]"), MapIndex)));
		}
		const FString SourcePath = FString(TEXT("Maps/")) + Name + TEXT(".wot");
		FWoTRequiredFact& BSP = Contract.Facts.AddDefaulted_GetRef();
		BSP.Domain = EWoTFactDomain::BSP;
		BSP.FactId = FString(TEXT("retail.bsp.")) + Name;
		BSP.SourcePath = SourcePath;
		BSP.SourceExport = Name + TEXT(".Model");
		BSP.Importer = TEXT("bsp-v1");
		BSP.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
		BSP.AuditRule = TEXT("audit.bsp-payload-v1");
		for (int32 PartIndex = 0; PartIndex < Parts->Num(); ++PartIndex)
		{
			const FWoTCanonicalJsonValue& Part = Parts->GetElement(PartIndex);
			const FWoTCanonicalJsonValue* Identity = Part.IsObject()
				? Part.FindMember(TEXT("part_identity")) : nullptr;
			if (Identity == nullptr
				|| Identity->GetKind() != EWoTCanonicalJsonKind::String)
			{
				return FResult::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a BSP parity part has no string identity"),
					FString::Printf(TEXT("$.maps[%d].parts[%d]"),
						MapIndex, PartIndex)));
			}
			WoTBSPWorldSpecAddOutput(BSP,
				FString(TEXT("cache/bsp/")) + Name + TEXT("/")
					+ Identity->GetString(),
				TEXT("wot-bsp-v3"));
		}
		FWoTRequiredFact& World = Contract.Facts.AddDefaulted_GetRef();
		World.Domain = EWoTFactDomain::World;
		World.FactId = FString(TEXT("retail.world.")) + Name;
		World.SourcePath = SourcePath;
		World.SourceExport = Name + TEXT(".Level");
		World.Importer = TEXT("world-v1");
		World.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
		World.AuditRule = TEXT("audit.world-payload-v1");
		WoTBSPWorldSpecAddOutput(World,
			FString(TEXT("cache/world/")) + Name, TEXT("wot-world-v3"));
	}
	return FResult::Success(MoveTemp(Contract));
}

bool WoTBSPWorldSpecSources(const TArray<FString>& Names,
	TArray<FWoTWorldPackageBytesForTesting>& OutSources)
{
	for (const FString& Name : Names)
	{
		FWoTWorldPackageBytesForTesting& Source = OutSources.AddDefaulted_GetRef();
		Source.SourcePath = FString(TEXT("Maps/")) + Name + TEXT(".wot");
		if (!FFileHelper::LoadFileToArray(Source.Bytes,
			*WoTBSPWorldSpecRepoPath(*(FString(TEXT("original/Maps/"))
				+ Name + TEXT(".wot"))))) return false;
	}
	return true;
}

struct FWoTBSPWorldSpecWritten
{
	FString LogicalId;
	FString Schema;
	FWoTSha256 Sha256;
	uint64 Size = 0;
	TArray<uint8> HeaderFrame;
	TArray<uint8> BSPPayload;
};

class FWoTBSPWorldSpecWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256& Sha256Digest,
		uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes || Bytes.Num() < 12
			|| FMemory::Memcmp(Bytes.GetData(), "WOTOBJ3", 8) != 0)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the world writer received an invalid typed object")));
		}
		const uint32 HeaderSize = static_cast<uint32>(Bytes[8])
			| (static_cast<uint32>(Bytes[9]) << 8)
			| (static_cast<uint32>(Bytes[10]) << 16)
			| (static_cast<uint32>(Bytes[11]) << 24);
		if (HeaderSize > static_cast<uint32>(Bytes.Num() - 12))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the world writer received a truncated typed-object header")));
		}
		FWoTBSPWorldSpecWritten& Written = Objects.AddDefaulted_GetRef();
		Written.LogicalId = FString(LogicalId);
		Written.Schema = FString(Schema);
		Written.Sha256 = Sha256Digest;
		Written.Size = static_cast<uint64>(Bytes.Num());
		Written.HeaderFrame.Append(Bytes.GetData(), 12 + static_cast<int32>(HeaderSize));
		if (Schema == TEXT("wot-bsp-v3"))
		{
			const int32 PayloadOffset = 12 + static_cast<int32>(HeaderSize);
			Written.BSPPayload.Append(
				Bytes.GetData() + PayloadOffset, Bytes.Num() - PayloadOffset);
		}
		return FWoTBootstrapStatus::Success();
	}

	const FWoTBSPWorldSpecWritten* Find(FStringView LogicalId) const
	{
		for (const FWoTBSPWorldSpecWritten& Object : Objects)
			if (Object.LogicalId == LogicalId) return &Object;
		return nullptr;
	}

	TArray<FWoTBSPWorldSpecWritten> Objects;
};

bool WoTBSPWorldSpecDescriptorsEqual(
	TConstArrayView<FWoTWorldDescriptor> Left,
	TConstArrayView<FWoTWorldDescriptor> Right)
{
	if (Left.Num() != Right.Num()) return false;
	for (int32 DescriptorIndex = 0;
		DescriptorIndex < Left.Num(); ++DescriptorIndex)
	{
		const FWoTWorldDescriptor& A = Left[DescriptorIndex];
		const FWoTWorldDescriptor& B = Right[DescriptorIndex];
		if (A.MapIdentity != B.MapIdentity
			|| A.NeutralMapSlot != B.NeutralMapSlot
			|| A.BSPFactId != B.BSPFactId
			|| A.BSPImporter != B.BSPImporter
			|| A.WorldLogicalId != B.WorldLogicalId
			|| A.WorldSchema != B.WorldSchema
			|| A.WorldFactId != B.WorldFactId
			|| A.WorldImporter != B.WorldImporter
			|| A.PackageSha256 != B.PackageSha256
			|| A.WorldSha256 != B.WorldSha256
			|| A.SourceRoot != B.SourceRoot || A.MapRoot != B.MapRoot
			|| A.ActorCount != B.ActorCount
			|| A.AttachmentCount != B.AttachmentCount
			|| A.BSPNodeCount != B.BSPNodeCount
			|| A.BSPSurfaceCount != B.BSPSurfaceCount
			|| A.ZoneCount != B.ZoneCount
			|| A.PortalSurfaceCount != B.PortalSurfaceCount
			|| A.MoverCount != B.MoverCount
			|| A.LightCount != B.LightCount
			|| A.SoundCount != B.SoundCount
			|| A.NavigationSeedCount != B.NavigationSeedCount
			|| A.SemanticObjectCount != B.SemanticObjectCount
			|| A.ReachSpecCount != B.ReachSpecCount
			|| A.BSPParts.Num() != B.BSPParts.Num())
		{
			return false;
		}
		for (int32 PartIndex = 0; PartIndex < A.BSPParts.Num(); ++PartIndex)
		{
			const FWoTBSPPartDescriptor& AP = A.BSPParts[PartIndex];
			const FWoTBSPPartDescriptor& BP = B.BSPParts[PartIndex];
			if (AP.Role != BP.Role || AP.PartIdentity != BP.PartIdentity
				|| AP.ProceduralMaterialSlot != BP.ProceduralMaterialSlot
				|| AP.LogicalId != BP.LogicalId || AP.Schema != BP.Schema
				|| AP.FactId != BP.FactId
				|| AP.PayloadSha256 != BP.PayloadSha256
				|| AP.Sha256 != BP.Sha256
				|| AP.VertexCount != BP.VertexCount
				|| AP.IndexCount != BP.IndexCount
				|| AP.SectionCount != BP.SectionCount)
			{
				return false;
			}
		}
	}
	return true;
}

class FWoTBSPWorldSpecProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView Stage, uint64 Completed,
		uint64 Total) override
	{
		LastStage = FString(Stage);
		LastCompleted = Completed;
		LastTotal = Total;
		if (Stage == TEXT("world-set-discovery")
			&& Completed >= CancelDiscoveryAt) bCancelled = true;
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return bCancelled; }
	FString LastStage;
	uint64 LastCompleted = 0;
	uint64 LastTotal = 0;
	uint64 CancelDiscoveryAt = MAX_uint64;
	bool bCancelled = false;
};

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTBSPWorldSpecHeader(
	const FWoTBSPWorldSpecWritten& Written)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	const uint32 HeaderSize = static_cast<uint32>(Written.HeaderFrame[8])
		| (static_cast<uint32>(Written.HeaderFrame[9]) << 8)
		| (static_cast<uint32>(Written.HeaderFrame[10]) << 16)
		| (static_cast<uint32>(Written.HeaderFrame[11]) << 24);
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 1024 * 1024;
	Limits.MaxMembers = 1000000;
	return ParseCanonicalJson(TConstArrayView<uint8>(
		Written.HeaderFrame.GetData() + 12, static_cast<int32>(HeaderSize)), Limits);
}

bool WoTBSPWorldSpecHasExactKeys(FAutomationTestBase& Test,
	const FWoTCanonicalJsonValue& Object, const TCHAR* CommaKeys,
	const FString& Label)
{
	TArray<FString> Keys;
	FString(CommaKeys).ParseIntoArray(Keys, TEXT(","), true);
	bool bOkay = Test.TestEqual(Label + TEXT(" member count"), Object.Num(), Keys.Num());
	for (const FString& Key : Keys)
	{
		bOkay &= Test.TestTrue(Label + TEXT(" contains ") + Key,
			Object.FindMember(Key) != nullptr);
	}
	return bOkay;
}

struct FWoTBSPWorldSpecSection
{
	FString MaterialSlot;
	FString CanonicalPackage;
	FString CanonicalName;
	FString SourceTextureIdentity;
	FString PanZoneActorIdentity;
	uint32 SurfaceFlags = 0;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	uint32 TriangleCount = 0;
	uint32 MinVertex = 0;
	uint32 MaxVertex = 0;
	bool bCollisionEnabled = false;
};

struct FWoTBSPWorldSpecEntrySurface
{
	uint32 SurfaceIndex = 0;
	uint32 X = 0;
	uint32 Y = 0;
	uint32 W = 0;
	uint32 H = 0;
};

struct FWoTBSPWorldSpecEntryPair
{
	uint32 LightIndex = 0;
	uint32 SurfaceIndex = 0;
};

struct FWoTBSPWorldSpecPayload
{
	uint16 Version = 0;
	uint16 Flags = 0;
	uint16 UVChannels = 0;
	uint16 TangentAlgorithm = 0;
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	uint32 SectionCount = 0;
	uint32 BoundsBits[6] = {};
	TArray<FWoTBSPWorldSpecSection> Sections;
	uint64 SectionTableEnd = 0;
	uint64 MacroExtensionByteCount = 0;
	uint64 PositionsOffset = 0;
	uint64 NormalsOffset = 0;
	uint64 UV0Offset = 0;
	uint64 UV1Offset = 0;
	uint64 IndicesOffset = 0;
	uint64 IndicesEnd = 0;
	uint32 AtlasWidth = 0;
	uint32 AtlasHeight = 0;
	uint32 AtlasByteCount = 0;
	uint64 AtlasPixelsOffset = 0;
	/** Version-6 sky-rest trailer (flag 32): offset and byte count, 0 if absent. */
	uint64 SkyRestOffset = 0;
	uint32 SkyRestByteCount = 0;
	uint64 TriangleSurfacesOffset = 0;
	uint32 TriangleSurfacesByteCount = 0;
	uint32 SurfaceCount = 0;
	TArray<uint32> TriangleSurfaces;
	TArray<FWoTBSPWorldSpecEntrySurface> EntrySurfaces;
	TArray<FString> EntryLightIdentities;
	TArray<FWoTBSPWorldSpecEntryPair> EntryPairs;
};

bool WoTBSPWorldSpecU16(TConstArrayView<uint8> Bytes, uint64 Offset, uint16& Out)
{
	if (Offset > static_cast<uint64>(Bytes.Num())
		|| 2 > static_cast<uint64>(Bytes.Num()) - Offset) return false;
	Out = static_cast<uint16>(Bytes[static_cast<int32>(Offset)])
		| (static_cast<uint16>(Bytes[static_cast<int32>(Offset + 1)]) << 8);
	return true;
}

bool WoTBSPWorldSpecU32(TConstArrayView<uint8> Bytes, uint64 Offset, uint32& Out)
{
	if (Offset > static_cast<uint64>(Bytes.Num())
		|| 4 > static_cast<uint64>(Bytes.Num()) - Offset) return false;
	Out = static_cast<uint32>(Bytes[static_cast<int32>(Offset)])
		| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 1)]) << 8)
		| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 2)]) << 16)
		| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 3)]) << 24);
	return true;
}

bool WoTBSPWorldSpecPayloadLayout(TConstArrayView<uint8> Bytes,
	FWoTBSPWorldSpecPayload& Out)
{
	if (Bytes.Num() < 52 || FMemory::Memcmp(Bytes.GetData(), "WOTPBSP", 8) != 0
		|| !WoTBSPWorldSpecU16(Bytes, 8, Out.Version)
		|| !WoTBSPWorldSpecU16(Bytes, 10, Out.Flags)
		|| !WoTBSPWorldSpecU32(Bytes, 12, Out.VertexCount)
		|| !WoTBSPWorldSpecU32(Bytes, 16, Out.IndexCount)
		|| !WoTBSPWorldSpecU32(Bytes, 20, Out.SectionCount)
		|| !WoTBSPWorldSpecU16(Bytes, 24, Out.UVChannels)
		|| !WoTBSPWorldSpecU16(Bytes, 26, Out.TangentAlgorithm)) return false;
	for (uint32 Index = 0; Index < 6; ++Index)
		if (!WoTBSPWorldSpecU32(Bytes, 28 + Index * 4, Out.BoundsBits[Index])) return false;
	uint64 Offset = 52;
	for (uint32 SectionIndex = 0; SectionIndex < Out.SectionCount; ++SectionIndex)
	{
		uint16 NameLength = 0, Reserved = 0;
		FWoTBSPWorldSpecSection& Section = Out.Sections.AddDefaulted_GetRef();
		uint32 Collision = 0;
		if (!WoTBSPWorldSpecU16(Bytes, Offset, NameLength)
			|| !WoTBSPWorldSpecU16(Bytes, Offset + 2, Reserved)
			|| !WoTBSPWorldSpecU32(Bytes, Offset + 4, Section.FirstIndex)
			|| !WoTBSPWorldSpecU32(Bytes, Offset + 8, Section.IndexCount)
			|| !WoTBSPWorldSpecU32(Bytes, Offset + 12, Section.MinVertex)
			|| !WoTBSPWorldSpecU32(Bytes, Offset + 16, Section.MaxVertex)
			|| !WoTBSPWorldSpecU32(Bytes, Offset + 20, Collision)
			|| Reserved != 0 || NameLength == 0 || (Collision & 0xffffff00u) != 0)
		{
			return false;
		}
		Section.bCollisionEnabled = Collision != 0;
		Section.TriangleCount = Section.IndexCount / 3;
		Offset += 24;
		if (Offset > static_cast<uint64>(Bytes.Num())
			|| NameLength > static_cast<uint64>(Bytes.Num()) - Offset) return false;
		for (uint16 ByteIndex = 0; ByteIndex < NameLength; ++ByteIndex)
		{
			const uint8 Byte = Bytes[static_cast<int32>(Offset + ByteIndex)];
			if (Byte < 32 || Byte >= 127) return false;
			Section.MaterialSlot.AppendChar(static_cast<TCHAR>(Byte));
		}
		Offset += NameLength;
		if (Out.Version >= 4)
		{
			uint16 PackageLength = 0, TextureLength = 0;
			uint16 SourceLength = 0, PanZoneLength = 0;
			if (!WoTBSPWorldSpecU16(Bytes, Offset, PackageLength)
				|| !WoTBSPWorldSpecU16(Bytes, Offset + 2, TextureLength)
				|| !WoTBSPWorldSpecU16(Bytes, Offset + 4, SourceLength)
				|| !WoTBSPWorldSpecU16(Bytes, Offset + 6, PanZoneLength)
				|| !WoTBSPWorldSpecU32(Bytes, Offset + 8, Section.SurfaceFlags))
			{
				return false;
			}
			Offset += 12;
			auto ReadIdentity = [&Bytes, &Offset](uint16 Length, FString& Value)
			{
				if (Offset > static_cast<uint64>(Bytes.Num())
					|| Length > static_cast<uint64>(Bytes.Num()) - Offset) return false;
				for (uint16 ByteIndex = 0; ByteIndex < Length; ++ByteIndex)
				{
					const uint8 Byte = Bytes[static_cast<int32>(Offset + ByteIndex)];
					if (Byte < 32 || Byte >= 127) return false;
					Value.AppendChar(static_cast<TCHAR>(Byte));
				}
				Offset += Length;
				return true;
			};
			if (!ReadIdentity(PackageLength, Section.CanonicalPackage)
				|| !ReadIdentity(TextureLength, Section.CanonicalName)
				|| !ReadIdentity(SourceLength, Section.SourceTextureIdentity)
				|| !ReadIdentity(PanZoneLength, Section.PanZoneActorIdentity)
				|| (Section.MaterialSlot != TEXT("__None")
					&& (Section.CanonicalPackage.IsEmpty()
						|| Section.CanonicalName.IsEmpty()
						|| Section.SourceTextureIdentity.IsEmpty())))
			{
				return false;
			}
			if (Out.Version == 10)
			{
				uint16 MacroPackageLength = 0, MacroTextureLength = 0;
				uint16 MacroSourceLength = 0, MacroReserved = 0;
				uint32 MacroRatioBits = 0, MacroOffsetUBits = 0, MacroOffsetVBits = 0;
				if (!WoTBSPWorldSpecU16(Bytes, Offset, MacroPackageLength)
					|| !WoTBSPWorldSpecU16(Bytes, Offset + 2, MacroTextureLength)
					|| !WoTBSPWorldSpecU16(Bytes, Offset + 4, MacroSourceLength)
					|| !WoTBSPWorldSpecU16(Bytes, Offset + 6, MacroReserved)
					|| !WoTBSPWorldSpecU32(Bytes, Offset + 8, MacroRatioBits)
					|| !WoTBSPWorldSpecU32(Bytes, Offset + 12, MacroOffsetUBits)
					|| !WoTBSPWorldSpecU32(Bytes, Offset + 16, MacroOffsetVBits)
					|| MacroReserved != 0)
				{
					return false;
				}
				Offset += 20;
				Out.MacroExtensionByteCount += 20
					+ static_cast<uint64>(MacroPackageLength)
					+ static_cast<uint64>(MacroTextureLength)
					+ static_cast<uint64>(MacroSourceLength);
				FString MacroPackage, MacroTexture, MacroSource;
				if (!ReadIdentity(MacroPackageLength, MacroPackage)
					|| !ReadIdentity(MacroTextureLength, MacroTexture)
					|| !ReadIdentity(MacroSourceLength, MacroSource))
				{
					return false;
				}
			}
		}
	}
	Out.SectionTableEnd = Offset;
	Out.PositionsOffset = Offset;
	Out.NormalsOffset = Out.PositionsOffset
		+ static_cast<uint64>(Out.VertexCount) * 12;
	Out.UV0Offset = Out.NormalsOffset + static_cast<uint64>(Out.VertexCount) * 12;
	Out.UV1Offset = Out.UV0Offset + static_cast<uint64>(Out.VertexCount) * 8;
	Out.IndicesOffset = Out.UV1Offset + static_cast<uint64>(Out.VertexCount) * 8;
	const uint64 IndexBytes = static_cast<uint64>(Out.IndexCount) * 4;
	if (Out.IndicesOffset > static_cast<uint64>(Bytes.Num())
		|| IndexBytes > static_cast<uint64>(Bytes.Num()) - Out.IndicesOffset)
	{
		return false;
	}
	Out.IndicesEnd = Out.IndicesOffset + IndexBytes;
	const bool bIdentityVersion = (Out.Version >= 4 && Out.Version <= 6)
		|| Out.Version == 10;
	const bool bHasAtlas = Out.Version == 2 || Out.Version == 3
		|| (bIdentityVersion && (Out.Flags & 4) != 0);
	const bool bHasEntryTrailer = Out.Version == 3
		|| (bIdentityVersion && (Out.Flags & 8) != 0);
	const bool bHasAnimationTrailer = Out.Version >= 5 && (Out.Flags & 16) != 0;
	const bool bHasSkyRestTrailer = (Out.Version == 6 || Out.Version == 10)
		&& (Out.Flags & 32) != 0;
	const bool bHasTriangleSurfaces = Out.Version == 10 && (Out.Flags & 64) != 0;
	auto ReadTriangleSurfaces = [&](uint64& TrailerOffset)
	{
		uint32 TriangleCount = 0;
		if (!WoTBSPWorldSpecU32(Bytes, TrailerOffset, Out.TriangleSurfacesByteCount)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 4, TriangleCount)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 8, Out.SurfaceCount)
			|| TriangleCount != Out.IndexCount / 3 || Out.SurfaceCount == 0
			|| Out.TriangleSurfacesByteCount != 12 + TriangleCount * 4
			|| Out.TriangleSurfacesByteCount
				> static_cast<uint64>(Bytes.Num()) - TrailerOffset)
			return false;
		Out.TriangleSurfacesOffset = TrailerOffset;
		Out.TriangleSurfaces.Reserve(static_cast<int32>(TriangleCount));
		for (uint32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
		{
			uint32 Surface = 0;
			if (!WoTBSPWorldSpecU32(Bytes, TrailerOffset + 12 + Triangle * 4, Surface)
				|| Surface >= Out.SurfaceCount) return false;
			Out.TriangleSurfaces.Add(Surface);
		}
		TrailerOffset += Out.TriangleSurfacesByteCount;
		return true;
	};
	if (Out.Version == 1 || (bIdentityVersion && !bHasAtlas))
	{
		uint64 TrailerOffset = Out.IndicesEnd;
		return !bHasEntryTrailer && !bHasAnimationTrailer && !bHasSkyRestTrailer
			&& (!bHasTriangleSurfaces || ReadTriangleSurfaces(TrailerOffset))
			&& TrailerOffset == static_cast<uint64>(Bytes.Num());
	}
	if ((Out.Version != 2 && Out.Version != 3 && !bIdentityVersion)
		|| !WoTBSPWorldSpecU32(Bytes, Out.IndicesEnd, Out.AtlasWidth)
		|| !WoTBSPWorldSpecU32(Bytes, Out.IndicesEnd + 4, Out.AtlasHeight)
		|| !WoTBSPWorldSpecU32(Bytes, Out.IndicesEnd + 8, Out.AtlasByteCount)
		|| Out.AtlasWidth < 1 || Out.AtlasWidth > 8192
		|| Out.AtlasHeight < 1 || Out.AtlasHeight > 8192)
	{
		return false;
	}
	const uint64 AtlasPixels = static_cast<uint64>(Out.AtlasWidth) * Out.AtlasHeight;
	if (AtlasPixels > 4096ull * 8192
		|| static_cast<uint64>(Out.AtlasByteCount) != AtlasPixels * 4)
	{
		return false;
	}
	Out.AtlasPixelsOffset = Out.IndicesEnd + 12;
	if (Out.AtlasPixelsOffset > static_cast<uint64>(Bytes.Num())
		|| static_cast<uint64>(Out.AtlasByteCount)
			> static_cast<uint64>(Bytes.Num()) - Out.AtlasPixelsOffset)
	{
		return false;
	}
	for (uint64 Pixel = 0; Pixel < AtlasPixels; ++Pixel)
	{
		if (Bytes[static_cast<int32>(Out.AtlasPixelsOffset + Pixel * 4 + 3)] != 255)
			return false;
	}
	const uint64 AtlasEnd = Out.AtlasPixelsOffset + Out.AtlasByteCount;
	if (bHasAnimationTrailer || bHasSkyRestTrailer || bHasTriangleSurfaces)
	{
		// Map trailers follow the atlas in flag order, each prefixed by its
		// own byte count; the last one ends the payload (entry trailers never
		// accompany them in the builder's output).
		if (bHasEntryTrailer) return false;
		uint64 TrailerOffset = AtlasEnd;
		if (bHasAnimationTrailer)
		{
			uint32 AnimationByteCount = 0;
			if (!WoTBSPWorldSpecU32(Bytes, TrailerOffset, AnimationByteCount)
				|| AnimationByteCount < 16
				|| AnimationByteCount > static_cast<uint64>(Bytes.Num()) - TrailerOffset)
				return false;
			TrailerOffset += AnimationByteCount;
		}
		if (bHasSkyRestTrailer)
		{
			if (!WoTBSPWorldSpecU32(Bytes, TrailerOffset, Out.SkyRestByteCount)
				|| Out.SkyRestByteCount < 24
				|| Out.SkyRestByteCount > static_cast<uint64>(Bytes.Num()) - TrailerOffset)
				return false;
			Out.SkyRestOffset = TrailerOffset;
			TrailerOffset += Out.SkyRestByteCount;
		}
		if (bHasTriangleSurfaces && !ReadTriangleSurfaces(TrailerOffset))
			return false;
		return TrailerOffset == static_cast<uint64>(Bytes.Num());
	}
	if (!bHasEntryTrailer) return AtlasEnd == static_cast<uint64>(Bytes.Num());

	uint32 TrailerByteCount = 0;
	uint32 TriangleCount = 0;
	uint32 SurfaceCount = 0;
	uint32 LightCount = 0;
	uint32 PairCount = 0;
	uint32 RootOutside = 0;
	if (!WoTBSPWorldSpecU32(Bytes, AtlasEnd, TrailerByteCount)
		|| !WoTBSPWorldSpecU32(Bytes, AtlasEnd + 4, TriangleCount)
		|| !WoTBSPWorldSpecU32(Bytes, AtlasEnd + 8, SurfaceCount)
		|| !WoTBSPWorldSpecU32(Bytes, AtlasEnd + 12, LightCount)
		|| !WoTBSPWorldSpecU32(Bytes, AtlasEnd + 16, PairCount)
		|| !WoTBSPWorldSpecU32(Bytes, AtlasEnd + 20, RootOutside)
		|| TrailerByteCount != static_cast<uint64>(Bytes.Num()) - AtlasEnd
		|| TriangleCount != Out.IndexCount / 3 || SurfaceCount == 0
		|| RootOutside > 1)
	{
		return false;
	}
	uint64 TrailerOffset = AtlasEnd + 24;
	const uint64 TriangleBytes = static_cast<uint64>(TriangleCount) * 4;
	if (TrailerOffset > static_cast<uint64>(Bytes.Num())
		|| TriangleBytes > static_cast<uint64>(Bytes.Num()) - TrailerOffset)
	{
		return false;
	}
	TrailerOffset += TriangleBytes;
	for (uint32 Surface = 0; Surface < SurfaceCount; ++Surface)
	{
		FWoTBSPWorldSpecEntrySurface& Record = Out.EntrySurfaces.AddDefaulted_GetRef();
		if (!WoTBSPWorldSpecU32(Bytes, TrailerOffset, Record.SurfaceIndex)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 4, Record.X)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 8, Record.Y)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 12, Record.W)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 16, Record.H))
		{
			return false;
		}
		TrailerOffset += 28;
	}
	for (uint32 Light = 0; Light < LightCount; ++Light)
	{
		uint16 IdentityBytes = 0;
		uint16 Reserved = 0;
		if (!WoTBSPWorldSpecU16(Bytes, TrailerOffset, IdentityBytes)
			|| !WoTBSPWorldSpecU16(Bytes, TrailerOffset + 2, Reserved)
			|| Reserved != 0 || IdentityBytes == 0)
		{
			return false;
		}
		TrailerOffset += 4;
		if (TrailerOffset > static_cast<uint64>(Bytes.Num())
			|| static_cast<uint64>(IdentityBytes) + 8
				> static_cast<uint64>(Bytes.Num()) - TrailerOffset)
		{
			return false;
		}
		FString& Identity = Out.EntryLightIdentities.AddDefaulted_GetRef();
		for (uint16 ByteIndex = 0; ByteIndex < IdentityBytes; ++ByteIndex)
		{
			const uint8 Byte = Bytes[static_cast<int32>(TrailerOffset + ByteIndex)];
			if (Byte < 32 || Byte >= 127) return false;
			Identity.AppendChar(static_cast<TCHAR>(Byte));
		}
		TrailerOffset += static_cast<uint64>(IdentityBytes) + 8;
	}
	for (uint32 Pair = 0; Pair < PairCount; ++Pair)
	{
		FWoTBSPWorldSpecEntryPair& Record = Out.EntryPairs.AddDefaulted_GetRef();
		uint32 ByteCount = 0;
		if (!WoTBSPWorldSpecU32(Bytes, TrailerOffset, Record.LightIndex)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 4, Record.SurfaceIndex)
			|| !WoTBSPWorldSpecU32(Bytes, TrailerOffset + 8, ByteCount))
		{
			return false;
		}
		TrailerOffset += 12;
		if (TrailerOffset > static_cast<uint64>(Bytes.Num())
			|| static_cast<uint64>(ByteCount)
				> static_cast<uint64>(Bytes.Num()) - TrailerOffset)
		{
			return false;
		}
		TrailerOffset += ByteCount;
	}
	return TrailerOffset == static_cast<uint64>(Bytes.Num());
}

float WoTBSPWorldSpecFloat(uint32 Bits)
{
	float Value = 0.0f;
	FMemory::Memcpy(&Value, &Bits, sizeof(Value));
	return Value;
}

bool WoTBSPWorldSpecComparePayload(FAutomationTestBase& Test,
	const FString& Label, TConstArrayView<uint8> ActualBytes,
	const FWoTCanonicalJsonValue& ExpectedPart, bool bExpectedAtlasCarrier)
{
	FWoTBSPWorldSpecPayload Actual;
	if (!Test.TestTrue(Label + TEXT("native payload layout"),
			WoTBSPWorldSpecPayloadLayout(ActualBytes, Actual))) return false;
	bool bOkay = true;
	const FString& ExpectedRole =
		ExpectedPart.FindMember(TEXT("role"))->GetString();
	// Flag 16 (animated light-map responses) rides only the atlas carrier of
	// a map with an animation manifest, flag 32 (the version-10 sky-rest
	// census) only the atlas carrier of a map with a sky zone; the
	// fixture pins the geometry, not either trailer.
	const bool bHasAnimation = (Actual.Flags & 16u) != 0;
	const bool bHasSkyRest = (Actual.Flags & 32u) != 0;
	bOkay &= Test.TestEqual(Label + TEXT("payload version"), Actual.Version,
		static_cast<uint16>(10));
	bOkay &= Test.TestTrue(Label + TEXT("surface provenance flag"), (Actual.Flags & 64u) != 0);
	bOkay &= Test.TestEqual(Label + TEXT("surface provenance count"),
		Actual.TriangleSurfaces.Num(), static_cast<int32>(Actual.IndexCount / 3));
	bOkay &= Test.TestTrue(Label + TEXT("surface provenance model range"),
		Actual.SurfaceCount > 0
			&& !Actual.TriangleSurfaces.ContainsByPredicate(
				[&Actual](uint32 Surface) { return Surface >= Actual.SurfaceCount; }));
	bOkay &= Test.TestTrue(Label + TEXT("animation flag requires the atlas carrier"),
		!bHasAnimation || bExpectedAtlasCarrier);
	bOkay &= Test.TestTrue(Label + TEXT("sky-rest flag requires the atlas carrier"),
		!bHasSkyRest || bExpectedAtlasCarrier);
	bOkay &= Test.TestEqual(Label + TEXT("payload role flags"),
		static_cast<uint16>(Actual.Flags & ~112u),
		static_cast<uint16>((ExpectedRole == TEXT("portal") ? 1 : 0) | 2
			| (bExpectedAtlasCarrier ? 4 : 0)));
	bOkay &= Test.TestEqual(Label + TEXT("payload vertex count"), Actual.VertexCount,
		static_cast<uint32>(ExpectedPart.FindMember(TEXT("vertex_count"))->GetInteger()));
	bOkay &= Test.TestEqual(Label + TEXT("payload index count"), Actual.IndexCount,
		static_cast<uint32>(ExpectedPart.FindMember(TEXT("index_count"))->GetInteger()));
	bOkay &= Test.TestEqual(Label + TEXT("payload section count"), Actual.SectionCount,
		static_cast<uint32>(ExpectedPart.FindMember(TEXT("section_count"))->GetInteger()));
	bOkay &= Test.TestEqual(Label + TEXT("payload UV channel count"), Actual.UVChannels,
		static_cast<uint16>(2));
	bOkay &= Test.TestEqual(Label + TEXT("payload tangent algorithm id"),
		Actual.TangentAlgorithm, static_cast<uint16>(1));
	int64 AnimationBytes = 0;
	if (bHasAnimation)
	{
		uint32 TrailerByteCount = 0;
		WoTBSPWorldSpecU32(ActualBytes,
			Actual.AtlasPixelsOffset + Actual.AtlasByteCount, TrailerByteCount);
		AnimationBytes = TrailerByteCount;
	}
	const int64 ExpectedByteLength =
		ExpectedPart.FindMember(TEXT("payload_byte_length"))->GetInteger()
		+ (bExpectedAtlasCarrier
			? 12 + static_cast<int64>(Actual.AtlasByteCount) : 0)
		+ AnimationBytes + static_cast<int64>(Actual.SkyRestByteCount)
		+ static_cast<int64>(Actual.MacroExtensionByteCount)
		+ static_cast<int64>(Actual.TriangleSurfacesByteCount);
	bOkay &= Test.TestEqual(Label + TEXT("payload byte length"),
		static_cast<int64>(ActualBytes.Num()),
		ExpectedByteLength);
	const FWoTCanonicalJsonValue& Samples =
		*ExpectedPart.FindMember(TEXT("payload_samples"));
	const FWoTCanonicalJsonValue& ExpectedSections =
		*Samples.FindMember(TEXT("sections"));
	if (!Test.TestEqual(Label + TEXT("material slot/section count"),
		Actual.Sections.Num(), ExpectedSections.Num())) return false;
	for (int32 Index = 0; Index < Actual.Sections.Num(); ++Index)
	{
		const FWoTBSPWorldSpecSection& A = Actual.Sections[Index];
		const FWoTCanonicalJsonValue& E = ExpectedSections.GetElement(Index);
		const FString Prefix = FString::Printf(TEXT("%ssections[%d] "), *Label, Index);
		bOkay &= Test.TestEqual(Prefix + TEXT("material slot"), A.MaterialSlot,
			E.FindMember(TEXT("material_slot"))->GetString());
		bOkay &= Test.TestEqual(Prefix + TEXT("first index"), A.FirstIndex,
			static_cast<uint32>(E.FindMember(TEXT("first_index"))->GetInteger()));
		bOkay &= Test.TestEqual(Prefix + TEXT("index count"), A.IndexCount,
			static_cast<uint32>(E.FindMember(TEXT("index_count"))->GetInteger()));
		bOkay &= Test.TestEqual(Prefix + TEXT("triangle count"), A.TriangleCount,
			static_cast<uint32>(E.FindMember(TEXT("triangle_count"))->GetInteger()));
		bOkay &= Test.TestEqual(Prefix + TEXT("min vertex"), A.MinVertex,
			static_cast<uint32>(E.FindMember(TEXT("min_vertex"))->GetInteger()));
		bOkay &= Test.TestEqual(Prefix + TEXT("max vertex"), A.MaxVertex,
			static_cast<uint32>(E.FindMember(TEXT("max_vertex"))->GetInteger()));
		bOkay &= Test.TestEqual(Prefix + TEXT("collision flag"), A.bCollisionEnabled,
			E.FindMember(TEXT("collision_enabled"))->GetBool());
		bOkay &= Test.TestEqual(Prefix + TEXT("canonical package"),
			A.CanonicalPackage, E.FindMember(TEXT("canonical_package"))->GetString());
		bOkay &= Test.TestEqual(Prefix + TEXT("canonical name"),
			A.CanonicalName, E.FindMember(TEXT("canonical_name"))->GetString());
		bOkay &= Test.TestEqual(Prefix + TEXT("source texture identity"),
			A.SourceTextureIdentity,
			E.FindMember(TEXT("source_texture_identity"))->GetString());
		bOkay &= Test.TestEqual(Prefix + TEXT("surface flags"),
			A.SurfaceFlags,
			static_cast<uint32>(E.FindMember(TEXT("surface_flags"))->GetInteger()));
		bOkay &= Test.TestEqual(Prefix + TEXT("pan-zone actor identity"),
			A.PanZoneActorIdentity,
			E.FindMember(TEXT("pan_zone_actor_identity"))->GetString());
	}
	if (!bOkay) return false;

	const FWoTCanonicalJsonValue* Hashes =
		ExpectedPart.FindMember(TEXT("payload_block_sha256"));
	if (!Test.TestNotNull(Label + TEXT("exact-block diagnostic hashes"), Hashes)
		|| !Test.TestTrue(Label + TEXT("exact-block diagnostic hash object"),
			Hashes->IsObject())) return false;
	auto CompareHash = [&Test, &Label, Hashes, ActualBytes](const TCHAR* Name,
		uint64 Begin, uint64 End)
	{
		const FWoTCanonicalJsonValue* Fixture = Hashes->FindMember(Name);
		if (Fixture == nullptr || Begin > End
			|| End > static_cast<uint64>(ActualBytes.Num())) return false;
		const FWoTSha256 ActualHash = Sha256(TConstArrayView<uint8>(
			ActualBytes.GetData() + static_cast<int32>(Begin),
			static_cast<int32>(End - Begin)));
		return Test.TestEqual(Label + Name + TEXT(" diagnostic block hash"),
			ActualHash.ToHex(), Fixture->GetString());
	};
	// The frozen Python oracle intentionally remains the pre-extension geometry
	// oracle. Re-encode the v4 section-table projection so the diagnostic hash
	// continues to pin every established byte while the independently parsed
	// v7 macro records account for the additive extension above.
	TArray<uint8> LegacySectionTable;
	auto AppendU16 = [&LegacySectionTable](uint16 Value)
	{
		LegacySectionTable.Add(static_cast<uint8>(Value));
		LegacySectionTable.Add(static_cast<uint8>(Value >> 8));
	};
	auto AppendU32 = [&LegacySectionTable](uint32 Value)
	{
		LegacySectionTable.Add(static_cast<uint8>(Value));
		LegacySectionTable.Add(static_cast<uint8>(Value >> 8));
		LegacySectionTable.Add(static_cast<uint8>(Value >> 16));
		LegacySectionTable.Add(static_cast<uint8>(Value >> 24));
	};
	auto AppendText = [&LegacySectionTable](const FString& Value)
	{
		FTCHARToUTF8 Encoded(*Value);
		LegacySectionTable.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	};
	for (const FWoTBSPWorldSpecSection& Section : Actual.Sections)
	{
		FTCHARToUTF8 Slot(*Section.MaterialSlot);
		FTCHARToUTF8 Package(*Section.CanonicalPackage);
		FTCHARToUTF8 Name(*Section.CanonicalName);
		FTCHARToUTF8 Source(*Section.SourceTextureIdentity);
		FTCHARToUTF8 PanZone(*Section.PanZoneActorIdentity);
		AppendU16(static_cast<uint16>(Slot.Length()));
		AppendU16(0);
		AppendU32(Section.FirstIndex);
		AppendU32(Section.IndexCount);
		AppendU32(Section.MinVertex);
		AppendU32(Section.MaxVertex);
		AppendU32(Section.bCollisionEnabled ? 1u : 0u);
		AppendText(Section.MaterialSlot);
		AppendU16(static_cast<uint16>(Package.Length()));
		AppendU16(static_cast<uint16>(Name.Length()));
		AppendU16(static_cast<uint16>(Source.Length()));
		AppendU16(static_cast<uint16>(PanZone.Length()));
		AppendU32(Section.SurfaceFlags);
		AppendText(Section.CanonicalPackage);
		AppendText(Section.CanonicalName);
		AppendText(Section.SourceTextureIdentity);
		AppendText(Section.PanZoneActorIdentity);
	}
	const FWoTCanonicalJsonValue* SectionHash = Hashes->FindMember(TEXT("section_table"));
	bOkay &= Test.TestNotNull(Label + TEXT("section-table diagnostic hash"), SectionHash)
		&& Test.TestEqual(Label + TEXT("section_table diagnostic block hash"),
			Sha256(LegacySectionTable).ToHex(), SectionHash->GetString());
	bOkay &= CompareHash(TEXT("indices"), Actual.IndicesOffset, Actual.IndicesEnd);

	// Migration-window bound: eight float32 epsilons times max(1, |a|, |b|).
	// This is <= 9.54e-7 absolute near zero and a ~1e-6 relative bound at
	// larger magnitudes: enough for the final float32 narrowing of clean,
	// ordered binary64 arithmetic, but orders of magnitude below a wrong scale.
	constexpr double FloatToleranceScale = 0.00000095367431640625;
	auto CompareFloatSamples = [&Test, &Label, &bOkay, ActualBytes,
		&Samples, FloatToleranceScale](const TCHAR* Name, uint64 ActualOffset,
		uint32 Count, uint32 Width)
	{
		const FWoTCanonicalJsonValue& Edges = *Samples.FindMember(Name);
		for (const TCHAR* Edge : {TEXT("head"), TEXT("end")})
		{
			const FWoTCanonicalJsonValue& Values = *Edges.FindMember(Edge);
			const uint32 Start = FCString::Strcmp(Edge, TEXT("head")) == 0
				? 0 : Count - static_cast<uint32>(Values.Num());
			for (int32 SampleIndex = 0; SampleIndex < Values.Num(); ++SampleIndex)
			{
				const uint32 Index = Start + static_cast<uint32>(SampleIndex);
				uint32 ActualBits = 0;
				if (!WoTBSPWorldSpecU32(
					ActualBytes, ActualOffset + Index * 4, ActualBits)) return false;
				const uint32 ExpectedBits = static_cast<uint32>(
					Values.GetElement(SampleIndex).GetInteger());
				if (ActualBits == 0x80000000u || ExpectedBits == 0x80000000u)
				{
					bOkay = false;
					Test.AddError(FString::Printf(
						TEXT("%s%s[%u][%u]: negative zero escaped canonicalization"),
						*Label, Name, Index / Width, Index % Width));
					continue;
				}
				const float ActualValue = WoTBSPWorldSpecFloat(ActualBits);
				const float ExpectedValue = WoTBSPWorldSpecFloat(ExpectedBits);
				const double Bound = FloatToleranceScale * FMath::Max(1.0,
					FMath::Max(FMath::Abs(static_cast<double>(ActualValue)),
						FMath::Abs(static_cast<double>(ExpectedValue))));
				if (!FMath::IsFinite(ActualValue) || !FMath::IsFinite(ExpectedValue)
					|| FMath::Abs(static_cast<double>(ActualValue)
						- static_cast<double>(ExpectedValue)) > Bound)
				{
					bOkay = false;
					Test.AddError(FString::Printf(
						TEXT("%s%s[%u][%u]: expected %.9g (0x%08x), native %.9g "
							"(0x%08x), tolerance %.9g"),
						*Label, Name, Index / Width, Index % Width,
						ExpectedValue, ExpectedBits, ActualValue, ActualBits, Bound));
				}
			}
		}
		return true;
	};
	auto CompareIndexSamples = [&Test, &Label, &bOkay, ActualBytes, &Samples,
		&Actual]()
	{
		const FWoTCanonicalJsonValue& Edges = *Samples.FindMember(TEXT("indices"));
		for (const TCHAR* Edge : {TEXT("head"), TEXT("end")})
		{
			const FWoTCanonicalJsonValue& Values = *Edges.FindMember(Edge);
			const uint32 Start = FCString::Strcmp(Edge, TEXT("head")) == 0
				? 0 : Actual.IndexCount - static_cast<uint32>(Values.Num());
			for (int32 SampleIndex = 0; SampleIndex < Values.Num(); ++SampleIndex)
			{
				uint32 ActualValue = 0;
				const uint32 Index = Start + static_cast<uint32>(SampleIndex);
				if (!WoTBSPWorldSpecU32(ActualBytes,
					Actual.IndicesOffset + Index * 4, ActualValue)) return false;
				const uint32 ExpectedValue = static_cast<uint32>(
					Values.GetElement(SampleIndex).GetInteger());
				if (ActualValue != ExpectedValue)
				{
					bOkay = false;
					Test.AddError(FString::Printf(
						TEXT("%sindices[%u]: expected %u, native %u"),
						*Label, Index, ExpectedValue, ActualValue));
				}
			}
		}
		return true;
	};
	bOkay &= CompareIndexSamples();
	const FWoTCanonicalJsonValue& ExpectedBounds =
		*Samples.FindMember(TEXT("bounds"));
	for (uint32 Bound = 0; Bound < 6; ++Bound)
	{
		uint32 ActualBits = Actual.BoundsBits[Bound];
		uint32 ExpectedBits = static_cast<uint32>(
			ExpectedBounds.GetElement(static_cast<int32>(Bound)).GetInteger());
		if (ActualBits == 0x80000000u || ExpectedBits == 0x80000000u)
		{
			bOkay = false;
			Test.AddError(FString::Printf(TEXT("%sbounds[%u]: negative zero"),
				*Label, Bound));
			continue;
		}
		const float ActualValue = WoTBSPWorldSpecFloat(ActualBits);
		const float ExpectedValue = WoTBSPWorldSpecFloat(ExpectedBits);
		const double Tolerance = FloatToleranceScale * FMath::Max(1.0,
			FMath::Max(FMath::Abs(static_cast<double>(ActualValue)),
				FMath::Abs(static_cast<double>(ExpectedValue))));
		if (!FMath::IsFinite(ActualValue) || !FMath::IsFinite(ExpectedValue)
			|| FMath::Abs(static_cast<double>(ActualValue)
				- static_cast<double>(ExpectedValue)) > Tolerance)
		{
			bOkay = false;
			Test.AddError(FString::Printf(
				TEXT("%sbounds[%u]: expected %.9g (0x%08x), native %.9g "
					"(0x%08x), tolerance %.9g"),
				*Label, Bound, ExpectedValue, ExpectedBits, ActualValue,
				ActualBits, Tolerance));
		}
	}
	bOkay &= CompareFloatSamples(TEXT("positions"), Actual.PositionsOffset,
		Actual.VertexCount * 3, 3);
	bOkay &= CompareFloatSamples(TEXT("normals"), Actual.NormalsOffset,
		Actual.VertexCount * 3, 3);
	bOkay &= CompareFloatSamples(TEXT("uv0"), Actual.UV0Offset,
		Actual.VertexCount * 2, 2);
	bOkay &= CompareFloatSamples(TEXT("uv1"), Actual.UV1Offset,
		Actual.VertexCount * 2, 2);
	return bOkay;
}
} // namespace

struct FWoTBSPWorldSpecAnimationRow
{
	uint32 A = 0;
	uint32 B = 0;
	uint32 C = 0;
	uint32 LightCount = 0;
	uint32 RectCount = 0;
};

struct FWoTBSPWorldSpecAnimation
{
	uint64 TrailerOffset = 0;
	uint32 TrailerByteCount = 0;
	uint32 SurfaceCount = 0;
	TArray<FWoTBSPWorldSpecAnimationRow> Groups;
	TArray<FWoTBSPWorldSpecAnimationRow> Terms;
};

/** Walks the version-5 animation trailer that follows the atlas. */
bool WoTBSPWorldSpecAnimationLayout(TConstArrayView<uint8> Bytes,
	const FWoTBSPWorldSpecPayload& Layout, FWoTBSPWorldSpecAnimation& Out)
{
	uint64 Offset = Layout.AtlasPixelsOffset + Layout.AtlasByteCount;
	Out.TrailerOffset = Offset;
	uint32 GroupCount = 0, TermCount = 0;
	if (!WoTBSPWorldSpecU32(Bytes, Offset, Out.TrailerByteCount)
		|| !WoTBSPWorldSpecU32(Bytes, Offset + 4, Out.SurfaceCount)
		|| Out.TrailerByteCount > static_cast<uint64>(Bytes.Num()) - Offset)
		return false;
	const uint64 TrailerEnd = Offset + Out.TrailerByteCount;
	Offset += 8 + static_cast<uint64>(Out.SurfaceCount) * 16;
	if (!WoTBSPWorldSpecU32(Bytes, Offset, GroupCount)) return false;
	Offset += 4;
	for (uint32 Index = 0; Index < GroupCount; ++Index)
	{
		FWoTBSPWorldSpecAnimationRow& Row = Out.Groups.AddDefaulted_GetRef();
		if (Offset + 8 > static_cast<uint64>(Bytes.Num())) return false;
		Row.A = Bytes[static_cast<int32>(Offset)];
		Row.B = Bytes[static_cast<int32>(Offset + 1)];
		Row.C = Bytes[static_cast<int32>(Offset + 2)];
		if (!WoTBSPWorldSpecU32(Bytes, Offset + 4, Row.LightCount)) return false;
		Offset += 8;
	}
	if (!WoTBSPWorldSpecU32(Bytes, Offset, TermCount)) return false;
	Offset += 4;
	TArray<uint32> Widths, Heights;
	{
		uint64 SurfaceOffset = Out.TrailerOffset + 8;
		for (uint32 Index = 0; Index < Out.SurfaceCount; ++Index)
		{
			uint32 W = 0, H = 0;
			if (!WoTBSPWorldSpecU32(Bytes, SurfaceOffset + 8, W)
				|| !WoTBSPWorldSpecU32(Bytes, SurfaceOffset + 12, H)) return false;
			Widths.Add(W);
			Heights.Add(H);
			SurfaceOffset += 16;
		}
	}
	for (uint32 Index = 0; Index < TermCount; ++Index)
	{
		FWoTBSPWorldSpecAnimationRow& Row = Out.Terms.AddDefaulted_GetRef();
		if (Offset + 12 > static_cast<uint64>(Bytes.Num())) return false;
		Row.A = Bytes[static_cast<int32>(Offset)];
		Row.B = Bytes[static_cast<int32>(Offset + 1)];
		Row.C = Bytes[static_cast<int32>(Offset + 2)];
		if (!WoTBSPWorldSpecU32(Bytes, Offset + 4, Row.LightCount)
			|| !WoTBSPWorldSpecU32(Bytes, Offset + 8, Row.RectCount)) return false;
		Offset += 12;
		for (uint32 Rect = 0; Rect < Row.RectCount; ++Rect)
		{
			uint32 Surface = 0;
			if (!WoTBSPWorldSpecU32(Bytes, Offset, Surface)
				|| Surface >= Out.SurfaceCount) return false;
			Offset += 4 + static_cast<uint64>(Widths[Surface]) * Heights[Surface] * 3;
		}
	}
	return Offset == TrailerEnd;
}

struct FWoTBSPWorldSpecSkyRest
{
	uint32 SkyZone = 0;
	uint32 RayCount = 0;
	uint32 CoveredRays = 0;
	/** "surface:visible/blocker/fakebackdrop" rows joined by spaces, ascending. */
	FString Surfaces;
	/** "role:slot:triangles" rows in trailer order. */
	TArray<FString> Groups;
	uint32 TriangleCount = 0;
};

/** Walks the version-6 sky-rest trailer (flag 32) of a main part. */
bool WoTBSPWorldSpecSkyRestLayout(TConstArrayView<uint8> Bytes,
	const FWoTBSPWorldSpecPayload& Layout, FWoTBSPWorldSpecSkyRest& Out)
{
	if (Layout.SkyRestByteCount == 0) return false;
	uint64 Offset = Layout.SkyRestOffset + 4;
	const uint64 End = Layout.SkyRestOffset + Layout.SkyRestByteCount;
	uint32 SurfaceCount = 0;
	if (!WoTBSPWorldSpecU32(Bytes, Offset, Out.SkyZone)
		|| !WoTBSPWorldSpecU32(Bytes, Offset + 4, Out.RayCount)
		|| !WoTBSPWorldSpecU32(Bytes, Offset + 8, Out.CoveredRays)
		|| !WoTBSPWorldSpecU32(Bytes, Offset + 12, SurfaceCount)) return false;
	Offset += 16;
	TArray<FString> Rows;
	for (uint32 Index = 0; Index < SurfaceCount; ++Index)
	{
		uint32 Values[4] = {};
		for (int32 Field = 0; Field < 4; ++Field)
			if (!WoTBSPWorldSpecU32(Bytes, Offset + Field * 4, Values[Field])) return false;
		Rows.Add(FString::Printf(TEXT("%u:%u/%u/%u"),
			Values[0], Values[1], Values[2], Values[3]));
		Offset += 16;
	}
	Out.Surfaces = FString::Join(Rows, TEXT(" "));
	uint32 GroupCount = 0;
	if (!WoTBSPWorldSpecU32(Bytes, Offset, GroupCount)) return false;
	Offset += 4;
	for (uint32 Index = 0; Index < GroupCount; ++Index)
	{
		if (Offset + 4 > End) return false;
		const uint8 Role = Bytes[static_cast<int32>(Offset)];
		uint16 SlotLength = 0;
		if (!WoTBSPWorldSpecU16(Bytes, Offset + 2, SlotLength)) return false;
		Offset += 4;
		FString Slot;
		for (uint16 Byte = 0; Byte < SlotLength; ++Byte)
		{
			if (Offset + Byte >= End) return false;
			Slot.AppendChar(static_cast<TCHAR>(Bytes[static_cast<int32>(Offset + Byte)]));
		}
		Offset += SlotLength;
		uint32 Triangles = 0;
		if (!WoTBSPWorldSpecU32(Bytes, Offset, Triangles)) return false;
		Offset += 4 + static_cast<uint64>(Triangles) * (3 + 3 * 7) * 4;
		Out.TriangleCount += Triangles;
		Out.Groups.Add(FString::Printf(TEXT("%u:%s:%u"), Role, *Slot, Triangles));
	}
	return Offset == End;
}

/** Intermediate/Conv/<Map>_skyrest.json (skybake.py --rest, the development
 * pipeline's own census): sky zone, covered fraction (6 decimals), whether the
 * bake reported SEALED (every ray covered), and the per-surface ray counts. */
struct FExpectedSkyRest
{
	const TCHAR* Map;
	int32 SkyZone;
	double CoveredFraction;
	bool bSealed;
	const TCHAR* Surfaces;
};
static const FExpectedSkyRest ExpectedSkyRests[] = {
	{TEXT("Arena_01"), 4, 1.0, true, TEXT("")},
	{TEXT("Arena_02"), 5, 1.0, true, TEXT("")},
	{TEXT("Arena_03"), -1, 0.0, false, TEXT("")},
	{TEXT("Arena_04"), 2, 0.489338, false, TEXT("252:1107888/0/0 253:983504/0/0 254:1000107/0/0 255:1156207/0/0 256:36034/0/0 887:153325/0/0")},
	{TEXT("Arena_05"), 6, 1.0, true, TEXT("")},
	{TEXT("Arena_06"), 1, 1.0, true, TEXT("")},
	{TEXT("Arena_07"), 3, 0.489338, false, TEXT("475:1107888/0/0 476:983504/0/0 477:1000107/0/0 478:1156207/0/0 479:36034/0/0 484:153325/0/0")},
	{TEXT("Arena_08"), 3, 1.0, true, TEXT("")},
	{TEXT("Arena_09"), -1, 0.0, false, TEXT("")},
	{TEXT("Arena_11"), 2, 1.0, true, TEXT("")},
	{TEXT("Arena_12"), -1, 0.0, false, TEXT("")},
	{TEXT("Battle2_01"), 2, 1.0, true, TEXT("")},
	{TEXT("Battle2_02"), 23, 0.999999, false, TEXT("")},
	{TEXT("Battle2_03"), 6, 1.0, true, TEXT("")},
	{TEXT("Battle2_04"), 6, 1.0, true, TEXT("")},
	{TEXT("Battle2_05"), 35, 1.0, true, TEXT("")},
	{TEXT("Battle3_01"), 19, 1.0, false, TEXT("")},
	{TEXT("Battle3_02"), 20, 0.489989, false, TEXT("983:1/0/0 985:0/3/0 1014:0/3/0 1015:0/2/0 1023:0/3/0 1024:0/3/0 1028:0/3/0 1060:0/3/0 1061:0/20/0 1062:0/2/0 1063:0/3/0 1064:0/1/0 1431:0/9/0 1432:0/3/0 1463:1024964/0/0 1464:982683/0/0 1465:1072585/0/0 1466:1164228/0/0 2105:0/9/0 2106:0/20/0 2107:0/26/0 2108:0/46/0 2109:0/24/0 2576:0/38/0 2633:0/181/0 2636:0/477/0")},
	{TEXT("Battle4_01"), 12, 1.0, false, TEXT("")},
};

/** Intermediate/Conv/<Map>_lmatlas.json animation manifests (dev pipeline):
    groups as (LightType, LightPeriod, LightPhase, lights); terms as
    (kind, group or 255, effect, lights). */
struct FExpectedAnimationManifest
{
	const TCHAR* Map;
	int32 GroupCount;
	int32 TermCount;
	uint32 Groups[4][4];
	uint32 Terms[6][4];
};
static const FExpectedAnimationManifest ExpectedAnimationManifests[] = {
	{TEXT("Arena_01"), 0, 0, {}, {}},
	{TEXT("Arena_02"), 0, 0, {}, {}},
	{TEXT("Arena_03"), 0, 0, {}, {}},
	{TEXT("Arena_04"), 0, 0, {}, {}},
	{TEXT("Arena_05"), 0, 0, {}, {}},
	{TEXT("Arena_06"), 0, 1, {}, {{1,255,3,18}}},
	{TEXT("Arena_07"), 0, 0, {}, {}},
	{TEXT("Arena_08"), 0, 0, {}, {}},
	{TEXT("Arena_09"), 0, 0, {}, {}},
	{TEXT("Arena_11"), 0, 0, {}, {}},
	{TEXT("Arena_12"), 4, 6, {{2,99,102,2}, {2,134,0,2}, {7,83,0,30}, {7,134,0,2}},
		{{0,0,0,2}, {0,1,0,2}, {0,2,0,30}, {0,3,0,2}, {1,255,3,3}, {2,1,3,2}}},
	{TEXT("Battle2_01"), 1, 1, {{2,100,0,1}}, {{0,0,0,1}}},
	{TEXT("Battle2_02"), 1, 2, {{2,150,0,1}}, {{0,0,0,1}, {1,255,3,1}}},
	{TEXT("Battle2_03"), 0, 1, {}, {{1,255,3,1}}},
	{TEXT("Battle2_04"), 0, 1, {}, {{1,255,2,4}}},
	{TEXT("Battle2_05"), 0, 2, {}, {{1,255,1,8}, {1,255,2,2}}},
	{TEXT("Battle3_01"), 1, 1, {{2,100,0,1}}, {{0,0,0,1}}},
	{TEXT("Battle3_02"), 0, 1, {}, {{1,255,3,1}}},
	{TEXT("Battle4_01"), 1, 2, {{2,100,0,1}}, {{0,0,0,1}, {1,255,2,2}}},
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldUnsupportedPropertySpec,
	"WoT.RetailReader.BSPWorld.UnsupportedPropertyTypeRejected",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldUnsupportedPropertySpec::RunTest(const FString&)
{
	// Name index 0, unsupported UE1 tag type 7 with a one-byte payload, then
	// name index 1 (None). With bRejectUnsupportedTypes restored to false this
	// is otherwise a completely well-formed property stream and would pass.
	const TArray<FString> Names{TEXT("Unsupported"), TEXT("None")};
	const TArray<uint8> Bytes{0x00, 0x07, 0x2a, 0x01};
	TestFalse(TEXT("the BSP world reader rejects an unsupported property type"),
		ReadWorldPropertiesFixtureForTesting(Names, Bytes));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldParityFixtureFailureSpec,
	"WoT.RetailReader.BSPWorld.ParityFixtureFailuresAreErrors",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldParityFixtureFailureSpec::RunTest(const FString&)
{
	for (const ANSICHAR* Bytes : {"{]", "{}"})
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Malformed =
			WoTBSPWorldSpecParseParityFixture(TConstArrayView<uint8>(
				reinterpret_cast<const uint8*>(Bytes), FCStringAnsi::Strlen(Bytes)));
		if (!TestTrue(TEXT("a malformed parity fixture returns ERROR"),
			Malformed.IsError())) return false;
		TestTrue(TEXT("the malformed-fixture ERROR names the BSP parity fixture"),
			Malformed.GetError().ToString().Contains(TEXT("BSP parity fixture")));
	}
	return true;
}

// MIGRATION WINDOW ONLY: structure/identity/topology are exact; serialized
// float components use the documented ~1e-6 bound in ComparePayload solely to
// accommodate Python/native evaluation order. Payload v1 has no tangent float
// stream, so its tangent algorithm id/name remain exact structural pins. Once
// Python is retired, this fixture becomes a frozen regression pin of verified
// NATIVE output and the migration tolerance is removed with the Python oracle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldRetailSetSpec,
	"WoT.RetailReader.BSPWorld.RealRetailAtomicSetAndPins", WoTBSPWorldSpecFlags)

bool FWoTBSPWorldRetailSetSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	TArray<FString> Names;
	if (!TestTrue(TEXT("the generated fixture supplies exactly nineteen MP_MAPS"),
		WoTBSPWorldSpecMapNames(Names))) return false;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	if (!TestTrue(TEXT("all required retail maps are present"),
		WoTBSPWorldSpecSources(Names, Sources))) return false;
	for (int32 MapIndex = 0; MapIndex < Sources.Num(); ++MapIndex)
	{
		const TArray<uint8>& Bytes = Sources[MapIndex].Bytes;
		if (!TestTrue(FString::Printf(TEXT("%s has a package version field"),
			*Names[MapIndex]), Bytes.IsValidIndex(5))) return false;
		const uint16 Version = static_cast<uint16>(Bytes[4])
			| static_cast<uint16>(static_cast<uint16>(Bytes[5]) << 8);
		TestEqual(FString::Printf(
			TEXT("%s selects the v68 TravelInfo-map/destroy-observer branch"),
			*Names[MapIndex]), Version, static_cast<uint16>(68));
	}
	TWoTBootstrapResult<FWoTRetailContract> Contract =
		WoTBSPWorldSpecContract(Names);
	if (!TestFalse(TEXT("the BSP parity fixture builds the retail contract"),
		Contract.IsError()))
	{
		if (Contract.IsError()) AddError(Contract.GetError().ToString());
		return false;
	}
	FWoTBSPWorldSpecWriter Writer;
	FWoTBSPWorldSpecProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportRequiredWorldSetPackageBytesForTesting(
			Sources, Contract.GetValue(), Writer, Progress);
	if (!TestFalse(TEXT("the complete retail world set imports"), Imported.IsError()))
	{
		if (Imported.IsError()) AddError(Imported.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("nineteen descriptors complete atomically"),
		Imported.GetValue().Worlds.Num(), 19);
	TestEqual(TEXT("output-committing set root is a SHA-256"),
		Imported.GetValue().SetRoot.ToHex().Len(), 64);
    auto EncodedDescriptors = WriteCacheWorldDescriptors(Imported.GetValue().Worlds);
    if (!TestFalse(TEXT("world descriptor records serialize"), EncodedDescriptors.IsError())) return false;
    auto ParsedDescriptors = ParseCacheWorldDescriptors(EncodedDescriptors.GetValue());
    TestTrue(TEXT("manifest descriptor records round trip all nineteen maps"),
        !ParsedDescriptors.IsError() && WoTBSPWorldSpecDescriptorsEqual(
            Imported.GetValue().Worlds, ParsedDescriptors.GetValue()));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parity =
		WoTBSPWorldSpecParityFixture();
	if (!TestFalse(TEXT("canonical Python BSP parity fixture parses and validates"),
		Parity.IsError()))
	{
		if (Parity.IsError()) AddError(FString(TEXT("BSP parity fixture ERROR: "))
			+ Parity.GetError().ToString());
		return false;
	}
	const FWoTCanonicalJsonValue* ParityFormat =
		Parity.GetValue().FindMember(TEXT("format"));
	if (!TestNotNull(TEXT("parity fixture has a format"), ParityFormat)) return false;
	TestEqual(TEXT("parity fixture uses structural/float migration schema"),
		ParityFormat->GetString(), FString(TEXT("wot-native-bsp-parity-v3")));
	const FWoTCanonicalJsonValue* ParityMaps =
		Parity.GetValue().FindMember(TEXT("maps"));
	if (!TestNotNull(TEXT("parity fixture has maps"), ParityMaps)) return false;
	TestEqual(TEXT("parity fixture covers every retail map"), ParityMaps->Num(), 19);
	const TCHAR* ExpectedAtlasSha256[] = {
		TEXT("c2c886dac0835a4d6d4b85946e6ff714f6ddb2a6e3a1fc110c98659d71b8ca6f"),
		TEXT("60995b02dc683212bd460dbe4598da73a8ca7efbee1b45509cb241609605d34c"),
		TEXT("5c5acfc2afdaafba16ecb7f5e7a5fd31074faad5613a837ac130bcae37e44714"),
		TEXT("859f289665764a901b35037e2a30330b5ec5b0e8a2331a57ce50aa560950385b"),
		TEXT("9238ce1424117131b1d9c7219d37c93c3032776dd647016a5f09845227b6e203"),
		TEXT("89da2fe4c366ff7521be40872266c1ff282a52b825af2fe9a5741414991009e7"),
		TEXT("59157fa8ddf29090964591ce71ff6e31794a1f9cb84875ab74d28d36aff76103"),
		TEXT("59441e42cf64759fe85a63abb94d0dd193c45c555eaaea3cca762dcfd80df290"),
		TEXT("bef4ee64f291400e73b323f62a2926fbbf6fa8827594a4d0c02573642ed5157b"),
		TEXT("2a9f242748e1d7bcf5ffeb0dad3a4fad5933ad81eae3914e651ee19e69d8d3fb"),
		TEXT("2af70ac7cbeb44a804fe9e2242b3528a31a6075db50abc2edede194f8d468fed"),
		TEXT("077075d76bca83b2d66a8d179b24d57a1cebe27c48623a3638507bf0ccf9de34"),
		TEXT("f66518f9279bbad7f7557e1c05d35ef279f008e20e3323e5a98fc4ee123d1bc8"),
		TEXT("6f36cd9f2798e946de9f8be36ad197bcfa50fb74946ceb7323d1cf0279ce3cfa"),
		TEXT("f077958d40530b1bca4d735d2a987ef1976cfa27c4df0c1a8ad3b2ab2f400e35"),
		TEXT("0aed11b64040833d9e86eea5fcf17cf2b7f519ab892a724f5d608b4b9d84cd6f"),
		TEXT("fbcd7979ea4ee21630e6cb34cc7ca915cb8b8a1b0483c58ea9b05367925a936d"),
		TEXT("56290e1d54e34f0d9d615abc20d25b8f4b54f492bf3a573eb9acb0b394407552"),
		TEXT("7e87fbbc0b56d6143fe906ab01e19eca36bcb886a668adee45b09463946a20c6")};
	const FString AtlasEvidenceDirectory = WoTBSPWorldSpecRepoPath(
		TEXT("Saved/ReleaseEvidence/mp-lightmaps/native"));
	IFileManager::Get().MakeDirectory(*AtlasEvidenceDirectory, true);
	for (int32 MapIndex = 0; MapIndex < Imported.GetValue().Worlds.Num(); ++MapIndex)
	{
		const FWoTWorldDescriptor& Actual =
			Imported.GetValue().Worlds[MapIndex];
		const FWoTCanonicalJsonValue& Expected =
			ParityMaps->GetElement(MapIndex);
		const FWoTCanonicalJsonValue* ExpectedParts =
			Expected.FindMember(TEXT("parts"));
		TestEqual(FString::Printf(TEXT("map %d parity identity"), MapIndex + 1),
			Actual.MapIdentity,
			Expected.FindMember(TEXT("identity"))->GetString());
		TestTrue(Actual.MapIdentity + TEXT(" per-texel diagnostic is bounded"),
			Actual.LightmapPerTexelZoneSurfaceCount <= Actual.BSPSurfaceCount);
		for (const TPair<FString, uint32>& Skip : Actual.LightmapSkippedLights)
		{
			TestTrue(Actual.MapIdentity + TEXT(" skip reason is named"),
				!Skip.Key.IsEmpty());
			TestTrue(Actual.MapIdentity + TEXT(" skip count is positive"),
				Skip.Value > 0);
		}
		if (!TestNotNull(FString::Printf(
			TEXT("%s parity parts"), *Actual.MapIdentity), ExpectedParts)) return false;
		TestEqual(FString::Printf(TEXT("%s canonical part count"),
			*Actual.MapIdentity), Actual.BSPParts.Num(), ExpectedParts->Num());
		const int32 PartCount = FMath::Min(
			Actual.BSPParts.Num(), ExpectedParts->Num());
		for (int32 PartIndex = 0; PartIndex < PartCount; ++PartIndex)
		{
			const FWoTBSPPartDescriptor& ActualPart =
				Actual.BSPParts[PartIndex];
			const FWoTCanonicalJsonValue& ExpectedPart =
				ExpectedParts->GetElement(PartIndex);
			const FString Label = FString::Printf(TEXT("%s part %d "),
				*Actual.MapIdentity, PartIndex);
			TestEqual(Label + TEXT("identity"), ActualPart.PartIdentity,
				ExpectedPart.FindMember(TEXT("part_identity"))->GetString());
			TestEqual(Label + TEXT("procedural slot"),
				ActualPart.ProceduralMaterialSlot,
				ExpectedPart.FindMember(
					TEXT("procedural_material_slot"))->GetString());
			TestEqual(Label + TEXT("role"),
				ActualPart.Role == EWoTBSPPartRole::Portal
					? FString(TEXT("portal")) : FString(TEXT("main")),
				ExpectedPart.FindMember(TEXT("role"))->GetString());
			TestEqual(Label + TEXT("vertex count"),
				static_cast<int64>(ActualPart.VertexCount),
				ExpectedPart.FindMember(TEXT("vertex_count"))->GetInteger());
			TestEqual(Label + TEXT("index count"),
				static_cast<int64>(ActualPart.IndexCount),
				ExpectedPart.FindMember(TEXT("index_count"))->GetInteger());
			TestEqual(Label + TEXT("section count"),
				static_cast<int64>(ActualPart.SectionCount),
				ExpectedPart.FindMember(TEXT("section_count"))->GetInteger());
			TestEqual(Label + TEXT("triangle count"),
				static_cast<int64>(ActualPart.IndexCount / 3),
				ExpectedPart.FindMember(TEXT("triangle_count"))->GetInteger());
			TestEqual(Label + TEXT("tangent algorithm identity"),
				ExpectedPart.FindMember(TEXT("tangent_algorithm"))->GetString(),
				FString(TEXT("ue-mikk-preserve-normals-v1")));
			const FWoTBSPWorldSpecWritten* Written = Writer.Find(ActualPart.LogicalId);
			if (TestNotNull(Label + TEXT("written BSP object"), Written))
			{
				WoTBSPWorldSpecComparePayload(*this, Label,
					Written->BSPPayload, ExpectedPart,
					ActualPart.PartIdentity == Actual.MapIdentity);
			}
		}
		const FString MainLogicalId = FString(TEXT("cache/bsp/"))
			+ Actual.MapIdentity + TEXT("/") + Actual.MapIdentity;
		const FWoTBSPWorldSpecWritten* Main = Writer.Find(MainLogicalId);
		if (!TestNotNull(Actual.MapIdentity + TEXT(" main BSP exists"), Main))
			continue;
		FWoTBSPWorldSpecPayload MainPayload;
		if (!TestTrue(Actual.MapIdentity + TEXT(" main BSP has v4 atlas layout"),
			WoTBSPWorldSpecPayloadLayout(Main->BSPPayload, MainPayload))) continue;
		// The sky-rest census (skybake.py bake_rest) of every map
		// with a sky zone rides a version-6 flag-32 trailer; the development
		// pipeline's <Map>_skyrest.json is the parity oracle.
		const FExpectedSkyRest& ExpectedSky = ExpectedSkyRests[MapIndex];
		TestEqual(Actual.MapIdentity + TEXT(" sky-rest expectation order"),
			FString(ExpectedSky.Map), Actual.MapIdentity);
		const bool bHasSkyRest = (MainPayload.Flags & 32u) != 0;
		TestEqual(Actual.MapIdentity + TEXT(" carries a sky-rest census"),
			bHasSkyRest, ExpectedSky.SkyZone >= 0);
		TestEqual(Actual.MapIdentity + TEXT(" main BSP version"),
			MainPayload.Version, static_cast<uint16>(10));
		if (bHasSkyRest)
		{
			FWoTBSPWorldSpecSkyRest SkyRest;
			if (TestTrue(Actual.MapIdentity + TEXT(" sky-rest trailer parses"),
				WoTBSPWorldSpecSkyRestLayout(Main->BSPPayload, MainPayload, SkyRest)))
			{
				AddInfo(FString::Printf(TEXT("%s sky rest zone=%u covered=%u/%u surfaces=[%s] groups=[%s] triangles=%u"),
					*Actual.MapIdentity, SkyRest.SkyZone, SkyRest.CoveredRays,
					SkyRest.RayCount, *SkyRest.Surfaces,
					*FString::Join(SkyRest.Groups, TEXT(" ")), SkyRest.TriangleCount));
				TestEqual(Actual.MapIdentity + TEXT(" sky-rest zone"),
					static_cast<int32>(SkyRest.SkyZone), ExpectedSky.SkyZone);
				TestEqual(Actual.MapIdentity + TEXT(" sky-rest ray count"),
					SkyRest.RayCount, 4096u * 2048u);
				if (ExpectedSky.bSealed)
				{
					TestEqual(Actual.MapIdentity + TEXT(" sealed sky zone"),
						SkyRest.CoveredRays, SkyRest.RayCount);
				}
				else
				{
					TestTrue(Actual.MapIdentity + FString::Printf(
						TEXT(" covered fraction %.6f"),
						static_cast<double>(SkyRest.CoveredRays) / SkyRest.RayCount),
						FMath::Abs(static_cast<double>(SkyRest.CoveredRays)
							/ SkyRest.RayCount - ExpectedSky.CoveredFraction) <= 5e-7
						&& SkyRest.CoveredRays < SkyRest.RayCount);
				}
				TestEqual(Actual.MapIdentity + TEXT(" sky-rest surfaces equal skyrest.json"),
					SkyRest.Surfaces, FString(ExpectedSky.Surfaces));
				TArray<uint8> SkyRestBytes;
				SkyRestBytes.Append(Main->BSPPayload.GetData() + MainPayload.SkyRestOffset,
					static_cast<int32>(MainPayload.SkyRestByteCount));
				TestTrue(Actual.MapIdentity + TEXT(" native sky-rest evidence is written"),
					FFileHelper::SaveArrayToFile(SkyRestBytes,
						*(AtlasEvidenceDirectory / (Actual.MapIdentity + TEXT(".skyrest")))));
			}
		}
		// Animated light-map responses (Tools/convert/lightmaps.py
		// animation_manifest): the dev <Map>_lmatlas.json manifests pin the
		// group table and the term slot order per map; the trailer bytes are
		// written beside the atlas evidence for the Python response oracle.
		const FExpectedAnimationManifest& ExpectedAnimation =
			ExpectedAnimationManifests[MapIndex];
		TestEqual(Actual.MapIdentity + TEXT(" animation manifest order"),
			FString(ExpectedAnimation.Map), Actual.MapIdentity);
		FWoTBSPWorldSpecAnimation Animation;
		const bool bHasAnimation = (MainPayload.Flags & 16u) != 0;
		TestEqual(Actual.MapIdentity + TEXT(" carries an animation trailer"),
			bHasAnimation, ExpectedAnimation.TermCount > 0);
		if (bHasAnimation)
		{
			if (TestTrue(Actual.MapIdentity + TEXT(" animation trailer parses"),
				WoTBSPWorldSpecAnimationLayout(Main->BSPPayload, MainPayload, Animation)))
			{
				TestEqual(Actual.MapIdentity + TEXT(" animation surface table"),
					Animation.SurfaceCount, Actual.BSPSurfaceCount);
				TestEqual(Actual.MapIdentity + TEXT(" waveform group count"),
					Animation.Groups.Num(), ExpectedAnimation.GroupCount);
				TestEqual(Actual.MapIdentity + TEXT(" response term count"),
					Animation.Terms.Num(), ExpectedAnimation.TermCount);
				for (int32 Index = 0; Index < FMath::Min(Animation.Groups.Num(),
					ExpectedAnimation.GroupCount); ++Index)
				{
					const FWoTBSPWorldSpecAnimationRow& Row = Animation.Groups[Index];
					const uint32* ExpectedRow = ExpectedAnimation.Groups[Index];
					TestTrue(FString::Printf(TEXT("%s group %d is (%u,%u,%u) x%u"),
						*Actual.MapIdentity, Index, ExpectedRow[0], ExpectedRow[1],
						ExpectedRow[2], ExpectedRow[3]),
						Row.A == ExpectedRow[0] && Row.B == ExpectedRow[1]
						&& Row.C == ExpectedRow[2] && Row.LightCount == ExpectedRow[3]);
				}
				for (int32 Index = 0; Index < FMath::Min(Animation.Terms.Num(),
					ExpectedAnimation.TermCount); ++Index)
				{
					const FWoTBSPWorldSpecAnimationRow& Row = Animation.Terms[Index];
					const uint32* ExpectedRow = ExpectedAnimation.Terms[Index];
					TestTrue(FString::Printf(TEXT("%s term %d is kind %u group %u effect %u x%u"),
						*Actual.MapIdentity, Index, ExpectedRow[0], ExpectedRow[1],
						ExpectedRow[2], ExpectedRow[3]),
						Row.A == ExpectedRow[0] && Row.B == ExpectedRow[1]
						&& Row.C == ExpectedRow[2] && Row.LightCount == ExpectedRow[3]);
					TestTrue(Actual.MapIdentity + TEXT(" term has response rects"),
						Row.RectCount > 0);
				}
				TArray<uint8> TrailerBytes;
				TrailerBytes.Append(Main->BSPPayload.GetData() + Animation.TrailerOffset,
					static_cast<int32>(Animation.TrailerByteCount));
				TestTrue(Actual.MapIdentity + TEXT(" native animation evidence is written"),
					FFileHelper::SaveArrayToFile(TrailerBytes,
						*(AtlasEvidenceDirectory / (Actual.MapIdentity + TEXT(".anim")))));
			}
		}
		TestEqual(Actual.MapIdentity + TEXT(" atlas width"),
			MainPayload.AtlasWidth, 1024u);
		TestEqual(Actual.MapIdentity + TEXT(" atlas height"),
			MainPayload.AtlasHeight, 1024u);
		TArray<uint8> AtlasBytes;
		AtlasBytes.Append(Main->BSPPayload.GetData() + MainPayload.AtlasPixelsOffset,
			static_cast<int32>(MainPayload.AtlasByteCount));
		TestEqual(Actual.MapIdentity + TEXT(" atlas SHA-256"),
			Sha256(AtlasBytes).ToHex(), FString(ExpectedAtlasSha256[MapIndex]));
		TestTrue(Actual.MapIdentity + TEXT(" native atlas evidence is written"),
			FFileHelper::SaveArrayToFile(AtlasBytes,
				*(AtlasEvidenceDirectory / (Actual.MapIdentity + TEXT(".bgra")))));
	}
	const FWoTWorldDescriptor& Arena = Imported.GetValue().Worlds[0];
	TestEqual(TEXT("real pin names Arena_01"), Arena.MapIdentity, FString(TEXT("Arena_01")));
	TestEqual(TEXT("Arena_01 package content hash is pinned"), Arena.PackageSha256.ToHex(),
		FString(TEXT("eebe5698072f2571cee7f7cc41d5daf0280ef77c0520be48d8e284c8e3de7caf")));
	TestEqual(TEXT("Arena_01 source-root pin"), Arena.SourceRoot.ToHex(),
		FString(TEXT("484f424af7c8c6fc76e2fc389d34e6ab9c671ca5328961587e7d8ef8f38ea975")));
	TestNotEqual(TEXT("descriptor root is not source-only provenance"),
		Arena.MapRoot.ToHex(), Arena.SourceRoot.ToHex());
	TestEqual(TEXT("Arena_01 unique Level actors"), Arena.ActorCount, 991u);
	TestEqual(TEXT("Arena_01 BSP nodes"), Arena.BSPNodeCount, 5556u);
	TestEqual(TEXT("Arena_01 BSP surfaces"), Arena.BSPSurfaceCount, 2838u);
	TestEqual(TEXT("Arena_01 BSP zones"), Arena.ZoneCount, 11u);
	TestEqual(TEXT("Arena_01 PF_Portal surfaces"), Arena.PortalSurfaceCount, 42u);
	TestTrue(TEXT("Arena_01 carries light facts"), Arena.LightCount > 0);
	TestTrue(TEXT("Arena_01 carries sound facts"), Arena.SoundCount > 0);
	TestTrue(TEXT("Arena_01 carries navigation seeds"), Arena.NavigationSeedCount > 0);

	const FWoTBSPWorldSpecWritten* BSP =
		Writer.Find(TEXT("cache/bsp/Arena_01/Arena_01"));
	const FWoTBSPWorldSpecWritten* World = Writer.Find(TEXT("cache/world/Arena_01"));
	if (TestNotNull(TEXT("Arena_01 main BSP object exists"), BSP))
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header = WoTBSPWorldSpecHeader(*BSP);
		if (TestFalse(TEXT("BSP canonical header parses"), Header.IsError()))
		{
			WoTBSPWorldSpecHasExactKeys(*this, Header.GetValue(),
				TEXT("bounds_max_bits,bounds_min_bits,encoding,fact_id,importer,"
					"importer_version,index_count,kind,payload_byte_length,role,"
					"section_count,source,tangent_algorithm,triangle_count,uv_channels,"
					"vertex_count"), TEXT("BSP header"));
		}
	}
	if (TestNotNull(TEXT("Arena_01 world object exists"), World))
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header = WoTBSPWorldSpecHeader(*World);
		if (TestFalse(TEXT("world canonical header parses"), Header.IsError()))
		{
			WoTBSPWorldSpecHasExactKeys(*this, Header.GetValue(),
				TEXT("actor_count,attachment_count,bsp_node_count,bsp_surface_count,"
					"encoding,fact_id,importer,importer_version,kind,light_count,"
					"map_identity,mover_count,navigation_seed_count,payload_byte_length,"
					"portal_surface_count,reach_spec_count,semantic_object_count,"
					"sound_count,source,zone_count"),
				TEXT("world header"));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldRetailFrontEndPropertySpec,
	"WoT.RetailReader.BSPWorld.RealRetailFrontEndPropertyClosure",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldRetailFrontEndPropertySpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	if (!TestTrue(TEXT("required WheelOfTime.wot is present"),
		WoTBSPWorldSpecSources({TEXT("WheelOfTime")}, Sources))) return false;
	FWoTRetailContract Contract;
	FWoTRequiredFact& BSP = Contract.Facts.AddDefaulted_GetRef();
	BSP.Domain = EWoTFactDomain::BSP;
	BSP.FactId = TEXT("retail.bsp.WheelOfTime");
	BSP.SourcePath = TEXT("Maps/WheelOfTime.wot");
	BSP.SourceExport = TEXT("WheelOfTime.Model");
	BSP.Importer = TEXT("bsp-v1");
	WoTBSPWorldSpecAddOutput(BSP,
		TEXT("cache/bsp/WheelOfTime/WheelOfTime"), TEXT("wot-bsp-v3"));
	FWoTRequiredFact& World = Contract.Facts.AddDefaulted_GetRef();
	World.Domain = EWoTFactDomain::World;
	World.FactId = TEXT("retail.world.WheelOfTime");
	World.SourcePath = TEXT("Maps/WheelOfTime.wot");
	World.SourceExport = TEXT("WheelOfTime.Level");
	World.Importer = TEXT("world-v1");
	WoTBSPWorldSpecAddOutput(World,
		TEXT("cache/world/WheelOfTime"), TEXT("wot-world-v3"));
	FWoTBSPWorldSpecWriter Writer;
	FWoTBSPWorldSpecProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportFrontEndWorldPackageBytesForTesting(
			Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("the entry package passes native property validation"),
		Imported.IsError()))
	{
		if (Imported.IsError()) AddError(Imported.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("the entry world remains outside the nineteen-map set"),
		Imported.GetValue().Worlds.Num(), 1);
	TestEqual(TEXT("the imported entry identity"),
		Imported.GetValue().Worlds[0].MapIdentity, FString(TEXT("WheelOfTime")));
	TestEqual(TEXT("the entry retains its singular complete BSP contract"),
		Imported.GetValue().Worlds[0].BSPParts.Num(), 1);
	TestNotNull(TEXT("the typed entry world object is published"),
		Writer.Find(TEXT("cache/world/WheelOfTime")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldRetailFrontEndLightmapSpec,
	"WoT.RetailReader.BSPWorld.RealRetailFrontEndLightmapAtlas",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldRetailFrontEndLightmapSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *WoTBSPWorldSpecRepoPath(
			TEXT("original/Maps/WheelOfTime.wot"))))
	{
		AddError(TEXT("required original/Maps/WheelOfTime.wot is missing"));
		return false;
	}
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	Sources.Add({TEXT("Maps/WheelOfTime.wot"), MoveTemp(SourceBytes)});
	FWoTRetailContract Contract;
	FWoTRequiredFact& BSPFact = Contract.Facts.AddDefaulted_GetRef();
	BSPFact.Domain = EWoTFactDomain::BSP;
	BSPFact.FactId = TEXT("retail.bsp.WheelOfTime");
	BSPFact.SourcePath = TEXT("Maps/WheelOfTime.wot");
	BSPFact.SourceExport = TEXT("WheelOfTime.Model");
	BSPFact.Importer = TEXT("bsp-v1");
	WoTBSPWorldSpecAddOutput(BSPFact,
		TEXT("cache/bsp/WheelOfTime/WheelOfTime"), TEXT("wot-bsp-v3"));
	FWoTRequiredFact& WorldFact = Contract.Facts.AddDefaulted_GetRef();
	WorldFact.Domain = EWoTFactDomain::World;
	WorldFact.FactId = TEXT("retail.world.WheelOfTime");
	WorldFact.SourcePath = TEXT("Maps/WheelOfTime.wot");
	WorldFact.SourceExport = TEXT("WheelOfTime.Level");
	WorldFact.Importer = TEXT("world-v1");
	WoTBSPWorldSpecAddOutput(WorldFact,
		TEXT("cache/world/WheelOfTime"), TEXT("wot-world-v3"));
	FWoTBSPWorldSpecWriter Writer;
	FWoTBSPWorldSpecProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportFrontEndWorldPackageBytesForTesting(
			Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("the genuine entry map imports through the package-byte seam"),
		Imported.IsError()))
	{
		if (Imported.IsError()) AddError(Imported.GetError().ToString());
		return false;
	}
	const FWoTBSPWorldSpecWritten* BSP =
		Writer.Find(TEXT("cache/bsp/WheelOfTime/WheelOfTime"));
	if (!TestNotNull(TEXT("the typed entry BSP object is published"), BSP)) return false;
	FWoTBSPWorldSpecPayload Payload;
	if (!TestTrue(TEXT("the entry BSP has a bounded payload layout"),
		WoTBSPWorldSpecPayloadLayout(BSP->BSPPayload, Payload))) return false;
	if (!TestEqual(TEXT("entry weather selects identity-bearing WOTPBSP version 5"),
		Payload.Version, static_cast<uint16>(5))) return false;
	TestEqual(TEXT("entry atlas width follows the stable shelf pack"),
		Payload.AtlasWidth, 1024u);
	TestEqual(TEXT("entry atlas height follows the stable shelf pack"),
		Payload.AtlasHeight, 1024u);
	TestEqual(TEXT("entry atlas byte count is tightly packed BGRA8"),
		Payload.AtlasByteCount, 1024u * 1024u * 4u);
	bool bHasShadowedTexel = false;
	bool bHasLitTexel = false;
	for (uint64 Pixel = 0;
		Pixel < static_cast<uint64>(Payload.AtlasWidth) * Payload.AtlasHeight; ++Pixel)
	{
		const uint8* BGRA = BSP->BSPPayload.GetData()
			+ Payload.AtlasPixelsOffset + Pixel * 4;
		const uint8 Brightest = FMath::Max(BGRA[0], FMath::Max(BGRA[1], BGRA[2]));
		bHasShadowedTexel = bHasShadowedTexel
			|| (Brightest > 0 && Brightest <= 32);
		bHasLitTexel = bHasLitTexel
			|| (Brightest >= 64 && Brightest < 128);
	}
	TestTrue(TEXT("entry atlas retains shadowed texels"), bHasShadowedTexel);
	TestTrue(TEXT("entry atlas retains illuminated texels"), bHasLitTexel);
	TArray<uint8> AtlasBytes;
	AtlasBytes.Append(BSP->BSPPayload.GetData() + Payload.AtlasPixelsOffset,
		static_cast<int32>(Payload.AtlasByteCount));
	TestEqual(TEXT("entry steady atlas remains byte-exact"),
		Sha256(AtlasBytes).ToHex(),
		FString(TEXT("567041bd73bf8c149dae50fac64aaef7554775771632991aa147d9dd782d37d5")));
	const TArray<FString> ExpectedLightIdentities = {
		TEXT("WheelOfTime.Light156"),
		TEXT("WheelOfTime.Light157"),
		TEXT("WheelOfTime.Light158"),
		TEXT("WheelOfTime.Light159"),
		TEXT("WheelOfTime.Light34")
	};
	TestTrue(TEXT("entry carries the five sorted lightning identities"),
		Payload.EntryLightIdentities == ExpectedLightIdentities);
	TArray<bool> LightsWithAffectedRectangles;
	LightsWithAffectedRectangles.Init(false, ExpectedLightIdentities.Num());
	for (const FWoTBSPWorldSpecEntryPair& Pair : Payload.EntryPairs)
	{
		const bool bPairHasRectangle = Payload.EntrySurfaces.IsValidIndex(
			static_cast<int32>(Pair.SurfaceIndex))
			&& Payload.EntrySurfaces[Pair.SurfaceIndex].W > 0
			&& Payload.EntrySurfaces[Pair.SurfaceIndex].H > 0;
		TestTrue(TEXT("entry illumination pair targets a nonempty rectangle"),
			bPairHasRectangle);
		if (bPairHasRectangle && LightsWithAffectedRectangles.IsValidIndex(
				static_cast<int32>(Pair.LightIndex)))
		{
			LightsWithAffectedRectangles[Pair.LightIndex] = true;
		}
	}
	for (int32 LightIndex = 0; LightIndex < LightsWithAffectedRectangles.Num(); ++LightIndex)
	{
		TestTrue(FString::Printf(TEXT("entry lightning light %d affects a rectangle"),
			LightIndex), LightsWithAffectedRectangles[LightIndex]);
	}
	const FString EvidenceDirectory = WoTBSPWorldSpecRepoPath(
		TEXT("Saved/ReleaseEvidence/entry-lightmaps"));
	IFileManager::Get().MakeDirectory(*EvidenceDirectory, true);
	TestTrue(TEXT("native entry atlas evidence is written for oracle comparison"),
		FFileHelper::SaveArrayToFile(
			AtlasBytes, *(EvidenceDirectory / TEXT("native-atlas.bgra"))));
	const FString WeatherEvidenceDirectory = WoTBSPWorldSpecRepoPath(
		TEXT("Saved/ReleaseEvidence/entry-sky-weather"));
	IFileManager::Get().MakeDirectory(*WeatherEvidenceDirectory, true);
	TestTrue(TEXT("native entry v3 evidence is written for oracle comparison"),
		FFileHelper::SaveArrayToFile(BSP->BSPPayload,
			*(WeatherEvidenceDirectory / TEXT("native-entry-v3.bsp"))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldRetailFrontEndMalformedLightmapSpec,
	"WoT.RetailReader.BSPWorld.RealRetailFrontEndRejectsMalformedLightmaps",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldRetailFrontEndMalformedLightmapSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *WoTBSPWorldSpecRepoPath(
			TEXT("original/Maps/WheelOfTime.wot"))))
	{
		AddError(TEXT("required original/Maps/WheelOfTime.wot is missing"));
		return false;
	}
	FWoTRetailContract Contract;
	FWoTRequiredFact& BSPFact = Contract.Facts.AddDefaulted_GetRef();
	BSPFact.Domain = EWoTFactDomain::BSP;
	BSPFact.FactId = TEXT("retail.bsp.WheelOfTime");
	BSPFact.SourcePath = TEXT("Maps/WheelOfTime.wot");
	BSPFact.SourceExport = TEXT("WheelOfTime.Model");
	BSPFact.Importer = TEXT("bsp-v1");
	WoTBSPWorldSpecAddOutput(BSPFact,
		TEXT("cache/bsp/WheelOfTime/WheelOfTime"), TEXT("wot-bsp-v3"));
	FWoTRequiredFact& WorldFact = Contract.Facts.AddDefaulted_GetRef();
	WorldFact.Domain = EWoTFactDomain::World;
	WorldFact.FactId = TEXT("retail.world.WheelOfTime");
	WorldFact.SourcePath = TEXT("Maps/WheelOfTime.wot");
	WorldFact.SourceExport = TEXT("WheelOfTime.Level");
	WorldFact.Importer = TEXT("world-v1");
	WoTBSPWorldSpecAddOutput(WorldFact,
		TEXT("cache/world/WheelOfTime"), TEXT("wot-world-v3"));
	struct FMutation
	{
		const TCHAR* Label;
		int32 Offset;
		TArray<uint8> Expected;
		TArray<uint8> Replacement;
	};
	const FMutation Mutations[] = {
		{TEXT("out-of-range light-map DataOffset"), 197247,
			{0x00, 0x00, 0x00, 0x00}, {0x8c, 0x44, 0x01, 0x00}},
		{TEXT("out-of-range light-map LightActors index"), 197303,
			{0x9d, 0x08, 0x00, 0x00}, {0x36, 0x0d, 0x00, 0x00}},
		{TEXT("truncated LightBits declaration"), 209597,
			{0x4b, 0x92, 0x0a}, {0x7f, 0xff, 0x7f}},
	};
	for (const FMutation& Mutation : Mutations)
	{
		if (!TestTrue(FString(Mutation.Label) + TEXT(" retail byte pin"),
			SourceBytes.IsValidIndex(Mutation.Offset + Mutation.Expected.Num() - 1)
			&& FMemory::Memcmp(SourceBytes.GetData() + Mutation.Offset,
				Mutation.Expected.GetData(), Mutation.Expected.Num()) == 0))
		{
			return false;
		}
		TArray<FWoTWorldPackageBytesForTesting> Sources;
		FWoTWorldPackageBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = TEXT("Maps/WheelOfTime.wot");
		Source.Bytes = SourceBytes;
		FMemory::Memcpy(Source.Bytes.GetData() + Mutation.Offset,
			Mutation.Replacement.GetData(), Mutation.Replacement.Num());
		FWoTBSPWorldSpecWriter Writer;
		FWoTBSPWorldSpecProgress Progress;
		TWoTBootstrapResult<FWoTWorldSetResult> Imported =
			ImportFrontEndWorldPackageBytesForTesting(
				Sources, Contract, Writer, Progress);
		TestTrue(FString(Mutation.Label) + TEXT(" rejects"), Imported.IsError());
		TestEqual(FString(Mutation.Label) + TEXT(" writes no objects"),
			Writer.Objects.Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldAtomicFailureSpec,
	"WoT.RetailReader.BSPWorld.AbsentDuplicateLateCorruptInterrupted",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldAtomicFailureSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TArray<FString> Names;
	if (!WoTBSPWorldSpecMapNames(Names)) return false;
	TWoTBootstrapResult<FWoTRetailContract> Contract =
		WoTBSPWorldSpecContract(Names);
	if (Contract.IsError())
	{
		AddError(Contract.GetError().ToString());
		return false;
	}
	const FWoTRetailContract& Complete = Contract.GetValue();
	for (int32 MissingMap : {0, 9, 18})
	{
		FWoTRetailContract Missing = Complete;
		const FString MissingPath = FString(TEXT("Maps/")) + Names[MissingMap] + TEXT(".wot");
		Missing.Facts.RemoveAll([&MissingPath](const FWoTRequiredFact& Fact)
		{
			return Fact.SourcePath == MissingPath;
		});
		FWoTBSPWorldSpecWriter MissingWriter;
		FWoTBSPWorldSpecProgress MissingProgress;
		TArray<FWoTWorldPackageBytesForTesting> NoSources;
		TWoTBootstrapResult<FWoTWorldSetResult> Result =
			ImportRequiredWorldSetPackageBytesForTesting(
				NoSources, Missing, MissingWriter, MissingProgress);
		TestTrue(FString::Printf(TEXT("absent map %d rejects"), MissingMap + 1),
			Result.IsError());
		TestEqual(TEXT("an absent map leaves no completed objects"),
			MissingWriter.Objects.Num(), 0);
	}

	FWoTRetailContract Duplicate = Complete;
	Duplicate.Facts.Last(1).SourcePath = TEXT("Maps/Arena_01.wot");
	Duplicate.Facts.Last(1).SourceExport = TEXT("Arena_01.Model");
	Duplicate.Facts.Last().SourcePath = TEXT("Maps/Arena_01.wot");
	Duplicate.Facts.Last().SourceExport = TEXT("Arena_01.Level");
	FWoTBSPWorldSpecWriter DuplicateWriter;
	FWoTBSPWorldSpecProgress DuplicateProgress;
	TArray<FWoTWorldPackageBytesForTesting> NoSources;
	TWoTBootstrapResult<FWoTWorldSetResult> DuplicateResult =
		ImportRequiredWorldSetPackageBytesForTesting(
			NoSources, Duplicate, DuplicateWriter, DuplicateProgress);
	TestTrue(TEXT("duplicate world identity rejects"), DuplicateResult.IsError());
	TestEqual(TEXT("duplicate identity writes nothing"), DuplicateWriter.Objects.Num(), 0);

	TArray<FWoTWorldPackageBytesForTesting> Sources;
	if (!TestTrue(TEXT("all required retail maps are present"),
		WoTBSPWorldSpecSources(Names, Sources))) return false;
	FWoTBSPWorldSpecProgress InterruptedProgress;
	InterruptedProgress.CancelDiscoveryAt = 17;
	FWoTBSPWorldSpecWriter InterruptedWriter;
	TWoTBootstrapResult<FWoTWorldSetResult> Interrupted =
		ImportRequiredWorldSetPackageBytesForTesting(
			Sources, Complete, InterruptedWriter, InterruptedProgress);
	TestTrue(TEXT("interruption before the eighteenth map rejects"), Interrupted.IsError());
	TestEqual(TEXT("discovery interruption writes no temporary objects"),
		InterruptedWriter.Objects.Num(), 0);

	// Battle4_01 LevelInfo0.Summary is a two-byte ObjectProperty at 98,712:
	// export 3341 (Engine.LevelSummary). Retarget it, without changing the tag
	// size, to export 1830 (Engine.Light). Both are valid package objects; only
	// the generated ue1pkg reflection type registry can reject this mutation.
	TArray<FWoTWorldPackageBytesForTesting> WrongTypedSources = Sources;
	if (!TestTrue(TEXT("Battle4_01 retail pin contains the Summary object ref"),
		WrongTypedSources[18].Bytes.IsValidIndex(98713)
			&& WrongTypedSources[18].Bytes[98712] == 0x4d
			&& WrongTypedSources[18].Bytes[98713] == 0x34)) return false;
	WrongTypedSources[18].Bytes[98712] = 0x66;
	WrongTypedSources[18].Bytes[98713] = 0x1c;
	FWoTBSPWorldSpecWriter WrongTypeWriter;
	FWoTBSPWorldSpecProgress WrongTypeProgress;
	TWoTBootstrapResult<FWoTWorldSetResult> WrongType =
		ImportRequiredWorldSetPackageBytesForTesting(
			WrongTypedSources, Complete, WrongTypeWriter, WrongTypeProgress);
	TestTrue(TEXT("a valid but wrong-type object property rejects"),
		WrongType.IsError());
	TestEqual(TEXT("wrong-type registry closure rejects before the first write"),
		WrongTypeWriter.Objects.Num(), 0);

	// Battle4_01's first serialized ReachSpec starts at 6,680,366. Its Start
	// endpoint is the two-byte compact index 0x70,0x09 (export 624).  Retarget
	// exactly that Level-tail field to 0x7f,0x7f (export 8191, beyond this
	// package's 5613 exports) without shifting any following record.
	if (!TestTrue(TEXT("Battle4_01 retail pin contains the ReachSpec endpoint"),
		Sources[18].Bytes.IsValidIndex(6680371)
			&& Sources[18].Bytes[6680370] == 0x70
			&& Sources[18].Bytes[6680371] == 0x09)) return false;
	Sources[18].Bytes[6680370] = 0x7f;
	Sources[18].Bytes[6680371] = 0x7f;
	FWoTBSPWorldSpecWriter CorruptWriter;
	FWoTBSPWorldSpecProgress CorruptProgress;
	TWoTBootstrapResult<FWoTWorldSetResult> Corrupt =
		ImportRequiredWorldSetPackageBytesForTesting(
			Sources, Complete, CorruptWriter, CorruptProgress);
	TestTrue(TEXT("a corrupt nineteenth ReachSpec returns no world set"),
		Corrupt.IsError());
	TestEqual(TEXT("ReachSpec discovery rejects before the first write"),
		CorruptWriter.Objects.Num(), 0);

	// Restore the ReachSpec, then make the nineteenth map's empty TravelInfo
	// claim one key/value pair. The following byte is the empty v2
	// destroy-observer map, so the first key decodes as empty and its value is
	// necessarily truncated at the exact Level export boundary.
	Sources[18].Bytes[6680370] = 0x70;
	Sources[18].Bytes[6680371] = 0x09;
	if (!TestTrue(TEXT("Battle4_01 retail pin contains the TravelInfo count"),
		Sources[18].Bytes.IsValidIndex(6761953)
			&& Sources[18].Bytes[6761952] == 0x00
			&& Sources[18].Bytes[6761953] == 0x00)) return false;
	Sources[18].Bytes[6761952] = 0x01;
	FWoTBSPWorldSpecWriter TailWriter;
	FWoTBSPWorldSpecProgress TailProgress;
	TWoTBootstrapResult<FWoTWorldSetResult> TailCorrupt =
		ImportRequiredWorldSetPackageBytesForTesting(
			Sources, Complete, TailWriter, TailProgress);
	TestTrue(TEXT("a corrupt nineteenth serialized Level tail returns no world set"),
		TailCorrupt.IsError());
	TestEqual(TEXT("Level-tail discovery rejects before the first write"),
		TailWriter.Objects.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldAttachmentCycleSpec,
	"WoT.RetailReader.BSPWorld.MoverAttachmentCycles", WoTBSPWorldSpecFlags)

bool FWoTBSPWorldAttachmentCycleSpec::RunTest(const FString&)
{
	const int32 Acyclic[] = {-1, 0, 1};
	FWoTBootstrapStatus Accepted = ValidateWorldAttachmentsForTesting(Acyclic);
	TestFalse(TEXT("bounded mover attachment chain validates"), Accepted.IsError());
	const int32 Cyclic[] = {1, 2, 0};
	FWoTBootstrapStatus Rejected = ValidateWorldAttachmentsForTesting(Cyclic);
	TestTrue(TEXT("mover attachment cycle rejects"), Rejected.IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldEntryWeatherBudgetSpec,
	"WoT.RetailReader.BSPWorld.EntryWeatherPairBudgetPreflight",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldEntryWeatherBudgetSpec::RunTest(const FString&)
{
	const TArray<uint64> ValidKeys = {1, 2};
	const TArray<uint32> ValidBytes = {1024, 2048};
	TestTrue(TEXT("bounded unique entry-weather pairs pass preflight"),
		ValidateEntryWeatherPairBudgetForTesting(ValidKeys, ValidBytes));

	const TArray<uint64> DuplicateKeys = {1, 1};
	TestFalse(TEXT("duplicate entry-weather routing rejects before illumination"),
		ValidateEntryWeatherPairBudgetForTesting(DuplicateKeys, ValidBytes));

	TArray<uint64> ExcessKeys;
	TArray<uint32> ExcessBytes;
	ExcessKeys.Reserve(4097);
	ExcessBytes.Reserve(4097);
	for (uint32 Index = 0; Index < 4097; ++Index)
	{
		ExcessKeys.Add(Index);
		ExcessBytes.Add(1);
	}
	TestFalse(TEXT("excess entry-weather pair count rejects before illumination"),
		ValidateEntryWeatherPairBudgetForTesting(ExcessKeys, ExcessBytes));

	const TArray<uint64> AggregateKeys = {1, 2};
	const TArray<uint32> AggregateBytes = {3u * 1024u * 1024u,
		2u * 1024u * 1024u};
	TestFalse(TEXT("excess entry-weather byte work rejects before illumination"),
		ValidateEntryWeatherPairBudgetForTesting(AggregateKeys, AggregateBytes));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldSingleRowShadowFilterSpec,
	"WoT.RetailReader.BSPWorld.SingleRowShadowFilterAsymmetry",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldSingleRowShadowFilterSpec::RunTest(const FString&)
{
	const TArray<uint8> OneFullyLitRow = {0xff};
	TArray<uint8> Filtered;
	if (!TestTrue(TEXT("the bounded shadow-filter seam accepts one padded row"),
		FilterWorldShadowMaskForTesting(OneFullyLitRow, 1, 1, Filtered)))
	{
		return false;
	}
	TestEqual(TEXT("one padded shadow row expands to eight texels"),
		Filtered.Num(), 8);
	for (int32 Texel = 0; Texel < Filtered.Num(); ++Texel)
	{
		TestEqual(FString::Printf(TEXT("single-row texel %d has only top+middle rows"),
			Texel), Filtered[Texel], static_cast<uint8>(184));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldSyntheticMoverLightmapSpec,
	"WoT.RetailReader.BSPWorld.SyntheticMoverLightmap",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldSyntheticMoverLightmapSpec::RunTest(const FString&)
{
	uint8 FilteredShadow = 0;
	uint8 Illumination = 0;
	FIntVector RGB7 = FIntVector::ZeroValue;
	if (!TestTrue(TEXT("one-poly one-light mover composition succeeds"),
		ComposeSyntheticMoverLightmapForTesting(
			FilteredShadow, Illumination, RGB7))) return false;
	// Hand derivation (mirrored in test_mover_lightmaps.py): the one set bit's
	// asymmetric one-row filter is 133. Radius 25, distance/diffuse 10/25 and
	// LightSqrt[654]=1.6208408 round to illumination 86. Hue 0, saturation 0,
	// brightness 64 gives fixed RGB (13509,0,0), hence (86*13509)>>16 = 17.
	TestEqual(TEXT("one-bit mover shadow filters to 133"),
		FilteredShadow, static_cast<uint8>(133));
	TestEqual(TEXT("radial mover illumination rounds to 86"),
		Illumination, static_cast<uint8>(86));
	TestEqual(TEXT("composed mover texel is hand-derived RGB7 (17,0,0)"),
		RGB7, FIntVector(17, 0, 0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldSyntheticMoverAnimationSpec,
	"WoT.RetailReader.P11.MoverAnimationResponse",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldSyntheticMoverAnimationSpec::RunTest(const FString&)
{
	uint8 StaticMean = 0, StaticResponse = 0;
	bool bStaticAnimated = true;
	if (!TestTrue(TEXT("steady mover response composes"),
		ComposeSyntheticMoverAnimationResponseForTesting(
			1, StaticMean, StaticResponse, bStaticAnimated))) return false;
	TestFalse(TEXT("steady light does not mark a mover polygon animated"),
		bStaticAnimated);
	TestEqual(TEXT("steady light has no centered response"),
		StaticResponse, static_cast<uint8>(0));

	uint8 PulseMean = 0, PulseResponse = 0;
	bool bPulseAnimated = false;
	if (!TestTrue(TEXT("pulse mover response composes"),
		ComposeSyntheticMoverAnimationResponseForTesting(
			2, PulseMean, PulseResponse, bPulseAnimated))) return false;
	TestTrue(TEXT("pulse light marks its mover polygon animated"), bPulseAnimated);
	TestTrue(TEXT("pulse light has a nonzero centered response"),
		PulseResponse > 0);

	uint8 BlinkMean = 0, BlinkResponse = 0;
	bool bBlinkAnimated = false;
	if (!TestTrue(TEXT("blink mover response composes"),
		ComposeSyntheticMoverAnimationResponseForTesting(
			3, BlinkMean, BlinkResponse, bBlinkAnimated))) return false;
	TestTrue(TEXT("blink light marks its mover polygon animated"), bBlinkAnimated);
	TestTrue(TEXT("blink light has a nonzero centered response"),
		BlinkResponse > 0);
	TestEqual(TEXT("steady mean remains the full static contribution"),
		StaticMean, static_cast<uint8>(64));
	TestEqual(TEXT("pulse atlas remains frozen at the retail mean"),
		PulseMean, static_cast<uint8>(38));
	TestEqual(TEXT("blink atlas remains frozen at the retail mean"),
		BlinkMean, static_cast<uint8>(32));

	uint8 AnimatedOnly = 0, WorldMixed = 0, MoverMixed = 0;
	if (!TestTrue(TEXT("mixed static/Blink production accumulator composes"),
		ComposeSyntheticMixedMoverResponseForTesting(
			AnimatedOnly, WorldMixed, MoverMixed))) return false;
	TestTrue(TEXT("Blink alone produces a nonzero response"),
		AnimatedOnly > 0);
	TestEqual(TEXT("world composition clamps both mixed endpoints before differencing"),
		WorldMixed, static_cast<uint8>(0));
	TestEqual(TEXT("mover composition matches saturated world response"),
		MoverMixed, WorldMixed);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldLargeMoverAtlasSpec,
	"WoT.RetailReader.BSPWorld.LargeMoverAtlas",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldLargeMoverAtlasSpec::RunTest(const FString&)
{
	FIntPoint Size = FIntPoint::ZeroValue;
	TestTrue(TEXT("five maximum mover maps pack within the wire bound"),
		BuildLargeMoverAtlasForTesting(Size));
	TestEqual(TEXT("the first fitting mover-atlas width"), Size.X, 4096);
	TestEqual(TEXT("the first fitting mover-atlas height"), Size.Y, 4096);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldLightPolicySpec,
	"WoT.RetailReader.BSPWorld.LightDefaultsAndSkipPolicy", WoTBSPWorldSpecFlags)

bool FWoTBSPWorldLightPolicySpec::RunTest(const FString&)
{
	TestEqual(TEXT("positive exported half tie rounds away from zero"),
		ProjectWorldLightLocationForTesting(2006.0078125), 2006.007813);
	TestEqual(TEXT("Arena_09 Light129 negative half tie rounds away from zero"),
		ProjectWorldLightLocationForTesting(-12197.0078125), -12197.007813);

	struct FDefaultsCase
	{
		const TCHAR* Class;
		uint8 Type;
		uint8 Effect;
		uint8 Brightness;
		uint8 Hue;
		uint8 Saturation;
		uint8 Radius;
		uint8 Cone;
		bool bSunRadius;
	};
	const FDefaultsCase Cases[] = {
		{TEXT("Light"), 1, 0, 64, 0, 255, 64, 128, false},
		{TEXT("ParticleSprayer"), 0, 0, 0, 0, 0, 0, 0, false},
		{TEXT("Flame01"), 0, 0, 0, 0, 0, 0, 0, false},
		{TEXT("SunLight"), 1, 0, 255, 0, 255, 64, 128, true},
		{TEXT("PitOpening"), 0, 0, 0, 0, 0, 0, 0, false},
		{TEXT("LampFlame01"), 0, 0, 0, 0, 0, 0, 0, false},
		{TEXT("Cylinder"), 0, 0, 0, 0, 0, 0, 0, false},
		{TEXT("PixieSprayerBlue"), 1, 13, 100, 140, 64, 25, 0, false},
		{TEXT("Spotlight"), 1, 12, 64, 0, 255, 64, 128, false},
		{TEXT("twotriangles"), 0, 0, 0, 0, 0, 0, 0, false},
	};
	for (const FDefaultsCase& Expected : Cases)
	{
		uint8 Type = 255, Effect = 255, Brightness = 255, Hue = 255;
		uint8 Saturation = 255, Radius = 255, Cone = 255;
		bool bSunRadius = false;
		const FString Label(Expected.Class);
		TestTrue(Label + TEXT(" has a reviewed defaults row"),
			ResolveWorldLightDefaultsForTesting(Expected.Class, Type, Effect,
				Brightness, Hue, Saturation, Radius, Cone, bSunRadius));
		TestEqual(Label + TEXT(" type"), Type, Expected.Type);
		TestEqual(Label + TEXT(" effect"), Effect, Expected.Effect);
		TestEqual(Label + TEXT(" brightness"), Brightness, Expected.Brightness);
		TestEqual(Label + TEXT(" hue"), Hue, Expected.Hue);
		TestEqual(Label + TEXT(" saturation"), Saturation, Expected.Saturation);
		TestEqual(Label + TEXT(" radius"), Radius, Expected.Radius);
		TestEqual(Label + TEXT(" cone"), Cone, Expected.Cone);
		TestEqual(Label + TEXT(" sunlight multiplier"), bSunRadius,
			Expected.bSunRadius);
	}
	uint8 Byte = 0;
	bool bSunRadius = false;
	TestFalse(TEXT("unknown emitter has no implicit zero-default row"),
		ResolveWorldLightDefaultsForTesting(TEXT("UnknownEmitter"), Byte, Byte,
			Byte, Byte, Byte, Byte, Byte, bSunRadius));

	struct FSkipCase
	{
		const TCHAR* Label;
		bool bActorPresent;
		bool bRecognizedClass;
		bool bLightningOff;
		uint8 Type;
		uint8 Brightness;
		uint8 Effect;
		bool bMaskInRange;
		const TCHAR* Reason;
	};
	const FSkipCase Skips[] = {
		{TEXT("missing"), false, false, false, 1, 1, 0, true, TEXT("missing_actor")},
		{TEXT("class"), true, false, false, 1, 1, 0, true, TEXT("unsupported_class")},
		{TEXT("lightning"), true, true, true, 1, 1, 0, true, TEXT("lightning_off")},
		{TEXT("type"), true, true, false, 4, 1, 0, true, TEXT("type:LT_Flicker")},
		{TEXT("brightness"), true, true, false, 1, 0, 0, true, TEXT("brightness_0")},
		{TEXT("effect"), true, true, false, 1, 1, 6, true, TEXT("effect:LE_FastWave")},
		{TEXT("mask"), true, true, false, 1, 1, 0, false, TEXT("mask_out_of_range")},
		{TEXT("accepted"), true, true, false, 7, 1, 3, true, TEXT("")},
	};
	TMap<FString, uint32> Counts;
	for (const FSkipCase& Skip : Skips)
	{
		const FString Reason = ClassifyWorldLightSkipForTesting(
			Skip.bActorPresent, Skip.bRecognizedClass, Skip.bLightningOff,
			Skip.Type, Skip.Brightness, Skip.Effect, Skip.bMaskInRange);
		TestEqual(Skip.Label, Reason, FString(Skip.Reason));
		if (!Reason.IsEmpty()) ++Counts.FindOrAdd(Reason);
	}
	TestEqual(TEXT("synthetic skip policy exposes one count per reason"),
		Counts.Num(), 7);
	for (const TPair<FString, uint32>& Count : Counts)
		TestEqual(Count.Key + TEXT(" count"), Count.Value, 1u);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldAtlasCarrierPolicySpec,
	"WoT.RetailReader.BSPWorld.ComposedAtlasRequiresExactMainCarrier",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldAtlasCarrierPolicySpec::RunTest(const FString&)
{
	const TArray<FString> ProceduralOnly = {
		TEXT("AllProcedural_Fractal_000"),
		TEXT("AllProcedural_Fractal_001")};
	const FWoTBootstrapStatus Missing = ValidateWorldAtlasCarrierForTesting(
		true, TEXT("AllProcedural"), ProceduralOnly);
	if (TestTrue(TEXT("a composed atlas without the exact main carrier fails"),
		Missing.IsError()))
	{
		TestTrue(TEXT("missing atlas carrier failure is diagnostic"),
			Missing.GetError().ToString().Contains(TEXT("no exact main part carrier")));
	}
	const TArray<FString> WithMain = {
		TEXT("AllProcedural"), TEXT("AllProcedural_Fractal_000")};
	TestFalse(TEXT("a composed atlas accepts its exact main carrier"),
		ValidateWorldAtlasCarrierForTesting(
			true, TEXT("AllProcedural"), WithMain).IsError());
	TestFalse(TEXT("an atlas-free procedural set needs no carrier"),
		ValidateWorldAtlasCarrierForTesting(
			false, TEXT("AllProcedural"), ProceduralOnly).IsError());
	TestTrue(TEXT("the empty-part encode path cannot bypass carrier validation"),
		EmptyWorldBSPEncodingFailsForComposedAtlasForTesting(true));
	TestFalse(TEXT("the empty-part encode path remains valid without an atlas"),
		EmptyWorldBSPEncodingFailsForComposedAtlasForTesting(false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldSectionIdentityBoundSpec,
	"WoT.RetailReader.BSPWorld.SectionIdentitySharedByteBound",
	WoTBSPWorldSpecFlags)

bool FWoTBSPWorldSectionIdentityBoundSpec::RunTest(const FString&)
{
	TestTrue(TEXT("four identity fields admit the shared 1024-byte cap"),
		WorldBSPSectionIdentityLengthsValidForTesting({1024, 1024, 1024, 1024}));
	for (int32 Field = 0; Field < 4; ++Field)
	{
		TArray<int32> Lengths = {1, 1, 1, 0};
		Lengths[Field] = 1025;
		TestFalse(FString::Printf(TEXT("identity field %d rejects 1025 bytes"), Field),
			WorldBSPSectionIdentityLengthsValidForTesting(Lengths));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPWorldSyntheticVectorSpec,
	"WoT.RetailReader.BSPWorld.SyntheticNineteenVectorMatrix", WoTBSPWorldSpecFlags)

bool FWoTBSPWorldSyntheticVectorSpec::RunTest(const FString&)
{
	TArray<uint8> FixtureBytes;
	if (!TestTrue(TEXT("synthetic fixture bytes load"),
		WoTBSPWorldSpecFixtureBytes(FixtureBytes))) return false;
	TWoTBootstrapResult<FWoTWorldSetResult> NativeBaseline =
		ValidateWorldSemanticFixtureForTesting(FixtureBytes);
	if (!TestFalse(TEXT("production semantic validator accepts the baseline"),
		NativeBaseline.IsError())) return false;
	TestEqual(TEXT("semantic baseline returns all nineteen maps"),
		NativeBaseline.GetValue().Worlds.Num(), 19);
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Fixture = WoTBSPWorldSpecFixture();
	if (!TestFalse(TEXT("synthetic map-set fixture is canonical"), Fixture.IsError()))
		return false;
	const FWoTCanonicalJsonValue* Maps = Fixture.GetValue().FindMember(TEXT("maps"));
	if (!TestNotNull(TEXT("fixture has maps"), Maps)) return false;
	TestEqual(TEXT("fixture has nineteen distinct map records"), Maps->Num(), 19);
	TSet<FString> Identities;
	for (int32 MapIndex = 0; MapIndex < Maps->Num(); ++MapIndex)
	{
		const FWoTCanonicalJsonValue& Map = Maps->GetElement(MapIndex);
		const TCHAR* MapKeys[] = {TEXT("actors"), TEXT("attachments"), TEXT("bsp"),
			TEXT("identity"), TEXT("lights"), TEXT("map_root"),
			TEXT("navigation_seeds"), TEXT("sky_zone"), TEXT("sounds")};
		TOptional<FWoTBootstrapError> MapMembers = RequireExactMembers(
			Map, MakeArrayView(MapKeys), TEXT("synthetic world"));
		TestFalse(TEXT("world header member set is exact"), MapMembers.IsSet());
		const FWoTCanonicalJsonValue* Identity = Map.FindMember(TEXT("identity"));
		Identities.Add(Identity->GetString());
		const FWoTCanonicalJsonValue* Actors = Map.FindMember(TEXT("actors"));
		const FWoTCanonicalJsonValue* Attachments = Map.FindMember(TEXT("attachments"));
		const FWoTCanonicalJsonValue* BSP = Map.FindMember(TEXT("bsp"));
		const FWoTCanonicalJsonValue* Lights = Map.FindMember(TEXT("lights"));
		const FWoTCanonicalJsonValue* Sounds = Map.FindMember(TEXT("sounds"));
		const FWoTCanonicalJsonValue* Navigation =
			Map.FindMember(TEXT("navigation_seeds"));
		TestTrue(TEXT("actor property/reference vector is populated"),
			Actors != nullptr && Actors->IsArray() && Actors->Num() >= 7);
		bool bSawMover = false;
		if (Actors != nullptr)
		{
			for (int32 ActorIndex = 0; ActorIndex < Actors->Num(); ++ActorIndex)
			{
				const FWoTCanonicalJsonValue& Actor = Actors->GetElement(ActorIndex);
				const TCHAR* ActorKeys[] = {TEXT("class"), TEXT("identity"),
					TEXT("properties"), TEXT("references")};
				TOptional<FWoTBootstrapError> ActorMembers = RequireExactMembers(
					Actor, MakeArrayView(ActorKeys), TEXT("synthetic actor"));
				TestFalse(TEXT("actor header member set is exact"), ActorMembers.IsSet());
				bSawMover |= Actor.FindMember(TEXT("class"))->GetString() == TEXT("Mover");
			}
		}
		TestTrue(TEXT("synthetic actor registry contains a mover"), bSawMover);
		TestTrue(TEXT("mover attachment vector is populated"),
			Attachments != nullptr && Attachments->IsArray() && Attachments->Num() > 0);
		TestTrue(TEXT("light vector is populated"),
			Lights != nullptr && Lights->IsArray() && Lights->Num() > 0);
		TestTrue(TEXT("sound vector is populated"),
			Sounds != nullptr && Sounds->IsArray() && Sounds->Num() > 0);
		TestTrue(TEXT("navigation seed vector is populated"),
			Navigation != nullptr && Navigation->IsArray() && Navigation->Num() > 0);
		if (BSP != nullptr)
		{
			for (const TCHAR* Key : {TEXT("nodes"), TEXT("surfaces"), TEXT("zones"),
				TEXT("portals"), TEXT("collision_surfaces")})
			{
				const FWoTCanonicalJsonValue* Vector = BSP->FindMember(Key);
				TestTrue(FString(TEXT("BSP ")) + Key + TEXT(" vector is populated"),
					Vector != nullptr && Vector->IsArray() && Vector->Num() > 0);
			}
		}
		TestTrue(TEXT("sky/zone state is named"),
			Map.FindMember(TEXT("sky_zone"))->GetString().Len() > 0);
	}
	TestEqual(TEXT("all synthetic identities are distinct"), Identities.Num(), 19);
	struct FMutation
	{
		const ANSICHAR* Label;
		const ANSICHAR* Before;
		const ANSICHAR* After;
	};
	const FMutation Mutations[] = {
		{"dangling reference",
			"\"references\":[\"Arena_01.LevelInfo0\"]",
			"\"references\":[\"Arena_01.MissingActor\"]"},
		{"light wrong type",
			"\"lights\":[\"Arena_01.Light0\"]",
			"\"lights\":[\"Arena_01.Mover0\"]"},
		{"navigation wrong type",
			"\"navigation_seeds\":[\"Arena_01.WotPathNode0\",\"Arena_01.WotPathNode1\"]",
			"\"navigation_seeds\":[\"Arena_01.Light0\",\"Arena_01.WotPathNode1\"]"},
		{"sky wrong type",
			"\"sky_zone\":\"Arena_01.ZoneInfo0\"",
			"\"sky_zone\":\"Arena_01.Light0\""},
		{"sound actor wrong type",
			"\"sounds\":[{\"actor\":\"Arena_01.AmbientSound0\",\"asset\":\"AmbientA.Water.DIVE\"}]",
			"\"sounds\":[{\"actor\":\"Arena_01.Light0\",\"asset\":\"AmbientA.Water.DIVE\"}]"},
		{"collision surface out of range",
			"\"collision_surfaces\":[0]",
			"\"collision_surfaces\":[99]"},
		{"BSP node surface out of range",
			"\"nodes\":[{\"back\":-1,\"front\":-1,\"surface\":0,",
			"\"nodes\":[{\"back\":-1,\"front\":-1,\"surface\":99,"},
	};
	for (const FMutation& Mutation : Mutations)
	{
		TArray<uint8> Mutated = FixtureBytes;
		if (!TestTrue(FString(ANSI_TO_TCHAR(Mutation.Label))
			+ TEXT(" mutation applied"), WoTBSPWorldSpecReplaceOnce(
				Mutated, Mutation.Before, Mutation.After))) continue;
		TWoTBootstrapResult<FWoTWorldSetResult> Rejected =
			ValidateWorldSemanticFixtureForTesting(Mutated);
		TestTrue(FString(ANSI_TO_TCHAR(Mutation.Label))
			+ TEXT(" returns an error"), Rejected.IsError());
		// TWoTBootstrapResult has no value on error: therefore no descriptor
		// vector/set root can escape this semantic-validation seam.
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
