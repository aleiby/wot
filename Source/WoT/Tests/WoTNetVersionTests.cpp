// Release networking specs. Helper names are NetVersion-prefixed because the
// test sources share unity translation units.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTNetVersion.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/NetworkVersion.h"

namespace
{
	class FNetVersionConfigGuard
	{
	public:
		FNetVersionConfigGuard()
		{
			check(GConfig);
			bHadSection = GConfig->DoesSectionExist(
				TEXT("WoTNetVersion"), GGameIni);
			bHadReleaseCommit = GConfig->GetString(
				TEXT("WoTNetVersion"), TEXT("ReleaseCommit"),
				SavedReleaseCommit, GGameIni);
			GConfig->RemoveKey(
				TEXT("WoTNetVersion"), TEXT("ReleaseCommit"), GGameIni);
			FNetworkVersion::InvalidateNetworkChecksum();
		}

		~FNetVersionConfigGuard()
		{
			GConfig->RemoveKey(
				TEXT("WoTNetVersion"), TEXT("ReleaseCommit"), GGameIni);
			if (!bHadSection)
			{
				// SetString creates the section and RemoveKey leaves it behind
				// empty — drop it entirely so 600+ later specs in this process
				// see the ambient unpinned config (review-caught leak).
				if (FConfigFile* GameIni = GConfig->FindConfigFile(GGameIni))
				{
					GameIni->Remove(TEXT("WoTNetVersion"));
				}
			}
			else if (bHadReleaseCommit)
			{
				GConfig->SetString(
					TEXT("WoTNetVersion"), TEXT("ReleaseCommit"),
					*SavedReleaseCommit, GGameIni);
			}
			FNetworkVersion::InvalidateNetworkChecksum();
		}

		bool HadReleaseCommit() const
		{
			return bHadReleaseCommit;
		}

	private:
		FString SavedReleaseCommit;
		bool bHadSection = false;
		bool bHadReleaseCommit = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetVersionUnpinnedSpec,
	"WoT.NetVersion.Unpinned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetVersionUnpinnedSpec::RunTest(const FString&)
{
	FNetVersionConfigGuard ConfigGuard;
	TestFalse(TEXT("the repo/editor config is not release-pinned"),
		ConfigGuard.HadReleaseCommit());

	const uint32 BaseVersion =
		FNetworkVersion::GetLocalNetworkVersion(false);
	TestEqual(TEXT("an absent release commit leaves Current transparent"),
		WoTNetVersion::Current(), BaseVersion);

	WoTNetVersion::Bind();
	TestEqual(TEXT("the bound unpinned override returns the base version"),
		FNetworkVersion::GetLocalNetworkVersion(), BaseVersion);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetVersionPinnedSpec,
	"WoT.NetVersion.Pinned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetVersionPinnedSpec::RunTest(const FString&)
{
	FNetVersionConfigGuard ConfigGuard;
	const FString ReleaseCommit(TEXT("abcdef123"));
	GConfig->SetString(
		TEXT("WoTNetVersion"), TEXT("ReleaseCommit"),
		*ReleaseCommit, GGameIni);
	FNetworkVersion::InvalidateNetworkChecksum();

	const uint32 BaseVersion =
		FNetworkVersion::GetLocalNetworkVersion(false);
	const uint32 First = WoTNetVersion::Current();
	const uint32 Second = WoTNetVersion::Current();
	TestNotEqual(TEXT("a release commit changes the network version"),
		First, BaseVersion);
	TestEqual(TEXT("the pinned version is deterministic"), First, Second);
	TestEqual(TEXT("Current uses the documented release-commit mix"),
		First, WoTNetVersion::ComputeFor(ReleaseCommit));
	TestNotEqual(TEXT("different release commits produce different versions"),
		First, WoTNetVersion::ComputeFor(TEXT("abcdef124")));
	TestFalse(TEXT("UE rejects base and pinned versions as incompatible"),
		FNetworkVersion::IsNetworkCompatible(BaseVersion, First));

	// The review-caught shared-cache orderings: a direct Current() must neither
	// poison UE's checksum cache with the base (pin bypass on the next normal
	// query) nor re-hash an already-pinned cached value.
	WoTNetVersion::Bind();
	FNetworkVersion::InvalidateNetworkChecksum();
	const uint32 DirectFirst = WoTNetVersion::Current();
	TestEqual(TEXT("normal query after a direct Current stays pinned"),
		FNetworkVersion::GetLocalNetworkVersion(), DirectFirst);
	TestEqual(TEXT("direct Current after a cached normal query stays stable"),
		WoTNetVersion::Current(), DirectFirst);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetVersionBindIdempotentSpec,
	"WoT.NetVersion.BindIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetVersionBindIdempotentSpec::RunTest(const FString&)
{
	FNetVersionConfigGuard ConfigGuard;
	const FString ReleaseCommit(TEXT("123456789"));
	GConfig->SetString(
		TEXT("WoTNetVersion"), TEXT("ReleaseCommit"),
		*ReleaseCommit, GGameIni);
	FNetworkVersion::InvalidateNetworkChecksum();
	const uint32 Expected = WoTNetVersion::ComputeFor(ReleaseCommit);

	WoTNetVersion::Bind();
	WoTNetVersion::Bind();
	TestEqual(TEXT("double Bind leaves a working override"),
		FNetworkVersion::GetLocalNetworkVersion(), Expected);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
