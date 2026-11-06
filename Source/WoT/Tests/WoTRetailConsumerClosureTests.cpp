#include "Misc/AutomationTest.h"

#include "../Payload/WoTNeutralResources.h"
#include "../Payload/WoTPayloadActivation.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTRetailClosureFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailConsumerClosureTest,
	"WoT.Payload.ConsumerClosure.NeutralResourceDomains",
	WoTRetailClosureFlags)

bool FWoTRetailConsumerClosureTest::RunTest(const FString&)
{
	ResetWoTPayloadActivationForTesting();
	TestFalse(TEXT("consumer graph begins release-dormant"),
		IsWoTPayloadBootstrapActive());

	const EWoTNeutralResourceKind Kinds[] = {
		EWoTNeutralResourceKind::Texture,
		EWoTNeutralResourceKind::Sound,
		EWoTNeutralResourceKind::Music,
		EWoTNeutralResourceKind::VertexMesh,
		EWoTNeutralResourceKind::Animation,
		EWoTNeutralResourceKind::BSP,
		EWoTNeutralResourceKind::World,
		EWoTNeutralResourceKind::Font,
		EWoTNeutralResourceKind::Procedural,
		EWoTNeutralResourceKind::Registry,
		EWoTNeutralResourceKind::Localization,
		EWoTNeutralResourceKind::ClassDefaults,
	};
	TSet<FString> Schemas;
	for (const EWoTNeutralResourceKind Kind : Kinds)
	{
		const FString Schema = WoTNeutralResourceSchema(Kind);
		TestFalse(
			*FString::Printf(TEXT("%s has a schema"), LexToString(Kind)),
			Schema.IsEmpty());
		TestFalse(
			*FString::Printf(TEXT("%s schema is unique"), LexToString(Kind)),
			Schemas.Contains(Schema));
		Schemas.Add(Schema);
		FWoTNeutralResourceId Resource{TEXT("cache/closure-fixture"), Kind};
		TestTrue(
			*FString::Printf(TEXT("%s semantic id is valid"),
				LexToString(Kind)),
			Resource.IsValid());
	}
	TestFalse(TEXT("legacy texture cache keys are not neutral resource ids"),
		FWoTNeutralResourceId{
			TEXT("texture:Package__Texture"),
			EWoTNeutralResourceKind::Texture}.IsValid());
	TestFalse(TEXT("legacy sound cache keys are not neutral resource ids"),
		FWoTNeutralResourceId{
			TEXT("sound:Package__Sound"),
			EWoTNeutralResourceKind::Sound}.IsValid());
	TestEqual(TEXT("all release domains are represented"),
		Schemas.Num(), static_cast<int32>(UE_ARRAY_COUNT(Kinds)));
	TestEqual(TEXT("class-default schema keeps the frozen hyphen spelling"),
		WoTNeutralResourceSchema(EWoTNeutralResourceKind::ClassDefaults),
		FString(TEXT("wot-class-default-v3")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
