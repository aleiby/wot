#include "WoTBootstrapCore/TestOverrides.h"

#include "HAL/PlatformMisc.h"

namespace
{
constexpr TCHAR WoTTestAnnouncementUrlEnvironment[] =
	TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL");

#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
constexpr TCHAR WoTTestAnnouncementKey[] =
	TEXT("ed54a59fb1ac3a51239351362941b868e85a60e3d7b2485d828821dc7a69c279");
constexpr TCHAR WoTTestReleaseRoot[] =
	TEXT("ba3c735cdbe4235e6cadf81c412b883a3472d1045faea189f584f8beca46e343");
#endif
}

bool WoTParseStrictTestLoopbackUrl(FStringView Candidate, FString* OutUrl)
{
	constexpr FStringView Prefix(TEXTVIEW("http://127.0.0.1:"));
	if (!Candidate.StartsWith(Prefix, ESearchCase::CaseSensitive)
		|| Candidate.Contains(TEXTVIEW("?"))
		|| Candidate.Contains(TEXTVIEW("#"))
		|| Candidate.Contains(TEXTVIEW("@"))
		|| Candidate.Contains(TEXTVIEW("\\")))
	{
		return false;
	}

	const FStringView Rest = Candidate.RightChop(Prefix.Len());
	int32 SlashIndex = INDEX_NONE;
	if (!Rest.FindChar(TEXT('/'), SlashIndex) || SlashIndex <= 0
		|| SlashIndex == Rest.Len() - 1)
	{
		return false;
	}
	const FStringView PortText = Rest.Left(SlashIndex);
	uint32 Port = 0;
	for (const TCHAR Digit : PortText)
	{
		if (Digit < TEXT('0') || Digit > TEXT('9'))
		{
			return false;
		}
		Port = Port * 10u + static_cast<uint32>(Digit - TEXT('0'));
		if (Port > 65535u)
		{
			return false;
		}
	}
	if (Port == 0u)
	{
		return false;
	}
	for (const TCHAR Character : Candidate)
	{
		if (Character <= 0x20 || Character == 0x7f)
		{
			return false;
		}
	}
	if (OutUrl != nullptr)
	{
		*OutUrl = FString(Candidate);
	}
	return true;
}

bool WoTTryGetStrictTestLoopbackOverride(
	const TCHAR* EnvironmentName, FString& OutUrl)
{
	const FString Value =
		FPlatformMisc::GetEnvironmentVariable(EnvironmentName);
	return WoTParseStrictTestLoopbackUrl(Value, &OutUrl);
}

bool WoTIsTestLoopbackOverrideActive()
{
	FString Ignored;
	return WoTTryGetStrictTestLoopbackOverride(
		WoTTestAnnouncementUrlEnvironment, Ignored);
}

#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
bool WoTShouldAcceptTestUpdateIdentities()
{
	return WoTIsTestLoopbackOverrideActive();
}

const TCHAR* WoTTestAnnouncementPublicKeyHex()
{
	return WoTTestAnnouncementKey;
}

const TCHAR* WoTTestReleaseKeyRingRootHex()
{
	return WoTTestReleaseRoot;
}
#endif
