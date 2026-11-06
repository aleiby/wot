#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/FrontEndDescriptorSidecar.h"
#include "WoTRetailReader/FrontEndWorld.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTFrontEndSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::ProductFilter;

FString WoTFrontEndSpecRepoPath(const TCHAR* Relative)
{
	return WoTRepositoryOrReferencePath(Relative);
}

class FWoTFrontEndSpecProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

FWoTSha256 WoTFrontEndSpecSha(TCHAR Digit)
{
	const FString Hex = FString::ChrN(64, Digit);
	TWoTBootstrapResult<FWoTSha256> Parsed = FWoTSha256::FromHex(Hex);
	check(!Parsed.IsError());
	return MoveTemp(Parsed).GetValue();
}

int64 WoTFrontEndSpecDoubleBits(double Value)
{
	uint64 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return static_cast<int64>(Bits);
}

TArray<uint8> WoTFrontEndSpecUtf8(const FString& Value)
{
	FTCHARToUTF8 Encoded(*Value);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	return Bytes;
}

TArray<uint8> WoTFrontEndSpecResealLocationX(
	TConstArrayView<uint8> SealedBytes,
	const FWoTFrontEndDescriptor& Original,
	const FWoTFrontEndDescriptor& Mutated)
{
	TArray<uint8> NullTerminated(SealedBytes);
	NullTerminated.Add(0);
	FString Json(UTF8_TO_TCHAR(
		reinterpret_cast<const ANSICHAR*>(NullTerminated.GetData())));
	const FString OriginalLocation = FString::Printf(
		TEXT("\"location_bits\":[%lld"),
		WoTFrontEndSpecDoubleBits(Original.CameraLocation.X));
	const FString MutatedLocation = FString::Printf(
		TEXT("\"location_bits\":[%lld"),
		WoTFrontEndSpecDoubleBits(Mutated.CameraLocation.X));
	check(Json.ReplaceInline(*OriginalLocation, *MutatedLocation,
		ESearchCase::CaseSensitive) == 1);
	const FString OriginalRoot = FString::Printf(
		TEXT("\"descriptor_root\":\"%s\""),
		*Original.DescriptorRoot.ToHex());
	const FString MutatedRoot = FString::Printf(
		TEXT("\"descriptor_root\":\"%s\""),
		*Mutated.DescriptorRoot.ToHex());
	check(Json.ReplaceInline(*OriginalRoot, *MutatedRoot,
		ESearchCase::CaseSensitive) == 1);

	const FString RecordPrefix = TEXT(",\"record_sha256\":\"");
	const int32 RecordField = Json.Find(RecordPrefix, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd);
	check(RecordField != INDEX_NONE);
	const int32 RecordValue = RecordField + RecordPrefix.Len();
	check(Json.Len() >= RecordValue + 65);
	const FString UnsignedJson = Json.Left(RecordField) + TEXT("}");
	const TArray<uint8> UnsignedBytes = WoTFrontEndSpecUtf8(UnsignedJson);
	FWoTSha256Hasher Hasher;
	static const ANSICHAR Domain[] =
		"WOT-FRONT-END-DESCRIPTOR-SIDECAR-V2";
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Domain), UE_ARRAY_COUNT(Domain)));
	Hasher.Update(UnsignedBytes);
	Json = Json.Left(RecordValue) + Hasher.Finish().ToHex()
		+ Json.Mid(RecordValue + 64);
	return WoTFrontEndSpecUtf8(Json);
}

struct FWoTFrontEndSpecResource
{
	EWoTFactDomain Domain;
	const TCHAR* SourceExport;
};

// Independent, named expectation for the complete front-end closure. BSP/world
// are asserted separately; these are the 46 typed resources they require.
const FWoTFrontEndSpecResource WoTFrontEndSpecResources[] = {
	{EWoTFactDomain::Animation, TEXT("Environment.RainCyd")},
	{EWoTFactDomain::Font, TEXT("WOT.F_WOTReg14")},
	{EWoTFactDomain::Font, TEXT("WOT.F_WOTReg30")},
	{EWoTFactDomain::Localization, TEXT("WoT.int")},
	{EWoTFactDomain::Mesh, TEXT("Environment.RainCyd")},
	{EWoTFactDomain::Procedural, TEXT("JHPFireEng.M01ModWet")},
	{EWoTFactDomain::Procedural, TEXT("WarrenT.Rain2")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.DIVE")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.Stream6")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.Stream_1")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.Trickle3")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.WaterFll5")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Wind.Wind")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder10")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder2")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder3")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder4")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder8")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder9")},
	{EWoTFactDomain::Sound, TEXT("WOT.MoveSelector")},
	{EWoTFactDomain::Sound, TEXT("WOT.Select")},
	{EWoTFactDomain::Texture, TEXT("Environment.Skins.RainDropsA")},
	{EWoTFactDomain::Texture, TEXT("ForsakenT.DetailArchitect.MarbBalc2720")},
	{EWoTFactDomain::Texture, TEXT("ForsakenT.Dev.TypeMask_003sky")},
	{EWoTFactDomain::Texture, TEXT("ForsakenT.SurfaceFloor.MarblStep2362")},
	{EWoTFactDomain::Texture, TEXT("JamesT.flare_blue01")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.MetlGril4002v02")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.StonArch4348")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.StonTrim4353")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.StonWind4347v1")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceFloor.StonFlor4900")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.PlasWall4373v1")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonBlok4345")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonBlok4345v1")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonWall4018")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonWall4031v02")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonWall4333")},
	{EWoTFactDomain::Texture, TEXT("NatureT.Floor.DirtDirt_010")},
	{EWoTFactDomain::Texture, TEXT("NatureT.Sky.Sky_440")},
	{EWoTFactDomain::Texture, TEXT("NatureT.VerticalFace.StonFace_931")},
	{EWoTFactDomain::Texture, TEXT("ParticleSystems.Water.Splash02")},
	{EWoTFactDomain::Texture, TEXT("ScottT.mountain2")},
	{EWoTFactDomain::Texture, TEXT("WOT.F_WOTReg14.Texture9")},
	{EWoTFactDomain::Texture, TEXT("WOT.F_WOTReg30.Texture10")},
	{EWoTFactDomain::Texture, TEXT("WarrenT.Developer.Black")},
	{EWoTFactDomain::Texture, TEXT("WarrenT.MarcHigh.RckFlr_9011mt")},
};

const TCHAR* WoTFrontEndSpecDomainName(EWoTFactDomain Domain)
{
	switch (Domain)
	{
	case EWoTFactDomain::Texture: return TEXT("texture");
	case EWoTFactDomain::Sound: return TEXT("sound");
	case EWoTFactDomain::Mesh: return TEXT("mesh");
	case EWoTFactDomain::Animation: return TEXT("animation");
	case EWoTFactDomain::Font: return TEXT("font");
	case EWoTFactDomain::Procedural: return TEXT("procedural");
	case EWoTFactDomain::Localization: return TEXT("localization");
	default: return TEXT("invalid");
	}
}

FString WoTFrontEndSpecSourcePath(FStringView SourceExport)
{
	const FString Identity(SourceExport);
	if (Identity == TEXT("WoT.int")) return TEXT("System/WoT.int");
	if (Identity.StartsWith(TEXT("WOT."))) return TEXT("System/WOT.u");
	if (Identity.StartsWith(TEXT("Environment.")))
		return TEXT("System/Environment.u");
	if (Identity.StartsWith(TEXT("ParticleSystems.")))
		return TEXT("System/ParticleSystems.u");
	if (Identity.StartsWith(TEXT("AmbientA."))) return TEXT("Sounds/AmbientA.uax");
	int32 Dot = INDEX_NONE;
	check(Identity.FindChar(TEXT('.'), Dot));
	return TEXT("Textures/") + Identity.Left(Dot) + TEXT(".utx");
}

void WoTFrontEndSpecOutput(FWoTRequiredFact& Fact,
	const TCHAR* LogicalId, const TCHAR* Schema)
{
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = LogicalId;
	Output.Schema = Schema;
	Output.MaxDecodedSize = 1024 * 1024;
}

FWoTRetailContract WoTFrontEndSpecContract()
{
	FWoTRetailContract Contract;
	Contract.ContractId = TEXT("front-end-test-contract");
	auto AddFact = [&Contract](EWoTFactDomain Domain, const TCHAR* FactId,
		const TCHAR* SourcePath, const TCHAR* SourceExport,
		const TCHAR* LogicalId, const TCHAR* Schema)
	{
		FWoTRequiredFact& Fact = Contract.Facts.AddDefaulted_GetRef();
		Fact.Domain = Domain;
		Fact.FactId = FactId;
		Fact.SourcePath = SourcePath;
		Fact.SourceExport = SourceExport;
		Fact.Importer = TEXT("front-end-test-importer");
		Fact.RuntimeConsumer = TEXT("front-end-test-consumer");
		Fact.AuditRule = TEXT("front-end-test-audit");
		WoTFrontEndSpecOutput(Fact, LogicalId, Schema);
	};
	AddFact(EWoTFactDomain::BSP, TEXT("retail.bsp.WheelOfTime"),
		TEXT("Maps/WheelOfTime.wot"), TEXT("WheelOfTime.Model"),
		TEXT("cache/bsp/WheelOfTime/WheelOfTime"), TEXT("wot-bsp-v3"));
	AddFact(EWoTFactDomain::World, TEXT("retail.world.WheelOfTime"),
		TEXT("Maps/WheelOfTime.wot"), TEXT("WheelOfTime.Level"),
		TEXT("cache/world/WheelOfTime"), TEXT("wot-world-v3"));
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(WoTFrontEndSpecResources); ++Index)
	{
		const FWoTFrontEndSpecResource& Resource = WoTFrontEndSpecResources[Index];
		const FString Domain = WoTFrontEndSpecDomainName(Resource.Domain);
		AddFact(Resource.Domain, *FString::Printf(TEXT("front-end.%s.%d"),
			*Domain, Index), *WoTFrontEndSpecSourcePath(Resource.SourceExport),
			Resource.SourceExport,
			*FString::Printf(TEXT("cache/%s/front-end/%02d"), *Domain, Index),
			*FString::Printf(TEXT("wot-%s-v3"), *Domain));
	}
	return Contract;
}

TWoTBootstrapResult<FWoTRetailContract> WoTFrontEndSpecProductionContract()
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	const FString Path = WoTFrontEndSpecRepoPath(
		TEXT("Build/WoTRelease/cache-contract-v3-production.json"));
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the production contract fixture is absent"), Path));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Envelope = ParseCanonicalJson(Bytes);
	if (Envelope.IsError()) return FResult::Error(MoveTemp(Envelope).GetError());
	const FWoTCanonicalJsonValue* Payload =
		Envelope.GetValue().FindMember(TEXT("payload"));
	if (Payload == nullptr)
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("the production contract fixture has no payload"), Path));
	TWoTBootstrapResult<TArray<uint8>> PayloadBytes = WriteCanonicalJson(*Payload);
	if (PayloadBytes.IsError())
		return FResult::Error(MoveTemp(PayloadBytes).GetError());
	return ParseRetailContractPayload(PayloadBytes.GetValue());
}

FString WoTFrontEndSpecFixture()
{
	FString Actors;
	FString References;
	auto AddActors = [&Actors](const TCHAR* ClassName, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!Actors.IsEmpty()) Actors += TEXT(",");
			Actors += FString::Printf(
				TEXT("{\"class\":\"%s\",\"identity\":\"WheelOfTime.%s%d\"}"),
				ClassName, ClassName, Index);
		}
	};
	AddActors(TEXT("LevelInfo"), 1);
	AddActors(TEXT("Brush"), 76);
	AddActors(TEXT("PlayerStart"), 1);
	AddActors(TEXT("WOTZoneInfo"), 1);
	AddActors(TEXT("Light"), 39);
	AddActors(TEXT("ParticleSprayer"), 4);
	AddActors(TEXT("AmbientSound"), 5);
	AddActors(TEXT("RandomLightning"), 1);
	AddActors(TEXT("Lightning"), 5);
	AddActors(TEXT("WaterZone"), 1);
	AddActors(TEXT("SkyZoneInfo"), 1);
	AddActors(TEXT("RainCyd"), 6);
	for (const FWoTFrontEndSpecResource& Resource : WoTFrontEndSpecResources)
	{
		if (!References.IsEmpty()) References += TEXT(",");
		References += FString::Printf(
			TEXT("{\"domain\":\"%s\",\"identity\":\"%s\",\"kind\":\"resource\"}"),
			WoTFrontEndSpecDomainName(Resource.Domain), Resource.SourceExport);
	}
	return FString::Printf(TEXT(
		"{\"actors\":[%s],"
		"\"camera\":{\"horizontal_fov\":90,"
		"\"location\":[\"-412.484130859375\",\"-423.762054443359375\","
		"\"27.020946502685547\"],"
		"\"rotation\":[\"16.4794921875\",\"59.853515625\",\"0\"]},"
		"\"format\":\"wot-front-end-semantic-fixture-v1\","
		"\"map_name\":\"WheelOfTime\","
		"\"references\":[%s],"
		"\"thunder_sound_source_exports\":["
		"\"Environment.Thunder.Thunder9\",\"Environment.Thunder.Thunder3\","
		"\"Environment.Thunder.Thunder10\",\"Environment.Thunder.Thunder2\","
		"\"Environment.Thunder.Thunder4\",\"Environment.Thunder.Thunder8\","
		"\"Environment.Thunder.Thunder8\",\"Environment.Thunder.Thunder2\","
		"\"Environment.Thunder.Thunder3\",\"Environment.Thunder.Thunder10\"]}"),
		*Actors, *References);
}

TArray<uint8> WoTFrontEndSpecBytes(const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

bool WoTFrontEndSpecReplace(FString& Text,
	const TCHAR* From, const TCHAR* To)
{
	const int32 Before = Text.Len();
	Text = Text.Replace(From, To, ESearchCase::CaseSensitive);
	return Text.Len() != Before || FString(From).Len() == FString(To).Len();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndDescriptorSpec,
	"WoT.RetailReader.FrontEndWorld.DescriptorAndCensus", WoTFrontEndSpecFlags)

bool FWoTFrontEndDescriptorSpec::RunTest(const FString&)
{
	using FBuildSignature = TWoTBootstrapResult<FWoTFrontEndDescriptor> (*)(
		const FWoTRetailPackage&, const FWoTRetailContract&, IWoTProgressSink&);
	FBuildSignature ProductionEntryPoint = &BuildFrontEndDescriptor;
	// TestNotNull takes a data pointer; only MSVC converts a function pointer.
	TestTrue(TEXT("authenticated package entry point"), ProductionEntryPoint != nullptr);

	FWoTRetailContract Contract = WoTFrontEndSpecContract();
	TArray<uint8> Fixture = WoTFrontEndSpecBytes(WoTFrontEndSpecFixture());
	TWoTBootstrapResult<FWoTFrontEndDescriptor> Descriptor =
		BuildFrontEndDescriptorSemanticFixtureForTesting(Fixture, Contract);
	if (!TestFalse(TEXT("valid entry fixture"), Descriptor.IsError())) return false;
	const FWoTFrontEndDescriptor& Value = Descriptor.GetValue();
	TestEqual(TEXT("entry identity"), Value.MapName, FString(TEXT("WheelOfTime")));
	TestEqual(TEXT("BSP identity"), Value.BspLogicalId,
		FString(TEXT("cache/bsp/WheelOfTime/WheelOfTime")));
	TestEqual(TEXT("world identity"), Value.WorldLogicalId,
		FString(TEXT("cache/world/WheelOfTime")));
	TestEqual(TEXT("camera X"), Value.CameraLocation.X, -412.484130859375);
	TestEqual(TEXT("camera Y"), Value.CameraLocation.Y, -423.762054443359375);
	TestEqual(TEXT("eye-adjusted camera Z"), Value.CameraLocation.Z,
		27.020946502685547);
	TestEqual(TEXT("camera pitch degrees"), Value.CameraRotation.Pitch,
		16.4794921875);
	TestEqual(TEXT("camera yaw degrees"), Value.CameraRotation.Yaw,
		59.853515625);
	TestEqual(TEXT("camera roll degrees"), Value.CameraRotation.Roll, 0.0);
	TestEqual(TEXT("camera FOV"), Value.HorizontalFov, 90.0f);
	TestEqual(TEXT("lights"), Value.Census.Lights, 39u);
	TestEqual(TEXT("ambience"), Value.Census.AmbientSounds, 5u);
	TestEqual(TEXT("splash emitters"), Value.Census.SplashEmitters, 4u);
	TestEqual(TEXT("rain cylinders"), Value.Census.RainCylinders, 6u);
	TestEqual(TEXT("lightning actors"), Value.Census.LightningActors, 5u);
	TestEqual(TEXT("lightning controller"),
		Value.Census.RandomLightningControllers, 1u);
	const TArray<FString> EffectiveThunder = {
		TEXT("Environment.Thunder.Thunder9"), TEXT("Environment.Thunder.Thunder3"),
		TEXT("Environment.Thunder.Thunder10"), TEXT("Environment.Thunder.Thunder2"),
		TEXT("Environment.Thunder.Thunder4"), TEXT("Environment.Thunder.Thunder8"),
		TEXT("Environment.Thunder.Thunder8"), TEXT("Environment.Thunder.Thunder2"),
		TEXT("Environment.Thunder.Thunder3"), TEXT("Environment.Thunder.Thunder10")};
	TestEqual(TEXT("effective thunder order retains valid repeated identities"),
		Value.ThunderSoundSourceExports, EffectiveThunder);
	TestEqual(TEXT("typed resource closure size"), Value.RequiredResources.Num(), 48);
	TestEqual(TEXT("logical resource closure size"), Value.RequiredLogicalIds.Num(), 48);
	TestEqual(TEXT("resource closure sorted first"), Value.RequiredLogicalIds[0],
		FString(TEXT("cache/animation/front-end/00")));
	const FWoTFrontEndResourceRequirement* MenuFont =
		Value.RequiredResources.FindByPredicate(
			[](const FWoTFrontEndResourceRequirement& Resource)
			{
				return Resource.SourceExport == TEXT("WOT.F_WOTReg30");
			});
	if (TestNotNull(TEXT("menu font is named in the typed closure"), MenuFont))
		TestEqual(TEXT("menu font has the font domain"),
			MenuFont->Domain, EWoTFactDomain::Font);
	const FWoTFrontEndResourceRequirement* RainMesh =
		Value.RequiredResources.FindByPredicate(
			[](const FWoTFrontEndResourceRequirement& Resource)
			{
				return Resource.Domain == EWoTFactDomain::Mesh
					&& Resource.SourceExport == TEXT("Environment.RainCyd");
			});
	TestNotNull(TEXT("RainCyd mesh is named in the typed closure"), RainMesh);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndRejectionSpec,
	"WoT.RetailReader.FrontEndWorld.ClosedSemanticRejections", WoTFrontEndSpecFlags)

bool FWoTFrontEndRejectionSpec::RunTest(const FString&)
{
	const FWoTRetailContract Contract = WoTFrontEndSpecContract();
	const FString Good = WoTFrontEndSpecFixture();
	auto Rejects = [this, &Contract, &Good](const TCHAR* What,
		const TCHAR* From, const TCHAR* To)
	{
		FString Mutated = Good;
		TestTrue(FString(What) + TEXT(" mutation applied"),
			WoTFrontEndSpecReplace(Mutated, From, To));
		return TestTrue(What,
			BuildFrontEndDescriptorSemanticFixtureForTesting(
				WoTFrontEndSpecBytes(Mutated), Contract).IsError());
	};
	Rejects(TEXT("missing resource"), TEXT("Environment.Skins.RainDropsA"),
		TEXT("Environment.Skins.MissingRain"));
	Rejects(TEXT("duplicate resource"),
		TEXT("{\"domain\":\"sound\",\"identity\":\"WOT.Select\",\"kind\":\"resource\"}"),
		TEXT("{\"domain\":\"sound\",\"identity\":\"WOT.MoveSelector\",\"kind\":\"resource\"}"));
	Rejects(TEXT("extra resource"),
		TEXT("{\"domain\":\"sound\",\"identity\":\"WOT.Select\",\"kind\":\"resource\"}"),
		TEXT("{\"domain\":\"sound\",\"identity\":\"WOT.Select\",\"kind\":\"resource\"},"
			"{\"domain\":\"texture\",\"identity\":\"Entry.Unexpected\","
			"\"kind\":\"resource\"}"));
	for (const TCHAR* CampaignClass : {TEXT("PlayerPawn"), TEXT("Inventory"), TEXT("Mover")})
	{
		Rejects(*FString::Printf(TEXT("campaign class %s"), CampaignClass),
			TEXT("\"class\":\"Brush\""),
			*FString::Printf(TEXT("\"class\":\"%s\""), CampaignClass));
	}
	Rejects(TEXT("unclassified actor"), TEXT("\"class\":\"Brush\""),
		TEXT("\"class\":\"Decoration\""));
	Rejects(TEXT("unclassified reference"), TEXT("\"kind\":\"resource\""),
		TEXT("\"kind\":\"mystery\""));
	Rejects(TEXT("wrong map identity"), TEXT("\"map_name\":\"WheelOfTime\""),
		TEXT("\"map_name\":\"Arena_01\""));
	Rejects(TEXT("missing RandomLightning controller"),
		TEXT("\"class\":\"RandomLightning\""), TEXT("\"class\":\"Lightning\""));
	Rejects(TEXT("duplicate RandomLightning controller"),
		TEXT("\"class\":\"Lightning\""), TEXT("\"class\":\"RandomLightning\""));
	Rejects(TEXT("malformed camera"), TEXT("\"horizontal_fov\":90"),
		TEXT("\"horizontal_fov\":0"));
	Rejects(TEXT("missing ordered thunder slot"),
		TEXT(",\"Environment.Thunder.Thunder10\"]}"), TEXT("]}"));
	Rejects(TEXT("unknown ordered thunder identity"),
		TEXT("Environment.Thunder.Thunder10\"]}"),
		TEXT("Environment.Thunder.Unknown\"]}"));

	FWoTRetailContract Unsafe = Contract;
	Unsafe.Facts[0].Outputs[0].LogicalId = TEXT("../cache/bsp/entry");
	TestTrue(TEXT("unsafe logical ID"),
		BuildFrontEndDescriptorSemanticFixtureForTesting(
			WoTFrontEndSpecBytes(Good), Unsafe).IsError());

	FWoTRetailContract WrongDomain = Contract;
	FWoTRequiredFact* Skin = WrongDomain.Facts.FindByPredicate(
		[](const FWoTRequiredFact& Fact)
		{
			return Fact.SourceExport == TEXT("Environment.Skins.RainDropsA");
		});
	check(Skin != nullptr);
	Skin->Domain = EWoTFactDomain::Sound;
	TestTrue(TEXT("wrong-domain closure fact"),
		BuildFrontEndDescriptorSemanticFixtureForTesting(
			WoTFrontEndSpecBytes(Good), WrongDomain).IsError());

	FWoTRetailContract DuplicateFact = Contract;
	FWoTRequiredFact Duplicate = *DuplicateFact.Facts.FindByPredicate(
		[](const FWoTRequiredFact& Fact)
		{
			return Fact.SourceExport == TEXT("WOT.Select");
		});
	Duplicate.FactId += TEXT(".duplicate");
	Duplicate.Outputs[0].LogicalId += TEXT("-duplicate");
	DuplicateFact.Facts.Add(MoveTemp(Duplicate));
	TestTrue(TEXT("duplicate typed closure fact"),
		BuildFrontEndDescriptorSemanticFixtureForTesting(
			WoTFrontEndSpecBytes(Good), DuplicateFact).IsError());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndRetailReaderSpec,
	"WoT.RetailReader.FrontEndWorld.RealRetailEntryReader", WoTFrontEndSpecFlags)

bool FWoTFrontEndRetailReaderSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TWoTBootstrapResult<FWoTRetailContract> Contract =
		WoTFrontEndSpecProductionContract();
	if (!TestFalse(TEXT("production contract loads"), Contract.IsError())) return false;
	FWoTRetailContract EntryContract = MoveTemp(Contract).GetValue();
	TestEqual(TEXT("production contract owns one entry BSP fact"),
		EntryContract.Facts.FilterByPredicate([](const FWoTRequiredFact& Fact)
		{
			return Fact.SourcePath == TEXT("Maps/WheelOfTime.wot")
				&& Fact.Domain == EWoTFactDomain::BSP;
		}).Num(), 1);
	TestEqual(TEXT("production contract owns one entry world fact"),
		EntryContract.Facts.FilterByPredicate([](const FWoTRequiredFact& Fact)
		{
			return Fact.SourcePath == TEXT("Maps/WheelOfTime.wot")
				&& Fact.Domain == EWoTFactDomain::World;
		}).Num(), 1);
	TArray<uint8> Bytes;
	const FString MapPath = WoTFrontEndSpecRepoPath(
		TEXT("original/Maps/WheelOfTime.wot"));
	if (!FFileHelper::LoadFileToArray(Bytes, *MapPath))
	{
		AddError(TEXT("required original/Maps/WheelOfTime.wot is missing"));
		return false;
	}
	TArray<FWoTFrontEndSourceBytesForTesting> Sources;
	Sources.Add({TEXT("Maps/WheelOfTime.wot"), MoveTemp(Bytes)});
	for (const TCHAR* Relative : {
		TEXT("original/System/Environment.u"), TEXT("original/System/WOT.u")})
	{
		FWoTFrontEndSourceBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = FString(Relative).Replace(TEXT("original/"), TEXT(""));
		if (!FFileHelper::LoadFileToArray(Source.Bytes,
				*WoTFrontEndSpecRepoPath(Relative)))
		{
			AddError(FString::Printf(TEXT("required reference input is missing: %s"),
				Relative));
			return false;
		}
	}
	TWoTBootstrapResult<FWoTFrontEndDescriptor> Descriptor =
		BuildFrontEndDescriptorPackageBytesForTesting(Sources, EntryContract);
	if (!TestFalse(TEXT("real retail entry descriptor"), Descriptor.IsError()))
	{
		if (Descriptor.IsError()) AddError(Descriptor.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("real retail entry map"), Descriptor.GetValue().MapName,
		FString(TEXT("WheelOfTime")));
	TestEqual(TEXT("real retail lights"), Descriptor.GetValue().Census.Lights, 39u);
	TestEqual(TEXT("real retail ambience"),
		Descriptor.GetValue().Census.AmbientSounds, 5u);
	TestEqual(TEXT("real retail splash emitters"),
		Descriptor.GetValue().Census.SplashEmitters, 4u);
	TestEqual(TEXT("real retail rain"),
		Descriptor.GetValue().Census.RainCylinders, 6u);
	TestEqual(TEXT("real retail lightning"),
		Descriptor.GetValue().Census.LightningActors, 5u);
	TestEqual(TEXT("real retail lightning controller"),
		Descriptor.GetValue().Census.RandomLightningControllers, 1u);
	const TArray<FString> EffectiveThunder = {
		TEXT("Environment.Thunder.Thunder9"), TEXT("Environment.Thunder.Thunder3"),
		TEXT("Environment.Thunder.Thunder10"), TEXT("Environment.Thunder.Thunder2"),
		TEXT("Environment.Thunder.Thunder4"), TEXT("Environment.Thunder.Thunder8"),
		TEXT("Environment.Thunder.Thunder8"), TEXT("Environment.Thunder.Thunder2"),
		TEXT("Environment.Thunder.Thunder3"), TEXT("Environment.Thunder.Thunder10")};
	TestEqual(TEXT("real retail effective thunder order"),
		Descriptor.GetValue().ThunderSoundSourceExports, EffectiveThunder);
	TestEqual(TEXT("real retail exact typed closure"),
		Descriptor.GetValue().RequiredResources.Num(), 48);
	TestEqual(TEXT("real retail exact logical closure"),
		Descriptor.GetValue().RequiredLogicalIds.Num(), 48);
	for (const FWoTFrontEndSpecResource& Expected : WoTFrontEndSpecResources)
		TestNotNull(*FString::Printf(TEXT("real retail typed closure names %s"),
			Expected.SourceExport),
			Descriptor.GetValue().RequiredResources.FindByPredicate(
				[&Expected](const FWoTFrontEndResourceRequirement& Resource)
				{
					return Resource.Domain == Expected.Domain
						&& Resource.SourceExport == Expected.SourceExport;
				}));
	TestEqual(TEXT("real retail camera X bits"),
		Descriptor.GetValue().CameraLocation.X, -412.484130859375);
	TestEqual(TEXT("real retail camera Y bits"),
		Descriptor.GetValue().CameraLocation.Y, -423.762054443359375);
	TestEqual(TEXT("real retail eye-adjusted camera Z bits"),
		Descriptor.GetValue().CameraLocation.Z, 27.020946502685547);
	TestEqual(TEXT("real retail camera pitch degrees"),
		Descriptor.GetValue().CameraRotation.Pitch, 16.4794921875);
	TestEqual(TEXT("real retail camera yaw degrees"),
		Descriptor.GetValue().CameraRotation.Yaw, 59.853515625);
	TestEqual(TEXT("real retail camera roll degrees"),
		Descriptor.GetValue().CameraRotation.Roll, 0.0);
	for (const TPair<EWoTFrontEndPackageAttackForTesting, const TCHAR*>& Attack : {
		TPair<EWoTFrontEndPackageAttackForTesting, const TCHAR*>(
			EWoTFrontEndPackageAttackForTesting::DuplicateLevelActorReference,
			TEXT("duplicate nonzero Level actor reference")),
		TPair<EWoTFrontEndPackageAttackForTesting, const TCHAR*>(
			EWoTFrontEndPackageAttackForTesting::LevelModelTargetsNonModel,
			TEXT("Level model target has non-Model provenance")),
		TPair<EWoTFrontEndPackageAttackForTesting, const TCHAR*>(
			EWoTFrontEndPackageAttackForTesting::UnclassifiedInternalReference,
			TEXT("internal reference has unclassified provenance")),
		TPair<EWoTFrontEndPackageAttackForTesting, const TCHAR*>(
			EWoTFrontEndPackageAttackForTesting::ThunderSourceRootMismatch,
			TEXT("thunder source root mismatch")),
		TPair<EWoTFrontEndPackageAttackForTesting, const TCHAR*>(
			EWoTFrontEndPackageAttackForTesting::ThunderSourceCaseMismatch,
			TEXT("thunder source case mismatch"))})
	{
		TWoTBootstrapResult<TArray<uint8>> Hostile =
			MakeHostileFrontEndPackageBytesForTesting(
				Sources[0].Bytes, Attack.Key);
		if (!TestFalse(FString(Attack.Value) + TEXT(" fixture builds"),
			Hostile.IsError())) continue;
		TArray<FWoTFrontEndSourceBytesForTesting> HostileSources = Sources;
		HostileSources[0].Bytes = MoveTemp(Hostile).GetValue();
		TWoTBootstrapResult<FWoTFrontEndDescriptor> Rejected =
			BuildFrontEndDescriptorPackageBytesForTesting(
				HostileSources, EntryContract);
		if (!TestTrue(Attack.Value, Rejected.IsError())) continue;
		const TCHAR* ExpectedFailure =
			Attack.Key == EWoTFrontEndPackageAttackForTesting::DuplicateLevelActorReference
			? TEXT("duplicate nonzero reference")
			: Attack.Key == EWoTFrontEndPackageAttackForTesting::LevelModelTargetsNonModel
			? TEXT("model reference is malformed")
			: Attack.Key == EWoTFrontEndPackageAttackForTesting::ThunderSourceRootMismatch
			? TEXT("source root")
			: Attack.Key == EWoTFrontEndPackageAttackForTesting::ThunderSourceCaseMismatch
			? TEXT("does not resolve to a Sound")
			: TEXT("no semantic classification");
		TestTrue(FString(Attack.Value) + TEXT(" reaches the intended guard"),
			Rejected.GetError().ToString().Contains(ExpectedFailure));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSidecarSpec,
	"WoT.RetailReader.FrontEndWorld.CanonicalSidecarAndWorldIsolation",
	WoTFrontEndSpecFlags)

bool FWoTFrontEndSidecarSpec::RunTest(const FString&)
{
	FWoTRetailContract Contract = WoTFrontEndSpecContract();
	TWoTBootstrapResult<FWoTFrontEndDescriptor> Built =
		BuildFrontEndDescriptorSemanticFixtureForTesting(
			WoTFrontEndSpecBytes(WoTFrontEndSpecFixture()), Contract);
	if (!TestFalse(TEXT("fixture descriptor"), Built.IsError())) return false;
	const FWoTSha256 Installer = WoTFrontEndSpecSha(TEXT('1'));
	const FWoTSha256 ContractSha = WoTFrontEndSpecSha(TEXT('2'));
	const FWoTSha256 Production = WoTFrontEndSpecSha(TEXT('3'));
	TWoTBootstrapResult<TArray<uint8>> Sidecar =
		WriteSealedFrontEndDescriptorSidecar(
			Built.GetValue(), Installer, ContractSha, Production);
	if (!TestFalse(TEXT("front-end sidecar writes"), Sidecar.IsError())) return false;
	const FString ParityOutput =
		FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_FRONT_END_PARITY_OUTPUT"));
	if (!ParityOutput.IsEmpty())
		TestTrue(TEXT("native parity sidecar is written"),
			FFileHelper::SaveArrayToFile(Sidecar.GetValue(), *ParityOutput));
	TWoTBootstrapResult<FWoTFrontEndDescriptor> Parsed =
		ParseSealedFrontEndDescriptorSidecar(
			Sidecar.GetValue(), Installer, ContractSha, Production,
			Built.GetValue().DescriptorRoot);
	if (!TestFalse(TEXT("front-end sidecar parses"), Parsed.IsError())) return false;
	TestEqual(TEXT("front-end sidecar descriptor root"),
		Parsed.GetValue().DescriptorRoot, Built.GetValue().DescriptorRoot);
	TestTrue(TEXT("installer identity is sealed"),
		ParseSealedFrontEndDescriptorSidecar(
			Sidecar.GetValue(), WoTFrontEndSpecSha(TEXT('4')),
			ContractSha, Production, Built.GetValue().DescriptorRoot).IsError());

	FWoTFrontEndDescriptor WrongRoot = Built.GetValue();
	WrongRoot.DescriptorRoot = WoTFrontEndSpecSha(TEXT('0'));
	TestTrue(TEXT("descriptor-root mismatch"),
		WriteSealedFrontEndDescriptorSidecar(
			WrongRoot, Installer, ContractSha, Production).IsError());
	auto RejectsAuthenticatedCameraTamper = [this, &Built, &Installer,
		&ContractSha, &Production](const TCHAR* Label,
		TFunctionRef<void(FWoTFrontEndDescriptor&)> Mutate)
	{
		FWoTFrontEndDescriptor Tampered = Built.GetValue();
		Mutate(Tampered);
		TWoTBootstrapResult<FWoTSha256> Root =
			ComputeFrontEndDescriptorRoot(Tampered);
		check(!Root.IsError());
		Tampered.DescriptorRoot = MoveTemp(Root).GetValue();
		TWoTBootstrapResult<TArray<uint8>> TamperedSidecar =
			WriteSealedFrontEndDescriptorSidecar(
				Tampered, Installer, ContractSha, Production);
		if (!TestFalse(FString(Label) + TEXT(" remains a derived descriptor"),
			TamperedSidecar.IsError())) return;
		TestTrue(Label, ParseSealedFrontEndDescriptorSidecar(
			TamperedSidecar.GetValue(), Installer, ContractSha, Production,
			Built.GetValue().DescriptorRoot).IsError());
	};
	RejectsAuthenticatedCameraTamper(TEXT("authenticated camera location tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Descriptor.CameraLocation.X += 1.0;
		});
	RejectsAuthenticatedCameraTamper(TEXT("authenticated camera location Y tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Descriptor.CameraLocation.Y += 1.0;
		});
	RejectsAuthenticatedCameraTamper(TEXT("authenticated camera location Z tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Descriptor.CameraLocation.Z += 1.0;
		});
	RejectsAuthenticatedCameraTamper(TEXT("authenticated camera pitch tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Descriptor.CameraRotation.Pitch += 1.0;
		});
	RejectsAuthenticatedCameraTamper(TEXT("authenticated camera yaw tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Descriptor.CameraRotation.Yaw += 1.0;
		});
	RejectsAuthenticatedCameraTamper(TEXT("authenticated camera roll tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Descriptor.CameraRotation.Roll += 1.0;
		});
	RejectsAuthenticatedCameraTamper(TEXT("authenticated thunder order tamper"),
		[](FWoTFrontEndDescriptor& Descriptor)
		{
			Swap(Descriptor.ThunderSoundSourceExports[0],
				Descriptor.ThunderSoundSourceExports[1]);
		});
	FWoTFrontEndDescriptor MissingThunder = Built.GetValue();
	MissingThunder.ThunderSoundSourceExports.Pop();
	TestTrue(TEXT("missing thunder authority is not writable"),
		WriteSealedFrontEndDescriptorSidecar(
			MissingThunder, Installer, ContractSha, Production).IsError());
	FWoTFrontEndDescriptor WrongFov = Built.GetValue();
	WrongFov.HorizontalFov = 91.0f;
	TWoTBootstrapResult<FWoTSha256> WrongFovRoot =
		ComputeFrontEndDescriptorRoot(WrongFov);
	check(!WrongFovRoot.IsError());
	WrongFov.DescriptorRoot = MoveTemp(WrongFovRoot).GetValue();
	TestTrue(TEXT("structurally invalid camera FOV is not writable"),
		WriteSealedFrontEndDescriptorSidecar(
			WrongFov, Installer, ContractSha, Production).IsError());

	FWoTFrontEndDescriptor OverBound = Built.GetValue();
	OverBound.CameraLocation.X = 10000001.0;
	TWoTBootstrapResult<FWoTSha256> OverBoundRoot =
		ComputeFrontEndDescriptorRoot(OverBound);
	check(!OverBoundRoot.IsError());
	OverBound.DescriptorRoot = MoveTemp(OverBoundRoot).GetValue();
	TestTrue(TEXT("over-bound camera location is not writable"),
		WriteSealedFrontEndDescriptorSidecar(
			OverBound, Installer, ContractSha, Production).IsError());
	const TArray<uint8> OverBoundSidecar = WoTFrontEndSpecResealLocationX(
		Sidecar.GetValue(), Built.GetValue(), OverBound);
	TestTrue(TEXT("matching-root sealed over-bound camera is not parseable"),
		ParseSealedFrontEndDescriptorSidecar(
			OverBoundSidecar, Installer, ContractSha, Production,
			OverBound.DescriptorRoot).IsError());

	TArray<FWoTWorldDescriptor> Nineteen;
	for (uint32 Slot = 1; Slot <= 19; ++Slot)
	{
		FWoTWorldDescriptor& World = Nineteen.AddDefaulted_GetRef();
		World.NeutralMapSlot = Slot;
		World.MapIdentity = FString::Printf(TEXT("Map%02u"), Slot);
	}
	auto WorldRecords = WriteCacheWorldDescriptors(Nineteen);
	TestFalse(TEXT("manifest world records accept exactly nineteen"), WorldRecords.IsError());
	Nineteen.AddDefaulted_GetRef().NeutralMapSlot = 20;
	TestTrue(TEXT("manifest world records reject twenty"),
		WriteCacheWorldDescriptors(Nineteen).IsError());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
