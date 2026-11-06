#include "WoTRetailReader/Testing/ReferencePaths.h"

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReferencePathsClassificationSpec,
	"WoT.RetailReader.ReferencePaths.Classification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTReferencePathsClassificationSpec::RunTest(const FString&)
{
	for (const TCHAR* Path : {
		TEXT("original"), TEXT("original/Maps/Arena_01.wot"),
		TEXT("devtree"), TEXT("devtree/U2Radiosity"),
		TEXT("exports"), TEXT("exports/script/WOT")})
	{
		TestTrue(FString::Printf(TEXT("%s is private"), Path),
			WoTIsPrivateReferencePath(Path));
	}
	for (const TCHAR* Path : {
		TEXT("originality"), TEXT("devtrees/file"), TEXT("exports-old")})
	{
		TestFalse(FString::Printf(TEXT("%s is not private"), Path),
			WoTIsPrivateReferencePath(Path));
	}
	return !HasAnyErrors();
}

#endif
