// Pure, socket-free HTTPS route-policy and logical-fetch specs. Every helper
// name is spec-prefixed because WoT test sources share unity translation units.
#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "WoTUpdateTransports/WoTUpdateTransportPolicy.h"

namespace
{
constexpr EAutomationTestFlags WoTUpdateTransportPolicySpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;

constexpr TCHAR WoTUpdateTransportPolicySpecInitialUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/v1/object.bin");
constexpr double WoTUpdateTransportPolicySpecDeadline = 100.0;

struct FWoTUpdateTransportPolicySpecScriptStep
{
	FWoTHttpHopResponse Response;
	bool bTransportFailure = false;
};

FWoTUpdateTransportPolicySpecScriptStep
WoTUpdateTransportPolicySpecStep(int32 Status,
	const TCHAR* Location = TEXT(""), const ANSICHAR* Body = "")
{
	FWoTUpdateTransportPolicySpecScriptStep Step;
	Step.Response.Status = Status;
	Step.Response.Location = Location;
	while (*Body != '\0')
	{
		Step.Response.Body.Add(static_cast<uint8>(*Body));
		++Body;
	}
	return Step;
}

FWoTUpdateTransportPolicySpecScriptStep
WoTUpdateTransportPolicySpecTransportFailure()
{
	FWoTUpdateTransportPolicySpecScriptStep Step;
	Step.bTransportFailure = true;
	return Step;
}

TArray<uint8> WoTUpdateTransportPolicySpecBytes(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	while (*Text != '\0')
	{
		Bytes.Add(static_cast<uint8>(*Text));
		++Text;
	}
	return Bytes;
}

class FWoTUpdateTransportPolicySpecExecutor final
	: public IWoTHttpHopExecutor
{
public:
	explicit FWoTUpdateTransportPolicySpecExecutor(
		TArray<FWoTUpdateTransportPolicySpecScriptStep> InScript)
		: Script(MoveTemp(InScript))
	{
	}

	FWoTBootstrapStatus ExecuteHop(const FWoTHttpHopRequest& Request,
		FWoTHttpHopResponse& OutResponse) override
	{
		Requests.Add(Request);
		if (!Script.IsValidIndex(Cursor))
		{
			OutResponse = FWoTHttpHopResponse();
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("the transport-policy spec script was exhausted")));
		}

		const FWoTUpdateTransportPolicySpecScriptStep& Step = Script[Cursor++];
		OutResponse = Step.Response;
		if (Step.bTransportFailure)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("scripted transport failure")));
		}
		return FWoTBootstrapStatus::Success();
	}

	TArray<FWoTHttpHopRequest> Requests;

private:
	TArray<FWoTUpdateTransportPolicySpecScriptStep> Script;
	int32 Cursor = 0;
};

class FWoTUpdateTransportPolicySpecClock
{
public:
	explicit FWoTUpdateTransportPolicySpecClock(TArray<double> InTimes)
		: Times(MoveTemp(InTimes))
	{
	}

	double Next()
	{
		if (Times.IsEmpty())
		{
			return 0.0;
		}
		const int32 Index = FMath::Min(Cursor, Times.Num() - 1);
		++Cursor;
		return Times[Index];
	}

private:
	TArray<double> Times;
	int32 Cursor = 0;
};

TFunction<double()> WoTUpdateTransportPolicySpecNow(
	FWoTUpdateTransportPolicySpecClock& Clock)
{
	return [&Clock]()
	{
		return Clock.Next();
	};
}

bool WoTUpdateTransportPolicySpecAssertRequests(
	FAutomationTestBase& Test,
	const FWoTUpdateTransportPolicySpecExecutor& Hops,
	std::initializer_list<const TCHAR*> ExpectedUrls,
	const TCHAR* ExpectedRange = TEXT(""))
{
	bool bOkay = Test.TestEqual(TEXT("the exact hop count is recorded"),
		Hops.Requests.Num(), static_cast<int32>(ExpectedUrls.size()));
	int32 Index = 0;
	for (const TCHAR* ExpectedUrl : ExpectedUrls)
	{
		if (!Hops.Requests.IsValidIndex(Index))
		{
			break;
		}
		const FWoTHttpHopRequest& Request = Hops.Requests[Index];
		bOkay &= Test.TestEqual(
			*FString::Printf(TEXT("hop %d preserves the exact URL"), Index + 1),
			Request.Url, FString(ExpectedUrl));

		bOkay &= Test.TestEqual(
			*FString::Printf(TEXT("hop %d keeps the shared absolute deadline"),
				Index + 1),
			Request.AbsoluteDeadlineSeconds,
			WoTUpdateTransportPolicySpecDeadline);
		++Index;
	}
	// The request struct is the entire request surface. Its recorded Url,
	// RangeHeader, and deadline prove there is no credential, cookie,
	// Authorization, or other caller-controlled header channel.
	return bOkay;
}

FWoTBootstrapStatus WoTUpdateTransportPolicySpecFetch(
	FWoTUpdateTransportPolicySpecExecutor& Hops,
	TFunction<double()> NowSeconds, TArray<uint8>& OutBody,
	FWoTHttpHopResponse& OutTerminal, uint64 RangeOffset = 0,
	FStringView Url = WoTUpdateTransportPolicySpecInitialUrl)
{
	return WoTFetchReleaseObject(Hops, Url,
		WoTUpdateTransportPolicySpecDeadline, MoveTemp(NowSeconds),
		OutBody, OutTerminal);
}

FWoTBootstrapStatus WoTUpdateTransportPolicySpecFetchAtZero(
	FWoTUpdateTransportPolicySpecExecutor& Hops, TArray<uint8>& OutBody,
	FWoTHttpHopResponse& OutTerminal, uint64 RangeOffset = 0,
	FStringView Url = WoTUpdateTransportPolicySpecInitialUrl)
{
	FWoTUpdateTransportPolicySpecClock Clock({0.0});
	return WoTUpdateTransportPolicySpecFetch(Hops,
		WoTUpdateTransportPolicySpecNow(Clock), OutBody, OutTerminal,
		RangeOffset, Url);
}

bool WoTUpdateTransportPolicySpecAssertRefusedLocation(
	FAutomationTestBase& Test, const TCHAR* Location)
{
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, Location),
	});
	TArray<uint8> Body = WoTUpdateTransportPolicySpecBytes("sentinel");
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	Test.TestTrue(TEXT("the Location is refused"), Status.IsError());
	Test.TestTrue(TEXT("a refused Location leaves the body empty"),
		Body.IsEmpty());
	Test.TestEqual(TEXT("the last response remains observable"),
		Terminal.Location, FString(Location));
	WoTUpdateTransportPolicySpecAssertRequests(Test, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl});
	return !Test.HasAnyErrors();
}

bool WoTUpdateTransportPolicySpecAssertRefusedInitial(
	FAutomationTestBase& Test, const TCHAR* Url)
{
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "forbidden"),
	});
	TArray<uint8> Body = WoTUpdateTransportPolicySpecBytes("sentinel");
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal, 0, Url);
	Test.TestTrue(TEXT("the initial URL is refused"), Status.IsError());
	Test.TestTrue(TEXT("an initial refusal leaves the body empty"),
		Body.IsEmpty());
	Test.TestEqual(TEXT("an initial refusal issues no request"),
		Hops.Requests.Num(), 0);
	return !Test.HasAnyErrors();
}

bool WoTUpdateTransportPolicySpecAssertDeadline(
	FAutomationTestBase& Test, TArray<double> Times,
	TArray<FWoTUpdateTransportPolicySpecScriptStep> Script,
	std::initializer_list<const TCHAR*> ExpectedUrls)
{
	FWoTUpdateTransportPolicySpecExecutor Hops(MoveTemp(Script));
	FWoTUpdateTransportPolicySpecClock Clock(MoveTemp(Times));
	TArray<uint8> Body = WoTUpdateTransportPolicySpecBytes("sentinel");
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status = WoTUpdateTransportPolicySpecFetch(
		Hops, WoTUpdateTransportPolicySpecNow(Clock), Body, Terminal);
	Test.TestTrue(TEXT("deadline exhaustion is a refusal"), Status.IsError());
	Test.TestTrue(TEXT("deadline refusal leaves the body empty"),
		Body.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(
		Test, Hops, ExpectedUrls);
	return !Test.HasAnyErrors();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyZeroHopSpec,
	"WoT.Update.TransportPolicy.PermittedHops.Zero",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyZeroHopSpec::RunTest(const FString&)
{
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "terminal-zero"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestFalse(TEXT("a direct terminal response succeeds"), Status.IsError());
	TestEqual(TEXT("the terminal status propagates"), Terminal.Status, 200);
	TestTrue(TEXT("the terminal body propagates"),
		Body == WoTUpdateTransportPolicySpecBytes("terminal-zero"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyOneHopSpec,
	"WoT.Update.TransportPolicy.PermittedHops.One",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyOneHopSpec::RunTest(const FString&)
{
	const TCHAR* Redirected = TEXT(
		"https://release-assets.githubusercontent.com/one/object.bin?token=one");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, Redirected),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "terminal-one"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestFalse(TEXT("one permitted hop succeeds"), Status.IsError());
	TestEqual(TEXT("the terminal status propagates"), Terminal.Status, 200);
	TestTrue(TEXT("the terminal body propagates"),
		Body == WoTUpdateTransportPolicySpecBytes("terminal-one"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, Redirected});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyTwoHopSpec,
	"WoT.Update.TransportPolicy.PermittedHops.Two",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyTwoHopSpec::RunTest(const FString&)
{
	const TCHAR* First = TEXT(
		"https://release-assets.githubusercontent.com/two/first");
	const TCHAR* Second = TEXT(
		"https://objects.githubusercontent.com/two/second?key=value");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, First),
		WoTUpdateTransportPolicySpecStep(307, Second),
		WoTUpdateTransportPolicySpecStep(206, TEXT(""), "terminal-two"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestFalse(TEXT("two permitted hops succeed"), Status.IsError());
	TestEqual(TEXT("the terminal status propagates"), Terminal.Status, 206);
	TestTrue(TEXT("the terminal body propagates"),
		Body == WoTUpdateTransportPolicySpecBytes("terminal-two"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, First, Second});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyThreeHopSpec,
	"WoT.Update.TransportPolicy.PermittedHops.Three",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyThreeHopSpec::RunTest(const FString&)
{
	const TCHAR* First = TEXT(
		"https://release-assets.githubusercontent.com/three/first");
	const TCHAR* Second = TEXT(
		"https://objects.githubusercontent.com/three/second");
	const TCHAR* Third = TEXT(
		"https://github-releases.githubusercontent.com/three/terminal");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(301, First),
		WoTUpdateTransportPolicySpecStep(302, Second),
		WoTUpdateTransportPolicySpecStep(308, Third),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "terminal-three"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestFalse(TEXT("three permitted hops succeed"), Status.IsError());
	TestEqual(TEXT("the terminal status propagates"), Terminal.Status, 200);
	TestTrue(TEXT("the terminal body propagates"),
		Body == WoTUpdateTransportPolicySpecBytes("terminal-three"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, First, Second, Third});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyFourthHopSpec,
	"WoT.Update.TransportPolicy.Refuse.FourthHopBeforeParse",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyFourthHopSpec::RunTest(const FString&)
{
	const TCHAR* First =
		TEXT("https://release-assets.githubusercontent.com/fourth/one");
	const TCHAR* Second =
		TEXT("https://objects.githubusercontent.com/fourth/two");
	const TCHAR* Third =
		TEXT("https://github-releases.githubusercontent.com/fourth/three");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, First),
		WoTUpdateTransportPolicySpecStep(302, Second),
		WoTUpdateTransportPolicySpecStep(302, Third),
		// Deliberately malformed: the fourth-hop budget must win before parsing.
		WoTUpdateTransportPolicySpecStep(302, TEXT("not a URL")),
	});
	TArray<uint8> Body = WoTUpdateTransportPolicySpecBytes("sentinel");
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestTrue(TEXT("a fourth redirect hop is refused"), Status.IsError());
	TestTrue(TEXT("the refusal leaves the body empty"), Body.IsEmpty());
	TestEqual(TEXT("the fourth response remains observable"),
		Terminal.Location, FString(TEXT("not a URL")));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, First, Second, Third});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyLoopSpec,
	"WoT.Update.TransportPolicy.Refuse.RedirectLoopByBudget",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyLoopSpec::RunTest(const FString&)
{
	const TCHAR* Loop =
		TEXT("https://objects.githubusercontent.com/loop/object.bin");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, Loop),
		WoTUpdateTransportPolicySpecStep(302, Loop),
		WoTUpdateTransportPolicySpecStep(302, Loop),
		WoTUpdateTransportPolicySpecStep(302, Loop),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestTrue(TEXT("a redirect loop is stopped by the hop budget"),
		Status.IsError());
	TestTrue(TEXT("loop refusal leaves the body empty"), Body.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, Loop, Loop, Loop});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDowngradeHopOneSpec,
	"WoT.Update.TransportPolicy.Refuse.HttpDowngrade.Hop1",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDowngradeHopOneSpec::RunTest(const FString&)
{
	return WoTUpdateTransportPolicySpecAssertRefusedLocation(*this,
		TEXT("http://release-assets.githubusercontent.com/one"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDowngradeHopTwoSpec,
	"WoT.Update.TransportPolicy.Refuse.HttpDowngrade.Hop2",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDowngradeHopTwoSpec::RunTest(const FString&)
{
	const TCHAR* First =
		TEXT("https://release-assets.githubusercontent.com/two/one");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, First),
		WoTUpdateTransportPolicySpecStep(302,
			TEXT("http://objects.githubusercontent.com/two")),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestTrue(TEXT("an HTTP downgrade at hop two is refused"), Status.IsError());
	TestTrue(TEXT("downgrade refusal leaves the body empty"), Body.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, First});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDowngradeHopThreeSpec,
	"WoT.Update.TransportPolicy.Refuse.HttpDowngrade.Hop3",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDowngradeHopThreeSpec::RunTest(const FString&)
{
	const TCHAR* First =
		TEXT("https://release-assets.githubusercontent.com/three/one");
	const TCHAR* Second =
		TEXT("https://objects.githubusercontent.com/three/two");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, First),
		WoTUpdateTransportPolicySpecStep(302, Second),
		WoTUpdateTransportPolicySpecStep(302,
			TEXT("http://github-releases.githubusercontent.com/three")),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestTrue(TEXT("an HTTP downgrade at hop three is refused"),
		Status.IsError());
	TestTrue(TEXT("downgrade refusal leaves the body empty"), Body.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, First, Second});
	return !HasAnyErrors();
}

#define WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC( \
	ClassName, TestPath, BadUrl) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, TestPath, \
		WoTUpdateTransportPolicySpecFlags) \
	bool ClassName::RunTest(const FString&) \
	{ \
		return WoTUpdateTransportPolicySpecAssertRefusedInitial(*this, BadUrl); \
	}

WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyNonHttpsInitialSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.NonHttps",
	TEXT("http://github.com/aleiby/wot/releases/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyInitialSuffixHostSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.SuffixConfusion",
	TEXT("https://github.com.attacker.invalid/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyInitialSubdomainSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.Subdomain",
	TEXT("https://api.github.com/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyInitialPortSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.ExplicitPort",
	TEXT("https://github.com:8443/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyInitialUserInfoSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.UserInfo",
	TEXT("https://user@github.com/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyInitialIpLiteralSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.IpLiteral",
	TEXT("https://127.0.0.1/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyInitialTrailingDotSpec,
	"WoT.Update.TransportPolicy.Refuse.Initial.TrailingDot",
	TEXT("https://github.com./object.bin"))

#undef WOT_UPDATE_TRANSPORT_POLICY_INITIAL_REFUSAL_SPEC

#define WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC( \
	ClassName, TestPath, BadLocation) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, TestPath, \
		WoTUpdateTransportPolicySpecFlags) \
	bool ClassName::RunTest(const FString&) \
	{ \
		return WoTUpdateTransportPolicySpecAssertRefusedLocation( \
			*this, BadLocation); \
	}

WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationUserInfoSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.UserInfo",
	TEXT("https://user@release-assets.githubusercontent.com/object.bin"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationSuffixHostSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.SuffixConfusion",
	TEXT("https://release-assets.githubusercontent.com.attacker.invalid/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationPrefixHostSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.PrefixConfusion",
	TEXT("https://attacker-release-assets.githubusercontent.com/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationRawOriginSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.UnlistedRawOrigin",
	TEXT("https://raw.githubusercontent.com/aleiby/wot/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationPercentHostSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.PercentEncodedHost",
	TEXT("https://release%2dassets.githubusercontent.com/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationPunycodeSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.PunycodeHost",
	TEXT("https://xn--release-assets.githubusercontent.com/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationSchemeRelativeSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.SchemeRelative",
	TEXT("//release-assets.githubusercontent.com/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationRelativeSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.Relative",
	TEXT("../release/object"))
WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC(
	FWoTUpdateTransportPolicyLocationMalformedSpec,
	"WoT.Update.TransportPolicy.Refuse.Location.Malformed",
	TEXT("https:///missing-host"))

#undef WOT_UPDATE_TRANSPORT_POLICY_LOCATION_REFUSAL_SPEC

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyOpaqueTailSpec,
	"WoT.Update.TransportPolicy.Preserve.OpaquePathAndQuery",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyOpaqueTailSpec::RunTest(const FString&)
{
	const TCHAR* SignedLocation = TEXT(
		"https://objects.githubusercontent.com/releases/download/WoT.bin"
		"?X-Amz-Algorithm=AWS4-HMAC-SHA256"
		"&X-Amz-Credential=opaque%2F20260728%2Fus-east-1"
		"&X-Amz-Signature=AbC012%2B%2F%3D");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, SignedLocation),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "opaque-terminal"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestFalse(TEXT("the signed-style redirect succeeds"), Status.IsError());
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl, SignedLocation});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyPairExactUrlsSpec,
	"WoT.Update.TransportPolicy.Pair.ExactUrls",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyPairExactUrlsSpec::RunTest(const FString&)
{
	const TCHAR* JsonUrl = TEXT(
		"https://github.com/aleiby/wot/releases/latest/download/announcement.json");
	const TCHAR* SignatureUrl = TEXT(
		"https://github.com/aleiby/wot/releases/latest/download/announcement.json.sig");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "json"),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "signature"),
	});
	FWoTUpdateTransportPolicySpecClock Clock({0.0});
	TArray<uint8> Json;
	TArray<uint8> Signature;
	FWoTBootstrapStatus Status = WoTFetchSignedPair(Hops, JsonUrl,
		WoTUpdateTransportPolicySpecDeadline,
		WoTUpdateTransportPolicySpecNow(Clock), Json, Signature);
	TestFalse(TEXT("the exact pair succeeds"), Status.IsError());
	TestTrue(TEXT("the JSON body propagates"),
		Json == WoTUpdateTransportPolicySpecBytes("json"));
	TestTrue(TEXT("the signature body propagates"),
		Signature == WoTUpdateTransportPolicySpecBytes("signature"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{JsonUrl, SignatureUrl});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyPairSecondFailureSpec,
	"WoT.Update.TransportPolicy.Pair.SecondFailureClearsBoth",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyPairSecondFailureSpec::RunTest(const FString&)
{
	const TCHAR* JsonUrl = TEXT("https://github.com/release/announcement.json");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "json"),
		WoTUpdateTransportPolicySpecTransportFailure(),
	});
	FWoTUpdateTransportPolicySpecClock Clock({0.0});
	TArray<uint8> Json = WoTUpdateTransportPolicySpecBytes("old-json");
	TArray<uint8> Signature = WoTUpdateTransportPolicySpecBytes("old-signature");
	FWoTBootstrapStatus Status = WoTFetchSignedPair(Hops, JsonUrl,
		WoTUpdateTransportPolicySpecDeadline,
		WoTUpdateTransportPolicySpecNow(Clock), Json, Signature);
	TestTrue(TEXT("the second transport failure fails the pair"),
		Status.IsError());
	TestTrue(TEXT("the JSON output is empty"), Json.IsEmpty());
	TestTrue(TEXT("the signature output is empty"), Signature.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{JsonUrl, TEXT("https://github.com/release/announcement.json.sig")});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyTerminal404Spec,
	"WoT.Update.TransportPolicy.Terminal.Status404",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyTerminal404Spec::RunTest(const FString&)
{
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(404),
	});
	TArray<uint8> Body = WoTUpdateTransportPolicySpecBytes("sentinel");
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status =
		WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal);
	TestFalse(TEXT("a terminal HTTP status is not a transport failure"),
		Status.IsError());
	TestEqual(TEXT("the 404 status propagates"), Terminal.Status, 404);
	TestTrue(TEXT("the empty terminal body stays empty"), Body.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecInitialUrl});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDeadlineHopOneSpec,
	"WoT.Update.TransportPolicy.Deadline.BeforeHop1",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDeadlineHopOneSpec::RunTest(const FString&)
{
	return WoTUpdateTransportPolicySpecAssertDeadline(*this, {100.0},
		{WoTUpdateTransportPolicySpecStep(200)}, {});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDeadlineHopTwoSpec,
	"WoT.Update.TransportPolicy.Deadline.BeforeHop2",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDeadlineHopTwoSpec::RunTest(const FString&)
{
	return WoTUpdateTransportPolicySpecAssertDeadline(*this, {0.0, 100.0},
		{
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://release-assets.githubusercontent.com/two")),
			WoTUpdateTransportPolicySpecStep(200),
		}, {WoTUpdateTransportPolicySpecInitialUrl});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDeadlineHopThreeSpec,
	"WoT.Update.TransportPolicy.Deadline.BeforeHop3",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDeadlineHopThreeSpec::RunTest(const FString&)
{
	return WoTUpdateTransportPolicySpecAssertDeadline(
		*this, {0.0, 0.0, 100.0},
		{
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://release-assets.githubusercontent.com/three/one")),
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://objects.githubusercontent.com/three/two")),
			WoTUpdateTransportPolicySpecStep(200),
		}, {
			WoTUpdateTransportPolicySpecInitialUrl,
			TEXT("https://release-assets.githubusercontent.com/three/one"),
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyDeadlineHopFourSpec,
	"WoT.Update.TransportPolicy.Deadline.BeforeHop4",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyDeadlineHopFourSpec::RunTest(const FString&)
{
	return WoTUpdateTransportPolicySpecAssertDeadline(
		*this, {0.0, 0.0, 0.0, 100.0},
		{
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://release-assets.githubusercontent.com/four/one")),
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://objects.githubusercontent.com/four/two")),
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://github-releases.githubusercontent.com/four/three")),
			WoTUpdateTransportPolicySpecStep(200),
		}, {
			WoTUpdateTransportPolicySpecInitialUrl,
			TEXT("https://release-assets.githubusercontent.com/four/one"),
			TEXT("https://objects.githubusercontent.com/four/two"),
		});
}


// GitHub's /releases/latest/download/<asset> answers with
// a 302 to /releases/download/<tag>/<asset> on github.com itself, then the
// CDN. Exactly that hop is followed; everything else stays refused.
constexpr TCHAR WoTUpdateTransportPolicySpecLatestUrl[] = TEXT(
	"https://github.com/aleiby/wot/releases/latest/download/WoT-Win64-announcement-v1.json");
constexpr TCHAR WoTUpdateTransportPolicySpecTagUrl[] = TEXT(
	"https://github.com/aleiby/wot/releases/download/v1.2.0/WoT-Win64-announcement-v1.json");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyLatestToTagSpec,
	"WoT.Update.TransportPolicy.PermittedHops.LatestToTag",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyLatestToTagSpec::RunTest(const FString&)
{
	const TCHAR* Cdn = TEXT(
		"https://release-assets.githubusercontent.com/github-production-release-asset/1/abc?sp=r&sig=x");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, WoTUpdateTransportPolicySpecTagUrl),
		WoTUpdateTransportPolicySpecStep(302, Cdn),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "announcement"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status = WoTUpdateTransportPolicySpecFetchAtZero(
		Hops, Body, Terminal, 0, WoTUpdateTransportPolicySpecLatestUrl);
	TestFalse(TEXT("latest -> tag -> CDN is followed"), Status.IsError());
	TestTrue(TEXT("the CDN body is returned"),
		Body == WoTUpdateTransportPolicySpecBytes("announcement"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{WoTUpdateTransportPolicySpecLatestUrl, WoTUpdateTransportPolicySpecTagUrl, Cdn});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyLatestNotesSpec,
	"WoT.Update.TransportPolicy.PermittedHops.LatestNotesAsset",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyLatestNotesSpec::RunTest(const FString&)
{
	constexpr TCHAR NotesLatest[] = TEXT(
		"https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json");
	constexpr TCHAR NotesTag[] = TEXT(
		"https://github.com/aleiby/wot/releases/download/v1.2.0/WoT-release-notes-v1.json");
	const TCHAR* Cdn = TEXT(
		"https://release-assets.githubusercontent.com/github-production-release-asset/1/notes?sig=x");
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, NotesTag),
		WoTUpdateTransportPolicySpecStep(302, Cdn),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "notes"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status = WoTUpdateTransportPolicySpecFetchAtZero(
		Hops, Body, Terminal, 0, NotesLatest);
	TestFalse(TEXT("the notes latest -> tag -> CDN route is followed"),
		Status.IsError());
	TestTrue(TEXT("the notes body is returned"),
		Body == WoTUpdateTransportPolicySpecBytes("notes"));
	WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
		{NotesLatest, NotesTag, Cdn});
	return !HasAnyErrors();
}

static bool WoTUpdateTransportPolicySpecAssertRefusedLatestHop(
	FAutomationTestBase& Test, const TCHAR* Location)
{
	FWoTUpdateTransportPolicySpecExecutor Hops({
		WoTUpdateTransportPolicySpecStep(302, Location),
		WoTUpdateTransportPolicySpecStep(200, TEXT(""), "must-not-fetch"),
	});
	TArray<uint8> Body;
	FWoTHttpHopResponse Terminal;
	FWoTBootstrapStatus Status = WoTUpdateTransportPolicySpecFetchAtZero(
		Hops, Body, Terminal, 0, WoTUpdateTransportPolicySpecLatestUrl);
	Test.TestTrue(FString::Printf(TEXT("refused: %s"), Location), Status.IsError());
	Test.TestTrue(TEXT("a refused hop leaves the body empty"), Body.IsEmpty());
	WoTUpdateTransportPolicySpecAssertRequests(Test, Hops,
		{WoTUpdateTransportPolicySpecLatestUrl});
	return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyLatestHopRefusalsSpec,
	"WoT.Update.TransportPolicy.Refuse.LatestHop",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyLatestHopRefusalsSpec::RunTest(const FString&)
{
	for (const TCHAR* Location : {
		TEXT("https://github.com/attacker/wot/releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/evil/releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/releases/download/v1/WoT-Mac-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/releases/tag/v1"),
		TEXT("https://github.com/aleiby/wot/releases/download/latest/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json?x=1"),
		TEXT("https://github.com/aleiby/wot/releases/download/v1%2F..%2F/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/releases/download/../WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/releases/download/v1/x/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/Releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("https://api.github.com/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("http://github.com/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com:443/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com./aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json"),
		TEXT("https://github.com/aleiby/wot/releases/download/v1.2.0/WoT-Win64-announcement-v1.json#"),
	})
	{
		WoTUpdateTransportPolicySpecAssertRefusedLatestHop(*this, Location);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTransportPolicyLatestHopPlacementSpec,
	"WoT.Update.TransportPolicy.Refuse.LatestHopPlacement",
	WoTUpdateTransportPolicySpecFlags)

bool FWoTUpdateTransportPolicyLatestHopPlacementSpec::RunTest(const FString&)
{
	// Only hop 1 may stay on github.com: a second github.com hop is refused.
	{
		FWoTUpdateTransportPolicySpecExecutor Hops({
			WoTUpdateTransportPolicySpecStep(302, WoTUpdateTransportPolicySpecTagUrl),
			WoTUpdateTransportPolicySpecStep(302, TEXT(
				"https://github.com/aleiby/wot/releases/download/v1.3.0/WoT-Win64-announcement-v1.json")),
		});
		TArray<uint8> Body;
		FWoTHttpHopResponse Terminal;
		TestTrue(TEXT("a second github.com hop is refused"),
			WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal, 0,
				WoTUpdateTransportPolicySpecLatestUrl).IsError());
		WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
			{WoTUpdateTransportPolicySpecLatestUrl, WoTUpdateTransportPolicySpecTagUrl});
	}
	// The github.com hop is one of the three: latest -> tag -> CDN -> CDN stops
	// before a fourth Location is ever requested.
	{
		const TCHAR* Cdn1 = TEXT("https://release-assets.githubusercontent.com/one");
		const TCHAR* Cdn2 = TEXT("https://objects.githubusercontent.com/two");
		FWoTUpdateTransportPolicySpecExecutor Hops({
			WoTUpdateTransportPolicySpecStep(302, WoTUpdateTransportPolicySpecTagUrl),
			WoTUpdateTransportPolicySpecStep(302, Cdn1),
			WoTUpdateTransportPolicySpecStep(302, Cdn2),
			WoTUpdateTransportPolicySpecStep(302,
				TEXT("https://github-releases.githubusercontent.com/three")),
			WoTUpdateTransportPolicySpecStep(200, TEXT(""), "must-not-fetch"),
		});
		TArray<uint8> Body;
		FWoTHttpHopResponse Terminal;
		TestTrue(TEXT("the github.com hop counts against the three-hop budget"),
			WoTUpdateTransportPolicySpecFetchAtZero(Hops, Body, Terminal, 0,
				WoTUpdateTransportPolicySpecLatestUrl).IsError());
		WoTUpdateTransportPolicySpecAssertRequests(*this, Hops,
			{WoTUpdateTransportPolicySpecLatestUrl, WoTUpdateTransportPolicySpecTagUrl,
				Cdn1, Cdn2});
	}
	// A per-tag initial URL never needs the github.com hop and does not get it.
	WoTUpdateTransportPolicySpecAssertRefusedLocation(*this, WoTUpdateTransportPolicySpecTagUrl);
	// The judge itself: the pair is followed at hop 1 only, and the
	// two-argument form never follows github.com.
	TestTrue(TEXT("hop 1 latest -> tag is followed"),
		WoTJudgeRedirectHop(WoTUpdateTransportPolicySpecTagUrl, 1,
			WoTUpdateTransportPolicySpecLatestUrl) == EWoTRedirectVerdict::Follow);
	TestTrue(TEXT("the same pair at hop 2 is refused"),
		WoTJudgeRedirectHop(WoTUpdateTransportPolicySpecTagUrl, 2,
			WoTUpdateTransportPolicySpecLatestUrl) == EWoTRedirectVerdict::Refuse);
	TestTrue(TEXT("the two-argument judge refuses github.com"),
		WoTJudgeRedirectHop(WoTUpdateTransportPolicySpecTagUrl, 1)
			== EWoTRedirectVerdict::Refuse);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
