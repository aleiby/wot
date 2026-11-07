#include "WoTUpdateBanner.h"

#include "../Updater/WoTReleaseIdentity.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

namespace
{
	bool WoTIsAsciiLetter(TCHAR Character)
	{
		return (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('a') && Character <= TEXT('z'));
	}

	bool WoTIsAsciiDigit(TCHAR Character)
	{
		return Character >= TEXT('0') && Character <= TEXT('9');
	}

	bool WoTIsAsciiHexDigit(TCHAR Character)
	{
		return WoTIsAsciiDigit(Character)
			|| (Character >= TEXT('A') && Character <= TEXT('F'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'));
	}

	bool WoTIsValidReleasePageHost(FStringView Host)
	{
		// A public DNS name: two or more labels of [A-Za-z0-9-] (no leading or
		// trailing '-'), ending in an all-letter top-level label. Every numeric
		// IPv4 spelling (dotted, hexadecimal, a single number) fails this shape.
		if (Host.IsEmpty() || Host.Len() > 253) return false;
		int32 LabelStart = 0;
		int32 LabelCount = 0;
		bool bLastLabelAllLetters = false;
		for (int32 Index = 0; Index <= Host.Len(); ++Index)
		{
			if (Index < Host.Len() && Host[Index] != TEXT('.'))
			{
				const TCHAR Character = Host[Index];
				if (!WoTIsAsciiLetter(Character) && !WoTIsAsciiDigit(Character)
					&& Character != TEXT('-'))
				{
					return false;
				}
				continue;
			}

			const int32 LabelLength = Index - LabelStart;
			if (LabelLength <= 0 || LabelLength > 63
				|| Host[LabelStart] == TEXT('-') || Host[Index - 1] == TEXT('-'))
			{
				return false;
			}
			bLastLabelAllLetters = LabelLength >= 2;
			for (int32 LabelIndex = LabelStart; LabelIndex < Index; ++LabelIndex)
			{
				bLastLabelAllLetters = bLastLabelAllLetters
					&& WoTIsAsciiLetter(Host[LabelIndex]);
			}
			++LabelCount;
			LabelStart = Index + 1;
		}
		return LabelCount >= 2 && bLastLabelAllLetters;
	}

	bool WoTIsValidReleasePagePort(FStringView Port)
	{
		if (Port.IsEmpty() || Port.Len() > 5) return false;
		uint32 Value = 0;
		for (const TCHAR Character : Port)
		{
			if (!WoTIsAsciiDigit(Character)) return false;
			Value = Value * 10 + static_cast<uint32>(Character - TEXT('0'));
		}
		return Value >= 1 && Value <= 65535;
	}

	bool WoTIsValidReleasePagePath(FStringView Path)
	{
		if (Path.Len() < 2 || Path[0] != TEXT('/')) return false;
		bool bSegmentHasCharacter = false;
		for (int32 Index = 1; Index < Path.Len(); ++Index)
		{
			const TCHAR Character = Path[Index];
			if (Character == TEXT('/'))
			{
				if (!bSegmentHasCharacter) return false;
				bSegmentHasCharacter = false;
				continue;
			}
			if (WoTIsAsciiLetter(Character) || WoTIsAsciiDigit(Character)
				|| Character == TEXT('.') || Character == TEXT('_')
				|| Character == TEXT('~') || Character == TEXT('-'))
			{
				bSegmentHasCharacter = true;
				continue;
			}
			if (Character != TEXT('%') || Index + 2 >= Path.Len()
				|| !WoTIsAsciiHexDigit(Path[Index + 1])
				|| !WoTIsAsciiHexDigit(Path[Index + 2])) return false;
			bSegmentHasCharacter = true;
			Index += 2;
		}
		return bSegmentHasCharacter;
	}
}

bool WoTIsHttpsReleasePageUrl(FStringView Url)
{
	if (Url.Len() <= 8 || Url.Len() > 2048
		|| !Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
	{
		return false;
	}

	int32 PathStart = INDEX_NONE;
	for (int32 Index = 8; Index < Url.Len(); ++Index)
	{
		if (Url[Index] == TEXT('/'))
		{
			PathStart = Index;
			break;
		}
	}
	if (PathStart == INDEX_NONE) return false;
	const FStringView Authority = Url.Mid(8, PathStart - 8);
	int32 PortSeparator = INDEX_NONE;
	for (int32 Index = 0; Index < Authority.Len(); ++Index)
	{
		if (Authority[Index] != TEXT(':')) continue;
		if (PortSeparator != INDEX_NONE) return false;
		PortSeparator = Index;
	}
	const FStringView Host = PortSeparator == INDEX_NONE
		? Authority : Authority.Left(PortSeparator);
	return WoTIsValidReleasePageHost(Host)
		&& (PortSeparator == INDEX_NONE
			|| WoTIsValidReleasePagePort(Authority.Mid(PortSeparator + 1)))
		&& WoTIsValidReleasePagePath(Url.Mid(PathStart));
}

FWoTUpdateBannerPresentation WoTBuildUpdateBannerPresentation(
	const FWoTUpdateState& State)
{
	FWoTUpdateBannerPresentation Banner;
	// Name a release only by the human version the main menu shows (0.1.2).
	// Some states are decided before the signed manifest that carries it is
	// read (a translocated or read-only copy, a known-bad release); a bare
	// release sequence would never match the menu, so those say "new".
	const bool bNamed = !State.HumanVersion.IsEmpty()
		&& WoTReleaseIdentity::IsDisplayableHumanVersion(State.HumanVersion);
	const FString NewVersion = bNamed
		? FString::Printf(TEXT("version %s"), *State.HumanVersion)
		: FString(TEXT("the new version"));
	const FString MultiplayerLine = FString::Printf(
		TEXT("Players on %s can't join you until you update."), *NewVersion);
	switch (State.Status)
	{
	case EWoTUpdateStatus::Ready:
		Banner.bVisible = true;
		Banner.PrimaryLine = TEXT("Update ready; restart to install");
		Banner.MultiplayerLine = MultiplayerLine;
		// Both platforms install a staged update when the game quits (the
		// Windows launcher swap; the Mac quit-swap), so Ready is one banner.
		Banner.Action = EWoTUpdateBannerAction::Restart;
		Banner.ButtonLabel = TEXT("Quit to install");
		Banner.ReasonLine = TEXT("Open WoT Reborn again to finish installing.");
		break;
	case EWoTUpdateStatus::Available:
		if (State.Reason == TEXT("update-in-progress")
			|| State.Reason == TEXT("update-state-busy"))
		{
			break;
		}
		Banner.bVisible = true;
		Banner.PrimaryLine = bNamed
			? FString::Printf(TEXT("Version %s is available."), *State.HumanVersion)
			: FString(TEXT("A new version is available."));
		if (State.Reason == TEXT("install-root-not-writable"))
		{
			Banner.ReasonLine = TEXT(
				"This folder can't be updated in place; download the new version from the release page.");
		}
		else if (State.Reason == TEXT("translocated"))
		{
			Banner.ReasonLine = TEXT(
				"Move WoT to your Applications folder (or any folder) to enable automatic updates.");
		}
		else if (State.Reason == TEXT("download-failed"))
		{
			Banner.ReasonLine = TEXT(
				"The update couldn't be downloaded. Check your free disk space and connection; WoT Reborn will try again next time you start it.");
		}
		else
		{
			Banner.ReasonLine = TEXT(
				"This copy can't update itself; download the new version from the release page.");
		}
		Banner.MultiplayerLine = MultiplayerLine;
		if (WoTIsHttpsReleasePageUrl(State.ReleasePageUrl))
		{
			Banner.Action = EWoTUpdateBannerAction::OpenReleasePage;
			Banner.ButtonLabel = TEXT("Open release page");
			Banner.ReleasePageUrl = State.ReleasePageUrl;
		}
		break;
	case EWoTUpdateStatus::KnownBad:
		Banner.bVisible = true;
		Banner.PrimaryLine = bNamed
			? FString::Printf(TEXT(
				"Version %s could not start on this computer; you'll get the next version."),
				*State.HumanVersion)
			: FString(TEXT(
				"The new version could not start on this computer; you'll get the next version."));
		break;
	case EWoTUpdateStatus::None:
	case EWoTUpdateStatus::Checking:
	case EWoTUpdateStatus::Downloading:
	default:
		break;
	}
	return Banner;
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
namespace
{
	bool WoTParseFakeReleaseSequence(const FString& Text, uint64& OutSequence)
	{
		if (Text.IsEmpty()) return false;
		for (const TCHAR Character : Text)
		{
			if (!FChar::IsDigit(Character)) return false;
		}
		return LexTryParseString(OutSequence, *Text) && OutSequence > 0;
	}
}

bool WoTParseFakeUpdateState(FStringView Value, FWoTUpdateState& OutState)
{
	const FString Input(Value);
	if (Input.IsEmpty() || Input.TrimStartAndEnd() != Input)
	{
		return false;
	}
	TArray<FString> Parts;
	Input.ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() < 2 || Parts.Num() > 3)
	{
		return false;
	}
	uint64 Sequence = 0;
	if (!WoTParseFakeReleaseSequence(Parts[1], Sequence))
	{
		return false;
	}
	FWoTUpdateState Parsed;
	Parsed.ReleaseSequence = Sequence;
	if (Parts[0].Equals(TEXT("ready"), ESearchCase::CaseSensitive)
		&& Parts.Num() == 2)
	{
		Parsed.Status = EWoTUpdateStatus::Ready;
	}
	else if (Parts[0].Equals(TEXT("knownbad"), ESearchCase::CaseSensitive)
		&& Parts.Num() == 2)
	{
		Parsed.Status = EWoTUpdateStatus::KnownBad;
	}
	else if (Parts[0].Equals(TEXT("available"), ESearchCase::CaseSensitive)
		&& (Parts.Num() == 2 || !Parts[2].IsEmpty()))
	{
		Parsed.Status = EWoTUpdateStatus::Available;
		if (Parts.Num() == 3) Parsed.Reason = Parts[2];
		// The screenshot grammar intentionally carries no external URL. This
		// inert https address exposes the button while its handler remains a
		// heartbeat-only harness action.
		Parsed.ReleasePageUrl = TEXT("https://example.invalid/wot-release");
	}
	else
	{
		return false;
	}
	OutState = MoveTemp(Parsed);
	return true;
}

TOptional<FWoTUpdateState> WoTFakeUpdateStateFromCommandLine()
{
	FString Value;
	if (!FParse::Value(FCommandLine::Get(),
		TEXT("wotfakeupdatestate="), Value))
	{
		return {};
	}
	FWoTUpdateState State;
	return WoTParseFakeUpdateState(Value, State)
		? TOptional<FWoTUpdateState>(MoveTemp(State))
		: TOptional<FWoTUpdateState>();
}
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
