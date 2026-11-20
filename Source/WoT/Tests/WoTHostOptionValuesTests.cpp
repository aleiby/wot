#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTFrontEnd.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHostAllowedValuesSpec, "WoT.FrontEnd.HostAllowedValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHostAllowedValuesSpec::RunTest(const FString&)
{
	FWoTHostRequest Request;
	FWoTHostOptionRow Row;
	Row.URLKey = TEXT("TestValue");
	Row.Min = 2;
	Row.Max = [](const FWoTHostRequest&) { return 8; };
	Row.DefaultValue = 4;
	TestEqual(TEXT("legacy default unchanged"), FWoTFrontEndModel::HostOptionValue(Request, Row), 4);
	for (int32 Value : {-10, 2, 3, 5, 8, 100})
	{
		Request.ExtraValues.Add(Row.URLKey, Value);
		TestEqual(TEXT("legacy continuous clamp unchanged"), FWoTFrontEndModel::HostOptionValue(Request, Row), FMath::Clamp(Value, 2, 8));
	}
	TestFalse(TEXT("legacy numeric text remains the default"), bool(Row.ValueText));
	Row.Values = [](const FWoTHostRequest&) { return TArray<int32>{9, 0, 2, 5}; };
	for (const auto& Pair : TArray<TPair<int32, int32>>{{1, 0}, {4, 5}, {7, 5}, {-100, 0}, {100, 9}})
	{
		Request.ExtraValues.Add(Row.URLKey, Pair.Key);
		TestEqual(TEXT("allowed values supersede bounds and ties choose the lower value"),
			FWoTFrontEndModel::HostOptionValue(Request, Row), Pair.Value);
	}
	return !HasAnyErrors();
}
#endif
