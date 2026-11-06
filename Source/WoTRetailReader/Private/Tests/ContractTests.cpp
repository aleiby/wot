// Signed cache-contract-v3 grammar, authentication, and closure specs.
#include "Misc/AutomationTest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTRetailContractSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

const TCHAR* const RetailContractPublicKeyHex =
	TEXT("d2d8e33e37fe029ff68b897868d517dbf327071c82231d2caad81b32bbda02af");

const TCHAR* const RetailContractSignedJson = TEXT(R"wotjson({"payload":{"audit_set":[{"audit_rule":"audit.synthetic","runtime_consumer":"SyntheticConsumer"}],"contract_id":"contract-test","contract_version":3,"facts":[{"audit_rule":"audit.synthetic","domain":"texture","fact_id":"fact.texture","importer":"import-texture","outputs":[{"logical_id":"out/texture","max_decoded_size":1024,"schema":"schema-texture"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.texture","package":"Package/texture"}},{"audit_rule":"audit.synthetic","domain":"sound","fact_id":"fact.sound","importer":"import-sound","outputs":[{"logical_id":"out/sound","max_decoded_size":1025,"schema":"schema-sound"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.sound","package":"Package/sound"}},{"audit_rule":"audit.synthetic","domain":"music","fact_id":"fact.music","importer":"import-music","outputs":[{"logical_id":"out/music","max_decoded_size":1026,"schema":"schema-music"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.music","package":"Package/music"}},{"audit_rule":"audit.synthetic","domain":"mesh","fact_id":"fact.mesh","importer":"import-mesh","outputs":[{"logical_id":"out/mesh","max_decoded_size":1027,"schema":"schema-mesh"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.mesh","package":"Package/mesh"}},{"audit_rule":"audit.synthetic","domain":"animation","fact_id":"fact.animation","importer":"import-animation","outputs":[{"logical_id":"out/animation","max_decoded_size":1028,"schema":"schema-animation"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.animation","package":"Package/animation"}},{"audit_rule":"audit.synthetic","domain":"bsp","fact_id":"fact.bsp","importer":"import-bsp","outputs":[{"logical_id":"out/bsp","max_decoded_size":1029,"schema":"schema-bsp"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.bsp","package":"Package/bsp"}},{"audit_rule":"audit.synthetic","domain":"world","fact_id":"fact.world","importer":"import-world","outputs":[{"logical_id":"out/world","max_decoded_size":1030,"schema":"schema-world"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.world","package":"Package/world"}},{"audit_rule":"audit.synthetic","domain":"font","fact_id":"fact.font","importer":"import-font","outputs":[{"logical_id":"out/font","max_decoded_size":1031,"schema":"schema-font"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.font","package":"Package/font"}},{"audit_rule":"audit.synthetic","domain":"procedural","fact_id":"fact.procedural","importer":"import-procedural","outputs":[{"logical_id":"out/procedural","max_decoded_size":1032,"schema":"schema-procedural"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.procedural","package":"Package/procedural"}},{"audit_rule":"audit.synthetic","domain":"registry","fact_id":"fact.registry","importer":"import-registry","outputs":[{"logical_id":"out/registry","max_decoded_size":1033,"schema":"schema-registry"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.registry","package":"Package/registry"}},{"audit_rule":"audit.synthetic","domain":"localization","fact_id":"fact.localization","importer":"import-localization","outputs":[{"logical_id":"out/localization","max_decoded_size":1034,"schema":"schema-localization"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.localization","package":"Package/localization"}},{"audit_rule":"audit.synthetic","domain":"class_default","fact_id":"fact.class_default","importer":"import-class_default","outputs":[{"logical_id":"out/class_default","max_decoded_size":1035,"schema":"schema-class_default"}],"runtime_consumer":"SyntheticConsumer","source":{"export":"Export.class_default","package":"Package/class_default"}}],"format":"cache-contract-v3","key_id":"contract-key","release_sequence":19,"startup_critical_logical_ids":["out/sound","out/texture"],"startup_critical_logical_ids_sha256":"287b5b78094659c5b7396c9339faf19d9e8593a3db61f1cc34708003d189f9d6"},"signature":"02d425a9ca701256ef1c15315adec3cec2876e4d3912fd41900ab584056ee9080a2a55eb10f56a3e0e0a3d9140a387a8d83522fdd8a9493ac653b7116a330607"})wotjson");

TArray<uint8> WoTRetailContractSpecBytes(FStringView Ascii)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Ascii.Len());
	for (const TCHAR Character : Ascii)
	{
		Bytes.Add(static_cast<uint8>(Character));
	}
	return Bytes;
}

FWoTKeyRing WoTRetailContractSpecRing()
{
	FWoTKeyRing Ring;
	Ring.Format = TEXT("wot-release-key-ring-v1");
	Ring.Epoch = 1;
	FWoTReleaseKey Key;
	Key.KeyId = TEXT("contract-key");
	Key.FirstSequence = 1;
	Key.LastSequence = 100;
	TWoTBootstrapResult<FWoTEd25519PublicKey> PublicKey =
		FWoTEd25519PublicKey::FromHex(RetailContractPublicKeyHex);
	if (!PublicKey.IsError())
	{
		Key.PublicKey = PublicKey.GetValue();
	}
	Ring.Keys.Add(MoveTemp(Key));
	return Ring;
}

TWoTBootstrapResult<FWoTRetailContract> WoTRetailContractSpecParse(
	FStringView Json = RetailContractSignedJson)
{
	FWoTKeyRing Ring = WoTRetailContractSpecRing();
	TArray<uint8> Bytes = WoTRetailContractSpecBytes(Json);
	return ParseRetailContract(Bytes, Ring);
}

bool WoTRetailContractSpecExpectPath(FAutomationTestBase& Test,
	FWoTRetailContract& Contract, FStringView ExpectedPath)
{
	FWoTBootstrapStatus Status = ValidateFactClosure(Contract);
	if (!Test.TestTrue(TEXT("mutated closure is rejected"), Status.IsError()))
	{
		return false;
	}
	return Test.TestEqual(TEXT("stable mutation field path"),
		Status.GetError().Path, FString(ExpectedPath));
}

FString WoTRetailContractSpecPayloadJson()
{
	const FString Signed = RetailContractSignedJson;
	const FString Prefix = TEXT("{\"payload\":");
	const FString Suffix = TEXT(",\"signature\":");
	const int32 Start = Prefix.Len();
	const int32 End = Signed.Find(Suffix, ESearchCase::CaseSensitive);
	return End == INDEX_NONE ? FString() : Signed.Mid(Start, End - Start);
}

FString WoTRetailContractSpecPayloadWithStartupClosure(
	FStringView LogicalIdsJson,
	FStringView Digest = TEXT("287b5b78094659c5b7396c9339faf19d9e8593a3db61f1cc34708003d189f9d6"))
{
	FString Payload = WoTRetailContractSpecPayloadJson();
	Payload.ReplaceInline(TEXT("[\"out/sound\",\"out/texture\"]"),
		*FString(LogicalIdsJson), ESearchCase::CaseSensitive);
	Payload.ReplaceInline(
		TEXT("287b5b78094659c5b7396c9339faf19d9e8593a3db61f1cc34708003d189f9d6"),
		*FString(Digest), ESearchCase::CaseSensitive);
	return Payload;
}

bool WoTRetailContractSpecReplaceFirst(FString& Value, FStringView Needle,
	FStringView Replacement)
{
	const int32 Index = Value.Find(FString(Needle), ESearchCase::CaseSensitive);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Value = Value.Left(Index) + FString(Replacement) + Value.Mid(Index + Needle.Len());
	return true;
}

bool WoTRetailContractSpecExpectPayloadPath(FAutomationTestBase& Test,
	FStringView Json, FStringView ExpectedPath)
{
	TArray<uint8> Bytes = WoTRetailContractSpecBytes(Json);
	TWoTBootstrapResult<FWoTRetailContract> Parsed = ParseRetailContractPayload(Bytes);
	if (!Test.TestTrue(TEXT("mutated payload JSON is rejected"), Parsed.IsError()))
	{
		return false;
	}
	return Test.TestEqual(TEXT("parser-level mutation field path"),
		Parsed.GetError().Path, FString(ExpectedPath));
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailContractSignedSpec,
	"WoT.RetailReader.Contract.SignedGrammar", WoTRetailContractSpecFlags)

bool FWoTRetailContractSignedSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
	if (!TestFalse(TEXT("signed contract verifies and parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("one fact per domain"), Parsed.GetValue().Facts.Num(), 12);
	TSet<EWoTFactDomain> Domains;
	for (const FWoTRequiredFact& Fact : Parsed.GetValue().Facts)
	{
		Domains.Add(Fact.Domain);
	}
	TestEqual(TEXT("all twelve domains are distinct"), Domains.Num(), 12);

	FString Tampered = RetailContractSignedJson;
	Tampered.ReplaceInline(TEXT("\"release_sequence\":19"),
		TEXT("\"release_sequence\":20"), ESearchCase::CaseSensitive);
	TWoTBootstrapResult<FWoTRetailContract> Rejected = WoTRetailContractSpecParse(Tampered);
	if (TestTrue(TEXT("payload tampering is rejected"), Rejected.IsError()))
	{
		TestEqual(TEXT("tampering is a clean crypto rejection"), Rejected.GetError().Code,
			EWoTBootstrapError::Crypto);
	}

	FString Unknown = RetailContractSignedJson;
	// Outer-envelope closure can be tested without resigning because payload bytes
	// remain identical; insert the lexically-last field after the signature.
	Unknown = Unknown.LeftChop(1) + TEXT(",\"unknown\":0}");
	TWoTBootstrapResult<FWoTRetailContract> UnknownRejected =
		WoTRetailContractSpecParse(Unknown);
	if (TestTrue(TEXT("unknown envelope field is rejected"), UnknownRejected.IsError()))
	{
		TestEqual(TEXT("unknown field path"), UnknownRejected.GetError().Path,
			FString(TEXT("contract.unknown")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailContractClosureSpec,
	"WoT.RetailReader.Contract.RequiredFactClosure", WoTRetailContractSpecFlags)

bool FWoTRetailContractClosureSpec::RunTest(const FString&)
{
	struct FMutation
	{
		const TCHAR* ExpectedPath;
		TFunction<void(FWoTRetailContract&)> Apply;
	};
	const FMutation Mutations[] = {
		{TEXT("contract.payload.facts[0].importer"),
			[](FWoTRetailContract& Value) { Value.Facts[0].Importer.Empty(); }},
		{TEXT("contract.payload.facts[0].runtime_consumer"),
			[](FWoTRetailContract& Value) { Value.Facts[0].RuntimeConsumer.Empty(); }},
		{TEXT("contract.payload.facts[0].audit_rule"),
			[](FWoTRetailContract& Value) { Value.Facts[0].AuditRule.Empty(); }},
		{TEXT("contract.payload.facts[0].source.package"),
			[](FWoTRetailContract& Value) { Value.Facts[0].SourcePath.Empty(); }},
		{TEXT("contract.payload.facts[0].outputs[0].schema"),
			[](FWoTRetailContract& Value) { Value.Facts[0].Outputs[0].Schema.Empty(); }},
		{TEXT("contract.payload.facts[0].outputs[0].max_decoded_size"),
			[](FWoTRetailContract& Value) { Value.Facts[0].Outputs[0].MaxDecodedSize = 0; }},
	};
	for (const FMutation& Mutation : Mutations)
	{
		TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
		if (Parsed.IsError())
		{
			AddError(Parsed.GetError().ToString());
			return false;
		}
		Mutation.Apply(Parsed.GetValue());
		WoTRetailContractSpecExpectPath(*this, Parsed.GetValue(), Mutation.ExpectedPath);
	}

	{
		TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
		Parsed.GetValue().Facts[1].FactId = Parsed.GetValue().Facts[0].FactId;
		WoTRetailContractSpecExpectPath(*this, Parsed.GetValue(),
			TEXT("contract.payload.facts[1].fact_id"));
	}
	{
		TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
		Parsed.GetValue().Facts[0].Importer = TEXT("generic-authored-v1");
		WoTRetailContractSpecExpectPath(*this, Parsed.GetValue(),
			TEXT("contract.payload.facts[0].outputs"));
	}
	{
		TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
		Parsed.GetValue().Facts[0].RuntimeConsumer = TEXT("AbsentConsumer");
		WoTRetailContractSpecExpectPath(*this, Parsed.GetValue(),
			TEXT("contract.payload.facts[0].runtime_consumer"));
	}
	{
		TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
		Parsed.GetValue().Facts[0].SourceExport = TEXT("Export..texture");
		WoTRetailContractSpecExpectPath(*this, Parsed.GetValue(),
			TEXT("contract.payload.facts[0].source.export"));
	}
	{
		TWoTBootstrapResult<FWoTRetailContract> Parsed = WoTRetailContractSpecParse();
		Parsed.GetValue().Facts[0].SourcePath = TEXT("Pack.age/texture.u");
		WoTRetailContractSpecExpectPath(*this, Parsed.GetValue(),
			TEXT("contract.payload.facts[0].source.package"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailContractJsonMutationSpec,
	"WoT.RetailReader.Contract.ParserStructuralMutations", WoTRetailContractSpecFlags)

bool FWoTRetailContractJsonMutationSpec::RunTest(const FString&)
{
	const FString ValidPayload = WoTRetailContractSpecPayloadJson();
	{
		TArray<uint8> Bytes = WoTRetailContractSpecBytes(ValidPayload);
		TWoTBootstrapResult<FWoTRetailContract> Parsed = ParseRetailContractPayload(Bytes);
		if (!TestFalse(TEXT("base payload structurally parses"), Parsed.IsError()))
		{
			AddError(Parsed.GetError().ToString());
			return false;
		}
	}
	{
		FString EmptyExportComponent = ValidPayload;
		TestTrue(TEXT("empty export component mutation applies"),
			WoTRetailContractSpecReplaceFirst(EmptyExportComponent,
				TEXT("\"export\":\"Export.texture\""),
				TEXT("\"export\":\"Export..texture\"")));
		WoTRetailContractSpecExpectPayloadPath(*this, EmptyExportComponent,
			TEXT("payload.facts[0].source.export"));
	}
	{
		FString DottedPackageComponent = ValidPayload;
		TestTrue(TEXT("dotted package component mutation applies"),
			WoTRetailContractSpecReplaceFirst(DottedPackageComponent,
				TEXT("\"package\":\"Package/texture\""),
				TEXT("\"package\":\"Pack.age/texture\"")));
		WoTRetailContractSpecExpectPayloadPath(*this, DottedPackageComponent,
			TEXT("payload.facts[0].source.package"));
	}
	{
		FString EmptyPackageComponent = ValidPayload;
		TestTrue(TEXT("empty package component mutation applies"),
			WoTRetailContractSpecReplaceFirst(EmptyPackageComponent,
				TEXT("\"package\":\"Package/texture\""),
				TEXT("\"package\":\"Package//texture\"")));
		WoTRetailContractSpecExpectPayloadPath(*this, EmptyPackageComponent,
			TEXT("payload.facts[0].source.package"));
	}

	struct FJsonMutation
	{
		const TCHAR* Name;
		const TCHAR* Needle;
		const TCHAR* Replacement;
		const TCHAR* ExpectedPath;
	};
	const FJsonMutation RemovedMembers[] = {
		{TEXT("importer"),
			TEXT("\"fact_id\":\"fact.texture\",\"importer\":\"import-texture\","),
			TEXT("\"fact_id\":\"fact.texture\","),
			TEXT("payload.facts[0].importer")},
		{TEXT("runtime consumer"),
			TEXT("\"runtime_consumer\":\"SyntheticConsumer\",\"source\":{\"export\":\"Export.texture\""),
			TEXT("\"source\":{\"export\":\"Export.texture\""),
			TEXT("payload.facts[0].runtime_consumer")},
		{TEXT("audit rule"),
			TEXT("{\"audit_rule\":\"audit.synthetic\",\"domain\":\"texture\""),
			TEXT("{\"domain\":\"texture\""),
			TEXT("payload.facts[0].audit_rule")},
		{TEXT("source package"),
			TEXT(",\"package\":\"Package/texture\""), TEXT(""),
			TEXT("payload.facts[0].source.package")},
		{TEXT("output schema"),
			TEXT(",\"schema\":\"schema-texture\""), TEXT(""),
			TEXT("payload.facts[0].outputs[0].schema")},
		{TEXT("maximum decoded size"),
			TEXT(",\"max_decoded_size\":1024"), TEXT(""),
			TEXT("payload.facts[0].outputs[0].max_decoded_size")},
	};
	for (const FJsonMutation& Mutation : RemovedMembers)
	{
		FString Changed = ValidPayload;
		if (!TestTrue(Mutation.Name, WoTRetailContractSpecReplaceFirst(
				Changed, Mutation.Needle, Mutation.Replacement)))
		{
			continue;
		}
		WoTRetailContractSpecExpectPayloadPath(*this, Changed, Mutation.ExpectedPath);
	}

	const FJsonMutation UnknownMembers[] = {
		{TEXT("payload unknown"), nullptr, nullptr, TEXT("payload.unknown")},
		{TEXT("fact unknown"),
			TEXT("\"source\":{\"export\":\"Export.texture\",\"package\":\"Package/texture\"}}"),
			TEXT("\"source\":{\"export\":\"Export.texture\",\"package\":\"Package/texture\"},\"unknown\":0}"),
			TEXT("payload.facts[0].unknown")},
		{TEXT("source unknown"),
			TEXT("{\"export\":\"Export.texture\",\"package\":\"Package/texture\"}"),
			TEXT("{\"export\":\"Export.texture\",\"package\":\"Package/texture\",\"unknown\":0}"),
			TEXT("payload.facts[0].source.unknown")},
		{TEXT("output unknown"),
			TEXT("{\"logical_id\":\"out/texture\",\"max_decoded_size\":1024,\"schema\":\"schema-texture\"}"),
			TEXT("{\"logical_id\":\"out/texture\",\"max_decoded_size\":1024,\"schema\":\"schema-texture\",\"unknown\":0}"),
			TEXT("payload.facts[0].outputs[0].unknown")},
		{TEXT("audit unknown"),
			TEXT("{\"audit_rule\":\"audit.synthetic\",\"runtime_consumer\":\"SyntheticConsumer\"}"),
			TEXT("{\"audit_rule\":\"audit.synthetic\",\"runtime_consumer\":\"SyntheticConsumer\",\"unknown\":0}"),
			TEXT("payload.audit_set[0].unknown")},
	};
	for (const FJsonMutation& Mutation : UnknownMembers)
	{
		FString Changed = ValidPayload;
		bool bChanged = false;
		if (Mutation.Needle == nullptr)
		{
			Changed = Changed.LeftChop(1) + TEXT(",\"unknown\":0}");
			bChanged = true;
		}
		else
		{
			bChanged = WoTRetailContractSpecReplaceFirst(
				Changed, Mutation.Needle, Mutation.Replacement);
		}
		if (!TestTrue(Mutation.Name, bChanged))
		{
			continue;
		}
		WoTRetailContractSpecExpectPayloadPath(*this, Changed, Mutation.ExpectedPath);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailContractStartupClosureSpec,
	"WoT.RetailReader.Contract.StartupCriticalClosure", WoTRetailContractSpecFlags)

bool FWoTRetailContractStartupClosureSpec::RunTest(const FString&)
{
	const FString Valid = WoTRetailContractSpecPayloadWithStartupClosure(
		TEXT("[\"out/sound\",\"out/texture\"]"));
	TArray<uint8> ValidBytes = WoTRetailContractSpecBytes(Valid);
	TWoTBootstrapResult<FWoTRetailContract> Parsed =
		ParseRetailContractPayload(ValidBytes);
	if (!TestFalse(TEXT("canonical startup closure parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("startup closure remains ordered"),
		Parsed.GetValue().StartupCriticalLogicalIds,
		TArray<FString>({TEXT("out/sound"), TEXT("out/texture")}));
	TestEqual(TEXT("startup closure digest remains authenticated"),
		Parsed.GetValue().StartupCriticalLogicalIdsSha256.ToHex(),
		FString(TEXT("287b5b78094659c5b7396c9339faf19d9e8593a3db61f1cc34708003d189f9d6")));

	WoTRetailContractSpecExpectPayloadPath(*this,
		WoTRetailContractSpecPayloadWithStartupClosure(
			TEXT("[\"out/sound\",\"out/texture\"]"),
			TEXT("387b5b78094659c5b7396c9339faf19d9e8593a3db61f1cc34708003d189f9d6")),
		TEXT("payload.startup_critical_logical_ids_sha256"));
	WoTRetailContractSpecExpectPayloadPath(*this,
		WoTRetailContractSpecPayloadWithStartupClosure(
			TEXT("[\"out/texture\",\"out/sound\"]")),
		TEXT("payload.startup_critical_logical_ids[1]"));
	WoTRetailContractSpecExpectPayloadPath(*this,
		WoTRetailContractSpecPayloadWithStartupClosure(
			TEXT("[\"out/sound\",\"out/sound\"]")),
		TEXT("payload.startup_critical_logical_ids[1]"));
	WoTRetailContractSpecExpectPayloadPath(*this,
		WoTRetailContractSpecPayloadWithStartupClosure(
			TEXT("[\"out/sound\",\"out/unknown\"]")),
		TEXT("payload.startup_critical_logical_ids[1]"));
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
