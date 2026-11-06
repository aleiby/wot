#include "WoTUpdateTransports/WoTUpdateTransportPolicy.h"

#include "HAL/PlatformTime.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

namespace
{
struct FWoTUpdateTransportParsedUrl
{
	FString Scheme;
	FString Host;
};

bool WoTUpdateTransportIsAsciiAlpha(TCHAR Character)
{
	return (Character >= TEXT('A') && Character <= TEXT('Z'))
		|| (Character >= TEXT('a') && Character <= TEXT('z'));
}

bool WoTUpdateTransportIsAsciiDigit(TCHAR Character)
{
	return Character >= TEXT('0') && Character <= TEXT('9');
}

TCHAR WoTUpdateTransportAsciiLower(TCHAR Character)
{
	if (Character >= TEXT('A') && Character <= TEXT('Z'))
	{
		return Character + (TEXT('a') - TEXT('A'));
	}
	return Character;
}

bool WoTUpdateTransportContainsAsciiInsensitive(
	FStringView Haystack, FStringView Needle)
{
	if (Needle.IsEmpty() || Haystack.Len() < Needle.Len())
	{
		return false;
	}
	for (int32 Start = 0; Start <= Haystack.Len() - Needle.Len(); ++Start)
	{
		bool bMatches = true;
		for (int32 Index = 0; Index < Needle.Len(); ++Index)
		{
			if (WoTUpdateTransportAsciiLower(Haystack[Start + Index])
				!= WoTUpdateTransportAsciiLower(Needle[Index]))
			{
				bMatches = false;
				break;
			}
		}
		if (bMatches)
		{
			return true;
		}
	}
	return false;
}

bool WoTUpdateTransportParseAbsoluteUrl(
	FStringView Url, bool bRequireEntireUrlAscii,
	FWoTUpdateTransportParsedUrl& Out)
{
	Out = FWoTUpdateTransportParsedUrl();
	if (Url.IsEmpty())
	{
		return false;
	}
	for (const TCHAR Character : Url)
	{
		if (Character <= TEXT(' ') || Character == 0x7f
			|| Character == TEXT('\\')
			|| (bRequireEntireUrlAscii && Character > 0x7f))
		{
			return false;
		}
	}

	int32 SchemeEnd = INDEX_NONE;
	for (int32 Index = 0; Index < Url.Len(); ++Index)
	{
		const TCHAR Character = Url[Index];
		if (Character == TEXT(':'))
		{
			SchemeEnd = Index;
			break;
		}
		if (Character == TEXT('/') || Character == TEXT('?')
			|| Character == TEXT('#'))
		{
			return false;
		}
	}
	if (SchemeEnd <= 0 || SchemeEnd + 2 >= Url.Len()
		|| Url[SchemeEnd + 1] != TEXT('/')
		|| Url[SchemeEnd + 2] != TEXT('/'))
	{
		return false;
	}

	Out.Scheme.Reserve(SchemeEnd);
	for (int32 Index = 0; Index < SchemeEnd; ++Index)
	{
		const TCHAR Character = Url[Index];
		const bool bValid = Index == 0
			? WoTUpdateTransportIsAsciiAlpha(Character)
			: WoTUpdateTransportIsAsciiAlpha(Character)
				|| WoTUpdateTransportIsAsciiDigit(Character)
				|| Character == TEXT('+') || Character == TEXT('-')
				|| Character == TEXT('.');
		if (!bValid)
		{
			return false;
		}
		Out.Scheme.AppendChar(WoTUpdateTransportAsciiLower(Character));
	}

	const int32 AuthorityStart = SchemeEnd + 3;
	int32 AuthorityEnd = Url.Len();
	for (int32 Index = AuthorityStart; Index < Url.Len(); ++Index)
	{
		if (Url[Index] == TEXT('/') || Url[Index] == TEXT('?')
			|| Url[Index] == TEXT('#'))
		{
			AuthorityEnd = Index;
			break;
		}
	}
	if (AuthorityEnd == AuthorityStart)
	{
		return false;
	}
	if (AuthorityEnd < Url.Len() && Url[AuthorityEnd] == TEXT('#'))
	{
		return false;
	}
	for (int32 Index = AuthorityEnd; Index < Url.Len(); ++Index)
	{
		if (Url[Index] == TEXT('#'))
		{
			return false;
		}
	}

	const FStringView Authority =
		Url.Mid(AuthorityStart, AuthorityEnd - AuthorityStart);
	for (const TCHAR Character : Authority)
	{
		if (Character > 0x7f || Character == TEXT('@')
			|| Character == TEXT(':') || Character == TEXT('%')
			|| Character == TEXT('[') || Character == TEXT(']'))
		{
			return false;
		}
	}

	Out.Host.Reserve(Authority.Len());
	bool bHostIsOnlyDigitsAndDots = true;
	int32 LabelStart = 0;
	for (int32 Index = 0; Index <= Authority.Len(); ++Index)
	{
		if (Index == Authority.Len() || Authority[Index] == TEXT('.'))
		{
			const int32 LabelLength = Index - LabelStart;
			if (LabelLength <= 0
				|| Authority[LabelStart] == TEXT('-')
				|| Authority[Index - 1] == TEXT('-'))
			{
				return false;
			}
			const FStringView Label = Authority.Mid(LabelStart, LabelLength);
			if (Label.Len() >= 4
				&& WoTUpdateTransportContainsAsciiInsensitive(
					Label.Mid(0, 4), TEXT("xn--")))
			{
				return false;
			}
			LabelStart = Index + 1;
		}
		else
		{
			const TCHAR Character = Authority[Index];
			if (!WoTUpdateTransportIsAsciiAlpha(Character)
				&& !WoTUpdateTransportIsAsciiDigit(Character)
				&& Character != TEXT('-'))
			{
				return false;
			}
			if (!WoTUpdateTransportIsAsciiDigit(Character))
			{
				bHostIsOnlyDigitsAndDots = false;
			}
		}

		if (Index < Authority.Len())
		{
			Out.Host.AppendChar(
				WoTUpdateTransportAsciiLower(Authority[Index]));
		}
	}
	return !bHostIsOnlyDigitsAndDots;
}

bool WoTUpdateTransportIsRedirectStatus(int32 Status)
{
	return Status >= 300 && Status <= 399;
}

bool WoTUpdateTransportIsAllowedRedirectHost(FStringView Host)
{
	return Host.Equals(TEXT("release-assets.githubusercontent.com"),
			ESearchCase::CaseSensitive)
		|| Host.Equals(TEXT("objects.githubusercontent.com"),
			ESearchCase::CaseSensitive)
		|| Host.Equals(TEXT("github-releases.githubusercontent.com"),
			ESearchCase::CaseSensitive);
}

// A github.com path segment the release routes use: non-empty, not a dot
// segment, and only [A-Za-z0-9._+-] (so no '%', '?', '/', or non-ASCII).
bool WoTUpdateTransportIsPlainPathSegment(FStringView Segment)
{
	if (Segment.IsEmpty() || Segment.Equals(TEXT("."), ESearchCase::CaseSensitive)
		|| Segment.Equals(TEXT(".."), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (const TCHAR Character : Segment)
	{
		if (!WoTUpdateTransportIsAsciiAlpha(Character)
			&& !WoTUpdateTransportIsAsciiDigit(Character)
			&& Character != TEXT('.') && Character != TEXT('_')
			&& Character != TEXT('+') && Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

// The path segments of an https://github.com URL with no query, or false.
bool WoTUpdateTransportGitHubPathSegments(
	FStringView Url, TArray<FStringView>& OutSegments)
{
	OutSegments.Reset();
	FWoTUpdateTransportParsedUrl Parsed;
	if (!WoTUpdateTransportParseAbsoluteUrl(
			Url, /*bRequireEntireUrlAscii*/ true, Parsed)
		|| !Parsed.Scheme.Equals(TEXT("https"), ESearchCase::CaseSensitive)
		|| !Parsed.Host.Equals(TEXT("github.com"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	const int32 AuthorityStart = Parsed.Scheme.Len() + 3;
	int32 PathStart = INDEX_NONE;
	for (int32 Index = AuthorityStart; Index < Url.Len(); ++Index)
	{
		if (Url[Index] == TEXT('/'))
		{
			PathStart = Index;
			break;
		}
		if (Url[Index] == TEXT('?'))
		{
			return false;
		}
	}
	if (PathStart == INDEX_NONE)
	{
		return false;
	}
	const FStringView Path = Url.Mid(PathStart + 1);
	int32 SegmentStart = 0;
	for (int32 Index = 0; Index <= Path.Len(); ++Index)
	{
		if (Index == Path.Len() || Path[Index] == TEXT('/'))
		{
			const FStringView Segment =
				Path.Mid(SegmentStart, Index - SegmentStart);
			if (!WoTUpdateTransportIsPlainPathSegment(Segment))
			{
				return false;
			}
			OutSegments.Add(Segment);
			SegmentStart = Index + 1;
		}
	}
	return true;
}

// GitHub answers /<owner>/<repo>/releases/latest/download/<asset> with a 302
// to /<owner>/<repo>/releases/download/<tag>/<asset> on github.com itself.
// Follow exactly that: same owner, repo and asset, one
// concrete tag, no query. The asset is then served from the CDN hosts.
bool WoTUpdateTransportIsLatestToTagHop(
	FStringView PreviousUrl, FStringView Location)
{
	TArray<FStringView> From;
	TArray<FStringView> To;
	if (!WoTUpdateTransportGitHubPathSegments(PreviousUrl, From)
		|| !WoTUpdateTransportGitHubPathSegments(Location, To)
		|| From.Num() != 6 || To.Num() != 6)
	{
		return false;
	}
	auto Same = [](FStringView A, FStringView B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	};
	return Same(From[2], TEXT("releases")) && Same(From[3], TEXT("latest"))
		&& Same(From[4], TEXT("download"))
		&& Same(To[2], TEXT("releases")) && Same(To[3], TEXT("download"))
		&& !Same(To[4], TEXT("latest"))
		&& Same(From[0], To[0]) && Same(From[1], To[1])
		&& Same(From[5], To[5]);
}

FWoTBootstrapStatus WoTUpdateTransportRefusal(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::ServiceUnavailable, Message));
}

double WoTUpdateTransportNow(TFunction<double()>& NowSeconds)
{
	return NowSeconds ? NowSeconds() : FPlatformTime::Seconds();
}
} // namespace

bool WoTIsAllowedInitialReleaseUrl(FStringView Url)
{
	FWoTUpdateTransportParsedUrl Parsed;
	return WoTUpdateTransportParseAbsoluteUrl(
			Url, /*bRequireEntireUrlAscii*/ true, Parsed)
		&& Parsed.Scheme.Equals(TEXT("https"), ESearchCase::CaseSensitive)
		&& Parsed.Host.Equals(TEXT("github.com"), ESearchCase::CaseSensitive);
}

EWoTRedirectVerdict WoTJudgeRedirectHop(
	FStringView Location, int32 HopIndex)
{
	// The hop budget is deliberately checked before parsing the Location.
	if (HopIndex <= 0 || HopIndex >= 4)
	{
		return EWoTRedirectVerdict::Refuse;
	}

	FWoTUpdateTransportParsedUrl Parsed;
	if (!WoTUpdateTransportParseAbsoluteUrl(
			Location, /*bRequireEntireUrlAscii*/ false, Parsed)
		|| !Parsed.Scheme.Equals(TEXT("https"), ESearchCase::CaseSensitive)
		|| !WoTUpdateTransportIsAllowedRedirectHost(Parsed.Host))
	{
		return EWoTRedirectVerdict::Refuse;
	}
	return EWoTRedirectVerdict::Follow;
}

EWoTRedirectVerdict WoTJudgeRedirectHop(
	FStringView Location, int32 HopIndex, FStringView PreviousUrl)
{
	if (HopIndex == 1 && WoTUpdateTransportIsLatestToTagHop(PreviousUrl, Location))
	{
		return EWoTRedirectVerdict::Follow;
	}
	return WoTJudgeRedirectHop(Location, HopIndex);
}

FWoTBootstrapStatus WoTFetchReleaseObject(IWoTHttpHopExecutor& Hops,
	FStringView Url, double AbsoluteDeadlineSeconds,
	TFunction<double()> NowSeconds, TArray<uint8>& OutBody,
	FWoTHttpHopResponse& OutTerminal, IWoTHttpBodySink* BodySink)
{
	OutBody.Reset();
	OutTerminal = FWoTHttpHopResponse();
	if (!WoTIsAllowedInitialReleaseUrl(Url))
	{
		return WoTUpdateTransportRefusal(
			TEXT("the initial release URL is outside the HTTPS github.com policy"));
	}

	FString CurrentUrl(Url);
	int32 RedirectHopIndex = 1;

	for (;;)
	{
		if (WoTUpdateTransportNow(NowSeconds) >= AbsoluteDeadlineSeconds)
		{
			return WoTUpdateTransportRefusal(
				TEXT("the shared absolute HTTP deadline is exhausted"));
		}

		FWoTHttpHopRequest Request;
		Request.Url = CurrentUrl;
		Request.AbsoluteDeadlineSeconds = AbsoluteDeadlineSeconds;
		Request.BodySink = BodySink;
		OutTerminal = FWoTHttpHopResponse();
		FWoTBootstrapStatus Executed = Hops.ExecuteHop(Request, OutTerminal);
		if (Executed.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Executed).GetError());
		}
		if (OutTerminal.Status == 0)
		{
			return WoTUpdateTransportRefusal(
				TEXT("the HTTP hop failed before receiving a response"));
		}

		if (WoTUpdateTransportIsRedirectStatus(OutTerminal.Status)
			&& !OutTerminal.Location.IsEmpty())
		{
			if (WoTJudgeRedirectHop(
					OutTerminal.Location, RedirectHopIndex, CurrentUrl)
				!= EWoTRedirectVerdict::Follow)
			{
				return WoTUpdateTransportRefusal(
					TEXT("the HTTP redirect escaped the release route policy"));
			}
			CurrentUrl = OutTerminal.Location;
			++RedirectHopIndex;
			continue;
		}

		// Socket executors stream when a sink is present. Small scripted/test
		// executors may still return a buffered body; preserve that compatible
		// shape rather than silently dropping it.
		if (BodySink == nullptr || !OutTerminal.Body.IsEmpty())
		{
			OutBody = OutTerminal.Body;
		}
		return FWoTBootstrapStatus::Success();
	}
}

FWoTBootstrapStatus WoTFetchSignedPair(IWoTHttpHopExecutor& Hops,
	FStringView JsonUrl, double AbsoluteDeadlineSeconds,
	TFunction<double()> NowSeconds, TArray<uint8>& OutJson,
	TArray<uint8>& OutSignature)
{
	OutJson.Reset();
	OutSignature.Reset();

	auto SharedNow = [&NowSeconds]()
	{
		return WoTUpdateTransportNow(NowSeconds);
	};
	TArray<uint8> Json;
	FWoTHttpHopResponse JsonTerminal;
	FWoTBootstrapStatus JsonStatus = WoTFetchReleaseObject(Hops,
		JsonUrl, AbsoluteDeadlineSeconds, SharedNow, Json, JsonTerminal);
	if (JsonStatus.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(JsonStatus).GetError());
	}
	if (JsonTerminal.Status != 200)
	{
		return WoTUpdateTransportRefusal(
			TEXT("the signed JSON fetch did not terminate with HTTP 200"));
	}

	FString SignatureUrl(JsonUrl);
	SignatureUrl += TEXT(".sig");
	TArray<uint8> Signature;
	FWoTHttpHopResponse SignatureTerminal;
	FWoTBootstrapStatus SignatureStatus = WoTFetchReleaseObject(Hops,
		SignatureUrl, AbsoluteDeadlineSeconds,
		SharedNow, Signature, SignatureTerminal);
	if (SignatureStatus.IsError())
	{
		return FWoTBootstrapStatus::Error(
			MoveTemp(SignatureStatus).GetError());
	}
	if (SignatureTerminal.Status != 200)
	{
		return WoTUpdateTransportRefusal(
			TEXT("the signature fetch did not terminate with HTTP 200"));
	}

	OutJson = MoveTemp(Json);
	OutSignature = MoveTemp(Signature);
	return FWoTBootstrapStatus::Success();
}
