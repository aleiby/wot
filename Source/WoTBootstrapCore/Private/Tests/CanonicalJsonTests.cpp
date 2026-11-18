// Specs for the strict canonical JSON grammar.
//
// The contract under test is byte equality, not structural equivalence: for every
// accepted document, writing the parsed value back must reproduce the input
// exactly. Every rejection case below is a way two implementations could
// otherwise disagree about which bytes a signature covers.
//
// The accepted/rejected tables mirror Tools/dist/artifact_roundtrip.py, whose
// fixtures come from Python's canonical `json.dumps`. Keeping both sides
// independent is deliberate: these specs prove the native grammar on its own, and
// the driver proves the two agree byte for byte.
#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/CanonicalJson.h"

#if WITH_AUTOMATION_TESTS

namespace
{
// Unity builds merge test translation units, so every helper here carries a
// file-unique prefix.
constexpr EAutomationTestFlags WoTCanonicalJsonSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

TArray<uint8> WoTCanonicalJsonSpecBytes(FStringView Ascii)
{
	// Every fixture in this file is ASCII apart from the deliberately malformed
	// raw-byte cases, which are built from explicit byte arrays instead.
	TArray<uint8> Bytes;
	Bytes.Reserve(Ascii.Len());
	for (const TCHAR Character : Ascii)
	{
		Bytes.Add(static_cast<uint8>(Character));
	}
	return Bytes;
}

FString WoTCanonicalJsonSpecText(TConstArrayView<uint8> Bytes)
{
	FString Text;
	Text.Reserve(Bytes.Num());
	for (const uint8 Byte : Bytes)
	{
		// Show unprintable bytes so a diff in the log is readable.
		if (Byte >= 0x20 && Byte <= 0x7E)
		{
			Text.AppendChar(static_cast<TCHAR>(Byte));
		}
		else
		{
			Text += FString::Printf(TEXT("<%02x>"), Byte);
		}
	}
	return Text;
}

FString WoTCanonicalJsonSpecNested(int32 Containers)
{
	FString Text;
	for (int32 Index = 0; Index < Containers; ++Index)
	{
		Text += TEXT("{\"a\":");
	}
	Text += TEXT("0");
	for (int32 Index = 0; Index < Containers; ++Index)
	{
		Text += TEXT("}");
	}
	return Text;
}

/** Parse, write back, and require the exact input bytes. */
bool WoTCanonicalJsonSpecRoundTrips(FAutomationTestBase& Test, const FString& Label,
	TConstArrayView<uint8> Input)
{
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Input);
	if (Parsed.IsError())
	{
		Test.AddError(FString::Printf(TEXT("%s: rejected accepted fixture: %s"),
			*Label, *Parsed.GetError().ToString()));
		return false;
	}

	TWoTBootstrapResult<TArray<uint8>> Written = WriteCanonicalJson(Parsed.GetValue());
	if (Written.IsError())
	{
		Test.AddError(FString::Printf(TEXT("%s: could not re-emit: %s"),
			*Label, *Written.GetError().ToString()));
		return false;
	}

	const TArray<uint8>& Bytes = Written.GetValue();
	if (Bytes.Num() != Input.Num() || FMemory::Memcmp(Bytes.GetData(), Input.GetData(), Input.Num()) != 0)
	{
		Test.AddError(FString::Printf(TEXT("%s: re-emitted '%s', expected '%s'"),
			*Label, *WoTCanonicalJsonSpecText(Bytes), *WoTCanonicalJsonSpecText(Input)));
		return false;
	}
	return true;
}

void WoTCanonicalJsonSpecRejects(FAutomationTestBase& Test, const FString& Label,
	TConstArrayView<uint8> Input, EWoTBootstrapError Expected)
{
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Input);
	if (!Parsed.IsError())
	{
		Test.AddError(FString::Printf(TEXT("%s: accepted '%s', expected %s"),
			*Label, *WoTCanonicalJsonSpecText(Input), LexToString(Expected)));
		return;
	}
	const EWoTBootstrapError Actual = Parsed.GetError().Code;
	if (Actual != Expected)
	{
		Test.AddError(FString::Printf(TEXT("%s: rejected '%s' as %s, expected %s (%s)"),
			*Label, *WoTCanonicalJsonSpecText(Input), LexToString(Actual),
			LexToString(Expected), *Parsed.GetError().Message));
	}
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCanonicalJsonAcceptSpec,
	"WoT.BootstrapCore.CanonicalJson.Accept", WoTCanonicalJsonSpecFlags)

bool FWoTCanonicalJsonAcceptSpec::RunTest(const FString&)
{
	// Escapes below are C++ escapes: the fixture bytes are what Python's
	// json.dumps emits, so `\\n` here is the two-byte sequence backslash+n.
	const TCHAR* const Fixtures[] = {
		TEXT("{}"),
		TEXT("[]"),
		TEXT("null"),
		TEXT("true"),
		TEXT("false"),
		TEXT("0"),
		TEXT("-1"),
		TEXT("-9223372036854775808"),
		TEXT("9223372036854775807"),
		TEXT("\"wot\""),
		TEXT("\"\""),
		TEXT("{\"alpha\":1,\"beta\":{\"gamma\":[1,2,3]},\"zeta\":null}"),
		TEXT("\"quote:\\\" backslash:\\\\ nl:\\n tab:\\t cr:\\r bs:\\b ff:\\f\""),
		TEXT("\"\\u0001\\u001f\""),
		TEXT("\"\\u007f\""),
		TEXT("\"caf\\u00e9 \\u4e2d\""),
		TEXT("\"\\ud83d\\ude00\""),
		TEXT("[[],[[]],[[[1]]]]"),
		TEXT("[0,-1,1]"),
		TEXT("{\"a\":{},\"b\":[]}"),
		TEXT("{\"asset\":\"WoT-Mac-stripped-abc123456.zip\",\"platform\":\"Mac\",\"size\":123}"),
		// Keys are ordered by codepoint, so a shorter key that is a prefix of a
		// longer one comes first, and digits precede letters.
		TEXT("{\"a\":0,\"aa\":1,\"b\":2}"),
		TEXT("{\"0\":0,\"A\":1,\"a\":2}"),
	};

	for (const TCHAR* Fixture : Fixtures)
	{
		const TArray<uint8> Input = WoTCanonicalJsonSpecBytes(Fixture);
		WoTCanonicalJsonSpecRoundTrips(*this, FString(Fixture), Input);
	}

	// Exactly MaxDepth containers is accepted; MaxDepth + 1 is a SizeLimit below.
	const FWoTCanonicalJsonLimits Limits;
	const FString AtLimit = WoTCanonicalJsonSpecNested(Limits.MaxDepth);
	WoTCanonicalJsonSpecRoundTrips(*this, TEXT("max-depth"),
		WoTCanonicalJsonSpecBytes(AtLimit));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCanonicalJsonRejectSpec,
	"WoT.BootstrapCore.CanonicalJson.Reject", WoTCanonicalJsonSpecFlags)

bool FWoTCanonicalJsonRejectSpec::RunTest(const FString&)
{
	struct FCase
	{
		const TCHAR* Text;
		EWoTBootstrapError Expected;
	};

	const FCase Cases[] = {
		// Nothing to parse, and no whitespace tolerance in either direction.
		{TEXT(""),                     EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{} "),                  EWoTBootstrapError::NonCanonicalJson},
		{TEXT(" {}"),                  EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{}x"),                  EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{}\n"),                 EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"a\": 1}"),           EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"a\":1 }"),           EWoTBootstrapError::NonCanonicalJson},
		{TEXT("[1, 2]"),               EWoTBootstrapError::NonCanonicalJson},
		// Ordering and duplication are distinct failures.
		{TEXT("{\"b\":1,\"a\":2}"),    EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"aa\":1,\"a\":2}"),   EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"a\":1,\"a\":2}"),    EWoTBootstrapError::DuplicateField},
		// Integers only.
		{TEXT("-0"),                   EWoTBootstrapError::InvalidNumber},
		{TEXT("1e5"),                  EWoTBootstrapError::InvalidNumber},
		{TEXT("1E5"),                  EWoTBootstrapError::InvalidNumber},
		{TEXT("1.0"),                  EWoTBootstrapError::InvalidNumber},
		{TEXT("01"),                   EWoTBootstrapError::InvalidNumber},
		{TEXT("-01"),                  EWoTBootstrapError::InvalidNumber},
		{TEXT("-"),                    EWoTBootstrapError::InvalidNumber},
		{TEXT("+1"),                   EWoTBootstrapError::NonCanonicalJson},
		{TEXT("9223372036854775808"),  EWoTBootstrapError::InvalidNumber},
		{TEXT("-9223372036854775809"), EWoTBootstrapError::InvalidNumber},
		{TEXT("99999999999999999999"), EWoTBootstrapError::InvalidNumber},
		// Not JSON tokens at all.
		{TEXT("NaN"),                  EWoTBootstrapError::NonCanonicalJson},
		{TEXT("Infinity"),             EWoTBootstrapError::NonCanonicalJson},
		{TEXT("-Infinity"),            EWoTBootstrapError::InvalidNumber},
		{TEXT("True"),                 EWoTBootstrapError::NonCanonicalJson},
		{TEXT("nul"),                  EWoTBootstrapError::NonCanonicalJson},
		// Escaping must be the canonical spelling, not merely a legal one.
		{TEXT("\"\\/\""),              EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\u0041\""),          EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\u0020\""),          EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\u00E9\""),          EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\u000a\""),          EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\u0009\""),          EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\x41\""),            EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\u12\""),            EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"\\uzzzz\""),          EWoTBootstrapError::NonCanonicalJson},
		// U+0000 is legal JSON but has no representation this module can carry:
		// FString terminates at NUL, so accepting it would silently truncate the
		// value and break the byte-exact round trip that signing depends on. No
		// bootstrap artifact contains a NUL, so it is refused rather than mangled.
		{TEXT("\"\\u0000\""),          EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"a\\u0000b\""),        EWoTBootstrapError::NonCanonicalJson},
		// Surrogates must pair.
		{TEXT("\"\\ud800\""),          EWoTBootstrapError::InvalidUtf8},
		{TEXT("\"\\udc00\""),          EWoTBootstrapError::InvalidUtf8},
		{TEXT("\"\\ud800\\u0041\""),   EWoTBootstrapError::InvalidUtf8},
		{TEXT("\"\\ud800\\ud800\""),   EWoTBootstrapError::InvalidUtf8},
		// Structure.
		{TEXT("{\"a\":}"),             EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"a\":1"),             EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"a\"1}"),             EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{a:1}"),                EWoTBootstrapError::NonCanonicalJson},
		{TEXT("{\"a\":1,}"),           EWoTBootstrapError::NonCanonicalJson},
		{TEXT("[1,]"),                 EWoTBootstrapError::NonCanonicalJson},
		{TEXT("[1"),                   EWoTBootstrapError::NonCanonicalJson},
		{TEXT("\"abc"),                EWoTBootstrapError::NonCanonicalJson},
		{TEXT("}"),                    EWoTBootstrapError::NonCanonicalJson},
	};

	for (const FCase& Case : Cases)
	{
		WoTCanonicalJsonSpecRejects(*this, FString(Case.Text),
			WoTCanonicalJsonSpecBytes(Case.Text), Case.Expected);
	}

	// Raw bytes a TCHAR fixture cannot express. Malformed UTF-8 is InvalidUtf8;
	// well-formed but unescaped non-ASCII is merely non-canonical.
	const TArray<uint8> RawUtf8 = {'"', 0xC3, 0xA9, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("raw-utf8"), RawUtf8,
		EWoTBootstrapError::NonCanonicalJson);

	const TArray<uint8> RawControl = {'"', 0x01, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("raw-control-byte"), RawControl,
		EWoTBootstrapError::NonCanonicalJson);

	const TArray<uint8> RawDelete = {'"', 0x7F, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("raw-delete-byte"), RawDelete,
		EWoTBootstrapError::NonCanonicalJson);

	const TArray<uint8> Overlong = {'"', 0xC0, 0xAF, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("overlong-utf8"), Overlong,
		EWoTBootstrapError::InvalidUtf8);

	const TArray<uint8> OverlongThreeByte = {'"', 0xE0, 0x80, 0xAF, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("overlong-utf8-3"), OverlongThreeByte,
		EWoTBootstrapError::InvalidUtf8);

	const TArray<uint8> TruncatedUtf8 = {'"', 0xC3, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("truncated-utf8"), TruncatedUtf8,
		EWoTBootstrapError::InvalidUtf8);

	const TArray<uint8> LoneContinuation = {'"', 0x80, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("lone-continuation"), LoneContinuation,
		EWoTBootstrapError::InvalidUtf8);

	const TArray<uint8> EncodedSurrogate = {'"', 0xED, 0xA0, 0x80, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("utf8-encoded-surrogate"), EncodedSurrogate,
		EWoTBootstrapError::InvalidUtf8);

	const TArray<uint8> AboveUnicode = {'"', 0xF5, 0x80, 0x80, 0x80, '"'};
	WoTCanonicalJsonSpecRejects(*this, TEXT("above-u10ffff"), AboveUnicode,
		EWoTBootstrapError::InvalidUtf8);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCanonicalJsonLimitsSpec,
	"WoT.BootstrapCore.CanonicalJson.Limits", WoTCanonicalJsonSpecFlags)

bool FWoTCanonicalJsonLimitsSpec::RunTest(const FString&)
{
	const FWoTCanonicalJsonLimits Defaults;

	// One container past MaxDepth.
	WoTCanonicalJsonSpecRejects(*this, TEXT("depth-over-limit"),
		WoTCanonicalJsonSpecBytes(WoTCanonicalJsonSpecNested(Defaults.MaxDepth + 1)),
		EWoTBootstrapError::SizeLimit);

	// Deep arrays are bounded by the same counter as deep objects.
	{
		FString Text;
		for (uint16 Index = 0; Index < Defaults.MaxDepth + 1; ++Index)
		{
			Text += TEXT("[");
		}
		Text += TEXT("0");
		for (uint16 Index = 0; Index < Defaults.MaxDepth + 1; ++Index)
		{
			Text += TEXT("]");
		}
		WoTCanonicalJsonSpecRejects(*this, TEXT("array-depth-over-limit"),
			WoTCanonicalJsonSpecBytes(Text), EWoTBootstrapError::SizeLimit);
	}

	// MaxBytes is checked before parsing, so an oversized document costs nothing.
	{
		FWoTCanonicalJsonLimits Tight;
		Tight.MaxBytes = 3;
		const TArray<uint8> Input = WoTCanonicalJsonSpecBytes(TEXT("[1,2]"));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Input, Tight);
		TestTrue(TEXT("oversized document is rejected"), Parsed.IsError());
		if (Parsed.IsError())
		{
			TestEqual(TEXT("oversized document reports SizeLimit"),
				Parsed.GetError().Code, EWoTBootstrapError::SizeLimit);
		}
	}

	// MaxMembers bounds objects and arrays alike.
	{
		FWoTCanonicalJsonLimits Tight;
		Tight.MaxMembers = 2;

		TWoTBootstrapResult<FWoTCanonicalJsonValue> Array =
			ParseCanonicalJson(WoTCanonicalJsonSpecBytes(TEXT("[1,2,3]")), Tight);
		TestTrue(TEXT("array member cap is enforced"), Array.IsError());
		if (Array.IsError())
		{
			TestEqual(TEXT("array member cap reports SizeLimit"),
				Array.GetError().Code, EWoTBootstrapError::SizeLimit);
		}

		TWoTBootstrapResult<FWoTCanonicalJsonValue> Object = ParseCanonicalJson(
			WoTCanonicalJsonSpecBytes(TEXT("{\"a\":1,\"b\":2,\"c\":3}")), Tight);
		TestTrue(TEXT("object member cap is enforced"), Object.IsError());
		if (Object.IsError())
		{
			TestEqual(TEXT("object member cap reports SizeLimit"),
				Object.GetError().Code, EWoTBootstrapError::SizeLimit);
		}

		TWoTBootstrapResult<FWoTCanonicalJsonValue> AtCap =
			ParseCanonicalJson(WoTCanonicalJsonSpecBytes(TEXT("[1,2]")), Tight);
		TestFalse(TEXT("exactly MaxMembers is accepted"), AtCap.IsError());
	}

	// MaxStringBytes counts decoded UTF-8 bytes, not source characters: a single
	// escaped codepoint can cost more than the two bytes it was written with.
	{
		FWoTCanonicalJsonLimits Tight;
		Tight.MaxStringBytes = 2;

		TWoTBootstrapResult<FWoTCanonicalJsonValue> Escaped =
			ParseCanonicalJson(WoTCanonicalJsonSpecBytes(TEXT("\"\\u4e2d\"")), Tight);
		TestTrue(TEXT("decoded string length is capped"), Escaped.IsError());
		if (Escaped.IsError())
		{
			TestEqual(TEXT("string cap reports SizeLimit"),
				Escaped.GetError().Code, EWoTBootstrapError::SizeLimit);
		}

		TWoTBootstrapResult<FWoTCanonicalJsonValue> Short =
			ParseCanonicalJson(WoTCanonicalJsonSpecBytes(TEXT("\"ab\"")), Tight);
		TestFalse(TEXT("exactly MaxStringBytes is accepted"), Short.IsError());
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCanonicalJsonValueSpec,
	"WoT.BootstrapCore.CanonicalJson.Value", WoTCanonicalJsonSpecFlags)

bool FWoTCanonicalJsonValueSpec::RunTest(const FString&)
{
	// Parsed structure is inspectable without a second parser.
	const TArray<uint8> Input = WoTCanonicalJsonSpecBytes(
		TEXT("{\"count\":7,\"flag\":true,\"items\":[1,2],\"name\":\"wot\",\"none\":null}"));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Input);
	if (!TestFalse(TEXT("fixture parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}

	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	TestEqual(TEXT("root is an object"), Root.GetKind(), EWoTCanonicalJsonKind::Object);
	TestEqual(TEXT("member count"), Root.Num(), 5);
	TestEqual(TEXT("members are in canonical order"), Root.GetMemberKey(0), FString(TEXT("count")));
	TestEqual(TEXT("last member"), Root.GetMemberKey(4), FString(TEXT("none")));

	const FWoTCanonicalJsonValue* const Count = Root.FindMember(TEXT("count"));
	if (TestNotNull(TEXT("FindMember locates an integer"), Count))
	{
		TestEqual(TEXT("integer value"), Count->GetInteger(), static_cast<int64>(7));
	}
	const FWoTCanonicalJsonValue* const Flag = Root.FindMember(TEXT("flag"));
	if (TestNotNull(TEXT("FindMember locates a bool"), Flag))
	{
		TestTrue(TEXT("bool value"), Flag->GetBool());
	}
	const FWoTCanonicalJsonValue* const Name = Root.FindMember(TEXT("name"));
	if (TestNotNull(TEXT("FindMember locates a string"), Name))
	{
		TestEqual(TEXT("string value"), Name->GetString(), FString(TEXT("wot")));
	}
	const FWoTCanonicalJsonValue* const Items = Root.FindMember(TEXT("items"));
	if (TestNotNull(TEXT("FindMember locates an array"), Items))
	{
		TestEqual(TEXT("element count"), Items->Num(), 2);
		TestEqual(TEXT("second element"), Items->GetElement(1).GetInteger(), static_cast<int64>(2));
	}
	const FWoTCanonicalJsonValue* const None = Root.FindMember(TEXT("none"));
	if (TestNotNull(TEXT("FindMember locates null"), None))
	{
		TestTrue(TEXT("null kind"), None->IsNull());
	}
	TestNull(TEXT("an absent member is not invented"), Root.FindMember(TEXT("absent")));
	TestNull(TEXT("a key prefix does not match"), Root.FindMember(TEXT("nam")));

	// A hand-built value cannot be made to serialize non-canonically.
	FWoTCanonicalJsonValue Built = FWoTCanonicalJsonValue::MakeObject();
	TestTrue(TEXT("first member is accepted"),
		Built.AddMember(TEXT("alpha"), FWoTCanonicalJsonValue::MakeInteger(1)));
	TestTrue(TEXT("an increasing key is accepted"),
		Built.AddMember(TEXT("beta"), FWoTCanonicalJsonValue::MakeNull()));
	TestFalse(TEXT("a repeated key is refused"),
		Built.AddMember(TEXT("beta"), FWoTCanonicalJsonValue::MakeBool(false)));
	TestFalse(TEXT("a decreasing key is refused"),
		Built.AddMember(TEXT("alpha"), FWoTCanonicalJsonValue::MakeBool(false)));
	TestEqual(TEXT("refused members are not stored"), Built.Num(), 2);

	TWoTBootstrapResult<TArray<uint8>> Written = WriteCanonicalJson(Built);
	if (TestFalse(TEXT("hand-built value serializes"), Written.IsError()))
	{
		const FString Text = WoTCanonicalJsonSpecText(Written.GetValue());
		TestEqual(TEXT("hand-built canonical bytes"), Text,
			FString(TEXT("{\"alpha\":1,\"beta\":null}")));
	}

	// The writer escapes to ASCII: a string holding real non-ASCII text must come
	// out as lowercase \u escapes, never as raw UTF-8 bytes.
	FWoTCanonicalJsonValue Text = FWoTCanonicalJsonValue::MakeString(
		FString(UTF8_TO_TCHAR("caf\xC3\xA9 \xE4\xB8\xAD")));
	TWoTBootstrapResult<TArray<uint8>> Escaped = WriteCanonicalJson(Text);
	if (TestFalse(TEXT("non-ASCII string serializes"), Escaped.IsError()))
	{
		const TArray<uint8>& Bytes = Escaped.GetValue();
		bool bAsciiOnly = true;
		for (const uint8 Byte : Bytes)
		{
			bAsciiOnly &= (Byte >= 0x20 && Byte <= 0x7E);
		}
		TestTrue(TEXT("canonical output is ASCII only"), bAsciiOnly);
		TestEqual(TEXT("non-ASCII escapes are lowercase"),
			WoTCanonicalJsonSpecText(Bytes),
			FString(TEXT("\"caf\\u00e9 \\u4e2d\"")));
	}

	// Non-BMP text becomes a surrogate pair, matching Python's ensure_ascii.
	FWoTCanonicalJsonValue Emoji = FWoTCanonicalJsonValue::MakeString(
		FString(UTF8_TO_TCHAR("\xF0\x9F\x98\x80")));
	TWoTBootstrapResult<TArray<uint8>> Paired = WriteCanonicalJson(Emoji);
	if (TestFalse(TEXT("non-BMP string serializes"), Paired.IsError()))
	{
		TestEqual(TEXT("non-BMP becomes a surrogate pair"),
			WoTCanonicalJsonSpecText(Paired.GetValue()),
			FString(TEXT("\"\\ud83d\\ude00\"")));
	}

	// Integer extremes survive the round trip through the writer.
	TWoTBootstrapResult<TArray<uint8>> Minimum =
		WriteCanonicalJson(FWoTCanonicalJsonValue::MakeInteger(MIN_int64));
	if (TestFalse(TEXT("int64 minimum serializes"), Minimum.IsError()))
	{
		TestEqual(TEXT("int64 minimum bytes"),
			WoTCanonicalJsonSpecText(Minimum.GetValue()),
			FString(TEXT("-9223372036854775808")));
	}

	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
