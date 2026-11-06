#include "WoTGameEngine.h"

#include "EngineLogs.h"
#include "Logging/LogScopedVerbosityOverride.h"
#include "Misc/CommandLine.h"
#include "Misc/StatusLog.h"
#include "Net/NetworkProfiler.h"
#include "Online/WoTInternetPlay.h"

namespace
{
constexpr FStringView WoTBootstrapUrl =
	TEXTVIEW("/Game/WoT/Maps/WoTBootstrap");
constexpr FStringView WoTUncookedDefaultFrontEndUrl =
	TEXTVIEW("/Game/WoT/Maps/WoTFrontEnd");

bool WoTReplayTokenAt(FStringView CommandLine, int32 Offset,
	FStringView Token)
{
	return Offset >= 0 && Offset + Token.Len() <= CommandLine.Len()
		&& CommandLine.Mid(Offset, Token.Len()).Equals(
			Token, ESearchCase::IgnoreCase);
}

bool WoTIsSensitiveTravelOption(FStringView Name)
{
	return Name.Equals(TEXTVIEW("Password"), ESearchCase::IgnoreCase)
		|| Name.Equals(TEXTVIEW("PasswordToken"), ESearchCase::IgnoreCase)
		|| Name.Equals(TEXTVIEW("AdminPassword"), ESearchCase::IgnoreCase)
		|| Name.Equals(TEXTVIEW("GamePassword"), ESearchCase::IgnoreCase);
}

bool WoTHasSensitiveTravelOption(const FURL& URL)
{
	for (const FString& Option : URL.Op)
	{
		const int32 Equals = Option.Find(TEXT("="));
		const FStringView Name(Option.GetCharArray().GetData(),
			Equals == INDEX_NONE ? Option.Len() : Equals);
		if (WoTIsSensitiveTravelOption(Name))
		{
			return true;
		}
	}
	return false;
}

#if USE_NETWORK_PROFILER
FURL WoTRedactedTravelURL(const FURL& URL)
{
	FURL Result = URL;
	for (FString& Option : Result.Op)
	{
		const int32 Equals = Option.Find(TEXT("="));
		const FStringView Name(Option.GetCharArray().GetData(),
			Equals == INDEX_NONE ? Option.Len() : Equals);
		if (WoTIsSensitiveTravelOption(Name))
		{
			Option = FString(Name) + TEXT("=<redacted>");
		}
	}
	return Result;
}

class FWoTSensitiveNetworkProfilerScope
{
public:
	explicit FWoTSensitiveNetworkProfilerScope(const FURL& URL)
		: RedactedURL(WoTRedactedTravelURL(URL))
		, bRestoreTracking(GNetworkProfiler.IsTrackingEnabled())
	{
		if (bRestoreTracking)
		{
			// UEngine records the supplied FURL in the .nprof header as well as
			// LogNet. Keep the real connection URL away from both destinations.
			GNetworkProfiler.EnableTracking(false);
		}
	}

	~FWoTSensitiveNetworkProfilerScope()
	{
		if (bRestoreTracking)
		{
			GNetworkProfiler.EnableTracking(true);
			GNetworkProfiler.TrackSessionChange(true, RedactedURL);
		}
	}

private:
	FURL RedactedURL;
	bool bRestoreTracking = false;
};
#endif
}

TOptional<FString> WoTGuardedFirstBrowseUrl(bool bFirstBrowse)
{
	return WoTGuardedBrowseReplacement(bFirstBrowse, FStringView());
}

TOptional<FString> WoTGuardedBrowseReplacement(
	bool bFirstBrowse, FStringView RequestedMap, bool bNetworkURL)
{
	if (bFirstBrowse || (!bNetworkURL && RequestedMap.Equals(
		WoTUncookedDefaultFrontEndUrl, ESearchCase::CaseSensitive)))
	{
		return FString(WoTBootstrapUrl);
	}
	return {};
}

FString WoTGuardedSanitizeReplayCommandLine(FStringView CommandLine)
{
	static constexpr FStringView Replay = TEXTVIEW("-REPLAY=");
	static constexpr FStringView ReplayPlaylist = TEXTVIEW("-REPLAYPLAYLIST=");
	FString Sanitized;
	int32 CopiedThrough = 0;
	int32 Cursor = 0;
	while (Cursor < CommandLine.Len())
	{
		const bool bAtTokenBoundary = Cursor == 0
			|| FChar::IsWhitespace(CommandLine[Cursor - 1]);
		const bool bWholeTokenQuoted = bAtTokenBoundary
			&& CommandLine[Cursor] == TEXT('"');
		const int32 OptionOffset = Cursor + (bWholeTokenQuoted ? 1 : 0);
		FStringView MatchedToken;
		if (bAtTokenBoundary
			&& WoTReplayTokenAt(CommandLine, OptionOffset, ReplayPlaylist))
		{
			MatchedToken = ReplayPlaylist;
		}
		else if (bAtTokenBoundary
			&& WoTReplayTokenAt(CommandLine, OptionOffset, Replay))
		{
			MatchedToken = Replay;
		}
		if (MatchedToken.IsEmpty())
		{
			++Cursor;
			continue;
		}

		int32 TokenEnd = OptionOffset + MatchedToken.Len();
		bool bInsideQuotes = bWholeTokenQuoted;
		if (!bInsideQuotes && TokenEnd < CommandLine.Len()
			&& CommandLine[TokenEnd] == TEXT('"'))
		{
			bInsideQuotes = true;
			++TokenEnd;
		}
		while (TokenEnd < CommandLine.Len())
		{
			if (bInsideQuotes && CommandLine[TokenEnd] == TEXT('"'))
			{
				bInsideQuotes = false;
				++TokenEnd;
				if (bWholeTokenQuoted)
				{
					break;
				}
				continue;
			}
			if (!bInsideQuotes && FChar::IsWhitespace(CommandLine[TokenEnd]))
			{
				break;
			}
			++TokenEnd;
		}

		Sanitized.Append(CommandLine.Mid(CopiedThrough, Cursor - CopiedThrough));
		CopiedThrough = TokenEnd;
		Cursor = TokenEnd;
	}
	if (CopiedThrough == 0)
	{
		return FString(CommandLine);
	}
	Sanitized.Append(CommandLine.Mid(CopiedThrough));
	return Sanitized;
}

void UWoTGameEngine::Init(IEngineLoop* InEngineLoop)
{
#if WOT_GUARDED_BOOTSTRAP
	const FString OriginalCommandLine(FCommandLine::Get());
	const FString Sanitized =
		WoTGuardedSanitizeReplayCommandLine(OriginalCommandLine);
	if (Sanitized != OriginalCommandLine)
	{
		FCommandLine::Set(*Sanitized);
		UE_LOG(LogInit, Display,
			TEXT("WOT-GUARDED-REPLAY-ROUTE-REMOVED"));
	}
#if UE_BUILD_SHIPPING
	// Internet play: Shipping binds its EOS artifact, deployment and build
	// bucket to the attested config. OnlineSubsystemEOS/OnlineSubsystem read
	// their overrides lazily (first EOS platform creation, first session),
	// which is always after this point; -NoEOS (disable only) survives.
	const FString BeforeEOSStrip(FCommandLine::Get());
	const FString EOSSanitized =
		FWoTEOSConfig::StripShippingCommandLineOverrides(BeforeEOSStrip);
	if (EOSSanitized != BeforeEOSStrip)
	{
		FCommandLine::Set(*EOSSanitized);
	}
#endif
#endif
	Super::Init(InEngineLoop);
}

EBrowseReturnVal::Type UWoTGameEngine::Browse(FWorldContext& WorldContext,
	FURL URL, FString& Error)
{
#if WOT_GUARDED_BOOTSTRAP
	if (WorldContext.WorldType == EWorldType::Game)
	{
		const bool bFirstBrowse = !bHasBrowsedGuardedGameWorld;
		bHasBrowsedGuardedGameWorld = true;
		if (TOptional<FString> Replacement =
			WoTGuardedBrowseReplacement(bFirstBrowse, URL.Map,
				!URL.Host.IsEmpty()))
		{
			URL = FURL(nullptr, *Replacement.GetValue(), TRAVEL_Absolute);
			URL.Host.Reset();
			URL.Op.Reset();
			URL.Portal.Reset();
			URL.RedirectURL.Reset();
		}
	}
#endif
	if (WoTHasSensitiveTravelOption(URL))
	{
#if !NO_LOGGING
		// NO_LOGGING builds (Shipping) compile these categories out, so there is
		// nothing to mute and FLogScopedVerbosityOverride cannot bind to them.
		FLogScopedVerbosityOverride StatusLogs(
			&LogGlobalStatus, ELogVerbosity::Warning);
		FLogScopedVerbosityOverride NetworkLogs(
			&LogNet, ELogVerbosity::Warning);
#endif
#if USE_NETWORK_PROFILER
		FWoTSensitiveNetworkProfilerScope NetworkProfiler(URL);
#endif
		return Super::Browse(WorldContext, MoveTemp(URL), Error);
	}
	return Super::Browse(WorldContext, MoveTemp(URL), Error);
}

bool UWoTGameEngine::LoadMap(FWorldContext& WorldContext, FURL URL,
	UPendingNetGame* Pending, FString& Error)
{
#if WOT_GUARDED_BOOTSTRAP
	if (WorldContext.WorldType == EWorldType::Game)
	{
		if (TOptional<FString> Replacement =
			WoTGuardedBrowseReplacement(false, URL.Map, !URL.Host.IsEmpty()))
		{
			URL = FURL(nullptr, *Replacement.GetValue(), TRAVEL_Absolute);
			URL.Host.Reset();
			URL.Op.Reset();
			URL.Portal.Reset();
			URL.RedirectURL.Reset();
		}
	}
#endif
	// The source-engine hook inside UEngine::LoadMap owns initial/host/client
	// ordering, and the independent World.cpp hook owns both seamless roles.
	// Calling the base implementation therefore reaches the one guarded
	// dispatcher without introducing a later LoadMap-only transition.
	if (WoTHasSensitiveTravelOption(URL))
	{
#if !NO_LOGGING
		// NO_LOGGING builds (Shipping) compile these categories out, so there is
		// nothing to mute and FLogScopedVerbosityOverride cannot bind to them.
		FLogScopedVerbosityOverride LoadLogs(
			&LogLoad, ELogVerbosity::Warning);
		FLogScopedVerbosityOverride NetworkLogs(
			&LogNet, ELogVerbosity::Warning);
#endif
#if USE_NETWORK_PROFILER
		FWoTSensitiveNetworkProfilerScope NetworkProfiler(URL);
#endif
		return Super::LoadMap(
			WorldContext, MoveTemp(URL), Pending, Error);
	}
	return Super::LoadMap(WorldContext, MoveTemp(URL), Pending, Error);
}
