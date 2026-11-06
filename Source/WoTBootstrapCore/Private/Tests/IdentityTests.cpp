// Specs for installed-tree inventories and component identities.
#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/ComponentIdentity.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/InstalledTree.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTIdentitySpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

TArray<uint8> WoTIdentitySpecBytes(FStringView Ascii)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Ascii.Len());
	for (const TCHAR Character : Ascii)
	{
		Bytes.Add(static_cast<uint8>(Character));
	}
	return Bytes;
}

FWoTSha256 WoTIdentitySpecDigest(uint8 Seed)
{
	FWoTSha256 Digest;
	for (int32 Index = 0; Index < FWoTSha256::ByteCount; ++Index)
	{
		Digest.Bytes[Index] = static_cast<uint8>(Seed + Index);
	}
	return Digest;
}

/** A minimal two-file inventory in canonical form. */
FString WoTIdentitySpecTreeJson()
{
	return TEXT("{\"entries\":[")
		TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"Content/data.bin\",")
		TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
		TEXT("\"size\":12,\"type\":\"file\"},")
		TEXT("{\"link_target\":\"\",\"mode\":493,\"path\":\"WoT\",")
		TEXT("\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",")
		TEXT("\"size\":2048,\"type\":\"file\"}")
		TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");
}

TWoTBootstrapResult<FWoTInstalledTreeManifest> WoTIdentitySpecParseTree(FStringView Json)
{
	return ParseInstalledTreeV1(WoTIdentitySpecBytes(Json));
}

}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTIdentityInstalledTreeSpec,
	"WoT.BootstrapCore.Identity.InstalledTree", WoTIdentitySpecFlags)

bool FWoTIdentityInstalledTreeSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTInstalledTreeManifest> Parsed =
		WoTIdentitySpecParseTree(WoTIdentitySpecTreeJson());
	if (!TestFalse(TEXT("the inventory parses"), Parsed.IsError()))
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	const FWoTInstalledTreeManifest& Inventory = Parsed.GetValue();
	TestEqual(TEXT("entry count"), Inventory.Entries.Num(), 2);
	TestEqual(TEXT("first entry path"), Inventory.Entries[0].Path,
		FString(TEXT("Content/data.bin")));
	TestEqual(TEXT("mode is a permission value"),
		static_cast<int32>(Inventory.Entries[1].Mode), 493);

	// The root digest is derived, deterministic, and sensitive to everything.
	{
		TWoTBootstrapResult<FWoTSha256> Root = ComputeInstalledTreeRoot(Inventory);
		if (!TestFalse(TEXT("the root digest computes"), Root.IsError()))
		{
			return false;
		}
		TWoTBootstrapResult<FWoTSha256> Again = ComputeInstalledTreeRoot(Inventory);
		TestTrue(TEXT("the root digest is deterministic"),
			!Again.IsError() && Again.GetValue() == Root.GetValue());

		const auto RootOf = [this](FStringView Json) -> FWoTSha256
		{
			TWoTBootstrapResult<FWoTInstalledTreeManifest> Other =
				WoTIdentitySpecParseTree(Json);
			if (Other.IsError())
			{
				AddError(FString::Printf(TEXT("variant did not parse: %s"),
					*Other.GetError().ToString()));
				return FWoTSha256();
			}
			TWoTBootstrapResult<FWoTSha256> Digest =
				ComputeInstalledTreeRoot(Other.GetValue());
			return Digest.IsError() ? FWoTSha256() : Digest.GetValue();
		};

		// A widened mode changes the tree even though every content digest matches.
		FString WiderMode = WoTIdentitySpecTreeJson();
		WiderMode.ReplaceInline(TEXT("\"mode\":420"), TEXT("\"mode\":438"));
		TestTrue(TEXT("a changed mode changes the root"),
			RootOf(WiderMode) != Root.GetValue());

		// A renamed file changes the tree even though its bytes are identical.
		FString Renamed = WoTIdentitySpecTreeJson();
		Renamed.ReplaceInline(TEXT("Content/data.bin"), TEXT("Content/data2.bin"));
		TestTrue(TEXT("a renamed entry changes the root"),
			RootOf(Renamed) != Root.GetValue());

		// A file replaced by a symlink of the same name changes the tree.
		FString AsLink = TEXT("{\"entries\":[")
			TEXT("{\"link_target\":\"WoT\",\"mode\":420,\"path\":\"Content/data.bin\",")
			TEXT("\"sha256\":\"\",\"size\":0,\"type\":\"symlink\"},")
			TEXT("{\"link_target\":\"\",\"mode\":493,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",")
			TEXT("\"size\":2048,\"type\":\"file\"}")
			TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");
		TestTrue(TEXT("a file turned into a symlink changes the root"),
			RootOf(AsLink) != Root.GetValue());
	}

	// Structural refusals.
	{
		struct FCase
		{
			FString Json;
			const TCHAR* Why;
		};
		FString SelfListing = WoTIdentitySpecTreeJson();
		SelfListing.ReplaceInline(TEXT("Content/data.bin"), TEXT("installed-tree-v1.json"));

		FString Traversal = WoTIdentitySpecTreeJson();
		Traversal.ReplaceInline(TEXT("Content/data.bin"), TEXT("../outside.bin"));

		FString Absolute = WoTIdentitySpecTreeJson();
		Absolute.ReplaceInline(TEXT("Content/data.bin"), TEXT("/etc/passwd"));

		FString AppleDoubleFile = WoTIdentitySpecTreeJson();
		AppleDoubleFile.ReplaceInline(
			TEXT("Content/data.bin"), TEXT("Contents/._Info.plist"));

		FString AppleDoubleDirectory = WoTIdentitySpecTreeJson();
		AppleDoubleDirectory.ReplaceInline(
			TEXT("Content/data.bin"), TEXT("Contents/__MACOSX/data.bin"));

		FString Setuid = WoTIdentitySpecTreeJson();
		Setuid.ReplaceInline(TEXT("\"mode\":493"), TEXT("\"mode\":2541"));

		FString Unsorted = TEXT("{\"entries\":[")
			TEXT("{\"link_target\":\"\",\"mode\":493,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\",")
			TEXT("\"size\":2048,\"type\":\"file\"},")
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"Content/data.bin\",")
			TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"size\":12,\"type\":\"file\"}")
			TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");

		FString Duplicate = TEXT("{\"entries\":[")
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"size\":12,\"type\":\"file\"},")
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"size\":12,\"type\":\"file\"}")
			TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");

		FString CaseAlias = TEXT("{\"entries\":[")
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"size\":12,\"type\":\"file\"},")
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"wot\",")
			TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"size\":12,\"type\":\"file\"}")
			TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");

		FString EscapingLink = TEXT("{\"entries\":[")
			TEXT("{\"link_target\":\"../../etc/passwd\",\"mode\":420,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"\",\"size\":0,\"type\":\"symlink\"}")
			TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");

		FString MixedFields = TEXT("{\"entries\":[")
			TEXT("{\"link_target\":\"other\",\"mode\":420,\"path\":\"WoT\",")
			TEXT("\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"size\":12,\"type\":\"file\"}")
			TEXT("],\"format\":\"wot-installed-tree-v1\",\"platform\":\"Mac\",\"version\":1}");

		const FCase Cases[] = {
			{SelfListing, TEXT("an inventory listing itself")},
			{Traversal, TEXT("a traversing path")},
			{Absolute, TEXT("an absolute path")},
			{AppleDoubleFile, TEXT("an AppleDouble sidecar component")},
			{AppleDoubleDirectory, TEXT("a nested __MACOSX component")},
			{Setuid, TEXT("a setuid mode")},
			{Unsorted, TEXT("unsorted entries")},
			{Duplicate, TEXT("a duplicate path")},
			{CaseAlias, TEXT("two paths differing only by case")},
			{EscapingLink, TEXT("a link escaping the tree")},
			{MixedFields, TEXT("a file carrying a link target")},
		};
		for (const FCase& Case : Cases)
		{
			TestTrue(FString::Printf(TEXT("%s is refused"), Case.Why),
				WoTIdentitySpecParseTree(Case.Json).IsError());
		}
	}

	// An unordered inventory built in memory cannot be hashed into a value no
	// other implementation would reproduce.
	{
		FWoTInstalledTreeManifest Unordered;
		Unordered.Format = WoTInstalledTreeV1Format;
		Unordered.Version = WoTInstalledTreeVersion;
		Unordered.Platform = TEXT("Mac");
		FWoTInstalledTreeEntry Second;
		Second.Path = TEXT("b");
		FWoTInstalledTreeEntry First;
		First.Path = TEXT("a");
		Unordered.Entries.Add(Second);
		Unordered.Entries.Add(First);
		TestTrue(TEXT("an unordered inventory refuses to produce a root"),
			ComputeInstalledTreeRoot(Unordered).IsError());
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTIdentityComponentSpec,
	"WoT.BootstrapCore.Identity.Component", WoTIdentitySpecFlags)

bool FWoTIdentityComponentSpec::RunTest(const FString&)
{
	const FString Json = TEXT("{\"component_id\":\"candidate-root\",")
		TEXT("\"format\":\"wot-component-identity-v1\",\"name\":\"WoT.exe\",")
		TEXT("\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\",")
		TEXT("\"size\":2048}");

	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document =
		ParseCanonicalJson(WoTIdentitySpecBytes(Json));
	if (!TestFalse(TEXT("the component document parses"), Document.IsError()))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTComponentIdentity> Component =
		ParseComponentIdentityV1(Document.GetValue());
	if (!TestFalse(TEXT("the component identity parses"), Component.IsError()))
	{
		AddError(Component.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("component id"), Component.GetValue().ComponentId,
		FString(TEXT("candidate-root")));
	TestEqual(TEXT("component size"),
		static_cast<int64>(Component.GetValue().Size), static_cast<int64>(2048));

	// Matching is against a HELD handle's identity, never a re-opened path.
	{
		FWoTNativeFileIdentity Actual;
		Actual.Type = EWoTTreeEntryType::File;
		Actual.Size = 2048;
		TestFalse(TEXT("a matching component is accepted"),
			MatchComponentIdentity(Component.GetValue(), Actual,
				Component.GetValue().Sha256).IsError());

		// A directory or link of the right size and digest is still not the file.
		FWoTNativeFileIdentity AsDirectory = Actual;
		AsDirectory.Type = EWoTTreeEntryType::Directory;
		TestTrue(TEXT("a directory is not the component"),
			MatchComponentIdentity(Component.GetValue(), AsDirectory,
				Component.GetValue().Sha256).IsError());

		FWoTNativeFileIdentity AsLink = Actual;
		AsLink.Type = EWoTTreeEntryType::SymbolicLink;
		TestTrue(TEXT("a symlink is not the component"),
			MatchComponentIdentity(Component.GetValue(), AsLink,
				Component.GetValue().Sha256).IsError());

		FWoTNativeFileIdentity WrongSize = Actual;
		WrongSize.Size = 2049;
		TestTrue(TEXT("a size mismatch is refused"),
			MatchComponentIdentity(Component.GetValue(), WrongSize,
				Component.GetValue().Sha256).IsError());

		TestTrue(TEXT("a content mismatch is refused"),
			MatchComponentIdentity(Component.GetValue(), Actual,
				WoTIdentitySpecDigest(0x99)).IsError());
	}

	// A component name is a bare file name, never a path.
	{
		FString Escaping = Json;
		Escaping.ReplaceInline(TEXT("\"name\":\"WoT.exe\""),
			TEXT("\"name\":\"../../WoT.exe\""));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Bad =
			ParseCanonicalJson(WoTIdentitySpecBytes(Escaping));
		if (TestFalse(TEXT("the escaping document parses as JSON"), Bad.IsError()))
		{
			TestTrue(TEXT("an escaping component name is refused"),
				ParseComponentIdentityV1(Bad.GetValue()).IsError());
		}
	}

	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
