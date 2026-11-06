#if WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "../Updater/WoTInstallerAcquisition.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
constexpr EAutomationTestFlags WoTInstallerAcquisitionSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

TArray<uint8> WoTInstallerAcquisitionSpecBytes(const ANSICHAR* Text)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), Length);
	return Bytes;
}

bool WoTInstallerAcquisitionSpecCreateHardLink(
	const FString& LinkPath, const FString& ExistingPath)
{
#if PLATFORM_WINDOWS
	return ::CreateHardLinkW(*LinkPath, *ExistingPath, nullptr) != 0;
#else
	const FTCHARToUTF8 LinkUtf8(*LinkPath);
	const FTCHARToUTF8 ExistingUtf8(*ExistingPath);
	return link(ExistingUtf8.Get(), LinkUtf8.Get()) == 0;
#endif
}

class FWoTInstallerAcquisitionSpecFixture final
{
public:
	FWoTInstallerAcquisitionSpecFixture()
	{
		Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(), TEXT("wot-installer-acquisition-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		FPaths::NormalizeDirectoryName(Root);
		StateDir = FPaths::Combine(Root, TEXT("state"));
		Pins.Name = WoT::Bootstrap::InstallerName;
		Pins.Size = static_cast<uint64>(Good.Num());
		Pins.Sha256 = Sha256(Good);
		CanonicalPath = FPaths::Combine(Root, Pins.Name);
		bReady = IFileManager::Get().MakeDirectory(*Root, true);
	}

	~FWoTInstallerAcquisitionSpecFixture()
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().DeleteDirectory(*(Root + TEXT("-original")),
			false, true);
	}

	bool IsReady() const { return bReady; }
	bool WriteCanonical(const TArray<uint8>& Bytes) const
	{
		return FFileHelper::SaveArrayToFile(Bytes, *CanonicalPath);
	}
	TWoTBootstrapResult<FWoTAdjacentInstallerInspection> Inspect(
		const FWoTNativeFileIdentity* Parent = nullptr) const
	{
		return WoTInspectPinnedAdjacentInstallerForTesting(
			CanonicalPath, Pins, Parent);
	}
	TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> Acquire(
		const FWoTInstallerAcquisitionTestHooks& Hooks = {},
		const FWoTNativeFileIdentity* Parent = nullptr,
		const FWoTNativeFileIdentity* Installer = nullptr) const
	{
		return WoTTryAcquirePinnedAdjacentInstallerForTesting(
			CanonicalPath, Pins, {}, Hooks, Parent, Installer);
	}

	TArray<uint8> Good = WoTInstallerAcquisitionSpecBytes("abcdefgh");
	TArray<uint8> Bad = WoTInstallerAcquisitionSpecBytes("ABCDEFGH");
	FWoTInstallerAcquisitionPins Pins;
	FString Root;
	FString StateDir;
	FString CanonicalPath;

private:
	bool bReady = false;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerAcquisitionSpec,
	"WoT.Bootstrap.InstallerAcquisition", WoTInstallerAcquisitionSpecFlags)

bool FWoTInstallerAcquisitionSpec::RunTest(const FString& Parameters)
{
	(void)Parameters;
	{
		FWoTInstallerAcquisitionPins Defaults;
		TestEqual(TEXT("pin seam defaults the name"), Defaults.Name,
			FString(WoT::Bootstrap::InstallerName));
		TestEqual(TEXT("pin seam defaults the size"), Defaults.Size,
			WoT::Bootstrap::InstallerSize);
		TestEqual(TEXT("pin seam defaults the digest"),
			Defaults.Sha256.ToHex(), FString(WoT::Bootstrap::InstallerSha256));
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("missing fixture is ready"), Fixture.IsReady());
		auto Missing = Fixture.Inspect();
		TestFalse(TEXT("missing inspection succeeds"), Missing.IsError());
		if (!Missing.IsError())
		{
			TestEqual(TEXT("missing is classified"), Missing.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::Missing);
		}
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("wrong-size fixture is written"),
			Fixture.WriteCanonical(WoTInstallerAcquisitionSpecBytes("abcd")));
		auto WrongSize = Fixture.Inspect();
		TestFalse(TEXT("wrong-size inspection succeeds"), WrongSize.IsError());
		if (!WrongSize.IsError())
		{
			TestEqual(TEXT("a short file is still downloading"),
				WrongSize.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::Downloading);
		}
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		const FString Duplicate = FPaths::Combine(Fixture.Root,
			TEXT("setup_the_wheel_of_time_333b_(54846) (2).exe"));
		TestTrue(TEXT("duplicate-name fixture is written"),
			FFileHelper::SaveArrayToFile(Fixture.Good, *Duplicate));
		FWoTInstallerLocation Downloads;
		Downloads.Source = EWoTInstallerSource::Downloads;
		Downloads.CanonicalPath = Duplicate;
		Downloads.bRequirePinnedName = false;
		auto Valid = WoTTryAcquireInstallerLocationForTesting(
			Downloads, Fixture.Pins);
		TestTrue(TEXT("Downloads accepts an authenticated duplicate name"),
			!Valid.IsError()
				&& Valid.GetValue().State
					== EWoTPinnedAdjacentInstallerProbe::Valid
				&& Valid.GetValue().Verified.IsValid()
				&& Valid.GetValue().Verified->Source
					== EWoTInstallerSource::Downloads);
		FWoTInstallerLocation Adjacent;
		Adjacent.CanonicalPath = Duplicate;
		auto Refused = WoTInspectInstallerLocationForTesting(
			Adjacent, Fixture.Pins);
		TestTrue(TEXT("adjacent still requires the pinned leaf"),
			Refused.IsError()
				&& Refused.GetError().Code == EWoTBootstrapError::UnsafePath);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		FWoTInstallerLocation Location;
		Location.Source = EWoTInstallerSource::Downloads;
		Location.CanonicalPath = Fixture.CanonicalPath;
		Location.bRequirePinnedName = false;
		TestTrue(TEXT("partial fixture is written"),
			Fixture.WriteCanonical(WoTInstallerAcquisitionSpecBytes("abcd")));
		auto Partial = WoTInspectInstallerLocationForTesting(
			Location, Fixture.Pins);
		TestTrue(TEXT("a partial download waits"),
			!Partial.IsError()
				&& Partial.GetValue().State
					== EWoTPinnedAdjacentInstallerProbe::Downloading);
		TestTrue(TEXT("oversize fixture is written"),
			Fixture.WriteCanonical(
				WoTInstallerAcquisitionSpecBytes("abcdefghi")));
		auto Oversize = WoTInspectInstallerLocationForTesting(
			Location, Fixture.Pins);
		TestTrue(TEXT("an oversize download is wrong"),
			!Oversize.IsError()
				&& Oversize.GetValue().State
					== EWoTPinnedAdjacentInstallerProbe::NotTheInstaller);
		TestTrue(TEXT("wrong-digest fixture is written at pinned size"),
			Fixture.WriteCanonical(Fixture.Bad));
		auto WrongDigest = WoTTryAcquireInstallerLocationForTesting(
			Location, Fixture.Pins);
		TestTrue(TEXT("an equal-size wrong digest is wrong"),
			!WrongDigest.IsError()
				&& WrongDigest.GetValue().State
					== EWoTPinnedAdjacentInstallerProbe::NotTheInstaller);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		FWoTInstallerLocation Location;
		Location.Source = EWoTInstallerSource::Downloads;
		Location.CanonicalPath = Fixture.CanonicalPath;
		Location.bRequirePinnedName = false;
		auto First = WoTInspectInstallerLocationForTesting(
			Location, Fixture.Pins);
		if (!TestFalse(TEXT("first location inspection succeeds"),
				First.IsError())
			|| !TestTrue(TEXT("first location inspection captures its parent"),
				Location.ParentIdentity.IsSet()))
		{
			return false;
		}
		const FString Original = Fixture.Root + TEXT("-original");
		TestTrue(TEXT("captured location parent is substituted"),
			IFileManager::Get().Move(
				*Original, *Fixture.Root, true, false, false, true)
			&& IFileManager::Get().MakeDirectory(*Fixture.Root, true));
		auto Replaced = WoTInspectInstallerLocationForTesting(
			Location, Fixture.Pins);
		TestTrue(TEXT("captured parent substitution is enforced"),
			Replaced.IsError()
				&& WoTInstallerAcquisitionIsParentIdentityMismatch(
					Replaced.GetError()));
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		const FString CompletePath = FPaths::Combine(
			Fixture.Root, TEXT("complete-duplicate.exe"));
		TestTrue(TEXT("partial primary is written"),
			Fixture.WriteCanonical(WoTInstallerAcquisitionSpecBytes("abcd")));
		TestTrue(TEXT("complete duplicate is written"),
			FFileHelper::SaveArrayToFile(Fixture.Good, *CompletePath));
		FWoTInstallerLocation Partial;
		Partial.Source = EWoTInstallerSource::Downloads;
		Partial.CanonicalPath = Fixture.CanonicalPath;
		Partial.bRequirePinnedName = false;
		FWoTInstallerLocation Complete;
		Complete.Source = EWoTInstallerSource::Downloads;
		Complete.CanonicalPath = CompletePath;
		Complete.bRequirePinnedName = false;
		auto Waiting = WoTInspectInstallerLocationForTesting(
			Partial, Fixture.Pins);
		auto Acquired = WoTTryAcquireInstallerLocationForTesting(
			Complete, Fixture.Pins);
		TestTrue(TEXT("a partial candidate does not block a complete duplicate"),
			!Waiting.IsError()
				&& Waiting.GetValue().State
					== EWoTPinnedAdjacentInstallerProbe::Downloading
				&& !Acquired.IsError()
				&& Acquired.GetValue().State
					== EWoTPinnedAdjacentInstallerProbe::Valid);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("wrong-type fixture is created"),
			IFileManager::Get().MakeDirectory(*Fixture.CanonicalPath));
		auto WrongType = Fixture.Inspect();
		TestFalse(TEXT("wrong-type inspection succeeds"), WrongType.IsError());
		if (!WrongType.IsError())
		{
			TestEqual(TEXT("wrong type is replaceable"),
				WrongType.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::NotTheInstaller);
		}
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		const FString Source = FPaths::Combine(Fixture.Root, TEXT("source.exe"));
		TestTrue(TEXT("hard-link source is written"),
			FFileHelper::SaveArrayToFile(Fixture.Good, *Source));
		TestTrue(TEXT("hard link is created"),
			WoTInstallerAcquisitionSpecCreateHardLink(
				Fixture.CanonicalPath, Source));
		auto HardLink = Fixture.Inspect();
		TestFalse(TEXT("hard-link inspection succeeds"), HardLink.IsError());
		if (!HardLink.IsError())
		{
			TestEqual(TEXT("hard link is replaceable"),
				HardLink.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::NotTheInstaller);
		}
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("valid adjacent fixture is written"),
			Fixture.WriteCanonical(Fixture.Good));
		int32 Hashes = 0;
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		auto Valid = Fixture.Acquire(Hooks);
		TestFalse(TEXT("valid adjacent acquisition succeeds"), Valid.IsError());
		if (!Valid.IsError())
		{
			TestEqual(TEXT("valid adjacent state"), Valid.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::Valid);
			TestTrue(TEXT("valid result owns the held snapshot"),
				Valid.GetValue().Verified.IsValid()
				&& Valid.GetValue().Verified->Snapshot.Handle.IsValid());
			if (Valid.GetValue().Verified.IsValid())
			{
				TestEqual(TEXT("valid source is adjacent"),
					Valid.GetValue().Verified->Source,
					EWoTInstallerSource::Adjacent);
				IFileManager::Get().Delete(*Fixture.CanonicalPath);
				TestTrue(TEXT("replacement is written after authentication"),
					Fixture.WriteCanonical(Fixture.Bad));
				TArray<uint8> HeldBytes;
				HeldBytes.SetNumUninitialized(Fixture.Good.Num());
				auto Read = ReadNativeFileAt(
					Valid.GetValue().Verified->Snapshot.Handle, 0, HeldBytes);
				TestTrue(TEXT("held snapshot remains the authenticated object"),
					!Read.IsError()
						&& Read.GetValue()
							== static_cast<uint64>(Fixture.Good.Num())
						&& HeldBytes == Fixture.Good);
			}
		}
		TestEqual(TEXT("valid adjacent file is hashed once"), Hashes, 1);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("wrong-digest fixture is written"),
			Fixture.WriteCanonical(Fixture.Bad));
		int32 Hashes = 0;
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		auto WrongDigest = Fixture.Acquire(Hooks);
		TestFalse(TEXT("wrong digest is a classification"),
			WrongDigest.IsError());
		if (!WrongDigest.IsError())
		{
			TestEqual(TEXT("wrong digest is replaceable"),
				WrongDigest.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::NotTheInstaller);
		}
		TestEqual(TEXT("wrong digest requires one full hash"), Hashes, 1);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("mutation fixture is written"),
			Fixture.WriteCanonical(Fixture.Good));
		const FString Companion =
			FPaths::Combine(Fixture.Root, TEXT("companion.exe"));
		bool bLinked = false;
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.BeforeSnapshot = [&]()
		{
			bLinked = WoTInstallerAcquisitionSpecCreateHardLink(
				Companion, Fixture.CanonicalPath);
		};
		auto Mutated = Fixture.Acquire(Hooks);
		TestTrue(TEXT("after-hash link mutation was made"), bLinked);
		TestTrue(TEXT("identity mutation is refused"),
			Mutated.IsError()
				&& WoTInstallerAcquisitionIsIdentityMutation(
					Mutated.GetError()));
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("open-error fixture is written"),
			Fixture.WriteCanonical(Fixture.Good));
		int32 OpenAttempts = 0;
		int32 Hashes = 0;
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.BeforeHeldOpen = [&]()
		{
			++OpenAttempts;
			IFileManager::Get().Delete(*Fixture.CanonicalPath);
		};
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		auto OpenFailed = Fixture.Acquire(Hooks);
		TestTrue(TEXT("a permanent held-open error is propagated once"),
			OpenFailed.IsError()
				&& !WoTInstallerAcquisitionIsIdentityMutation(
					OpenFailed.GetError()));
		TestEqual(TEXT("a permanent held-open error does not retry"),
			OpenAttempts, 1);
		TestEqual(TEXT("a permanent held-open error does not hash"), Hashes, 0);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("path-substitution fixture is written"),
			Fixture.WriteCanonical(Fixture.Good));
		const FString Renamed =
			FPaths::Combine(Fixture.Root, TEXT("renamed-installer.exe"));
		bool bSubstituted = false;
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.BeforeSnapshot = [&]()
		{
			bSubstituted = IFileManager::Get().Move(
				*Renamed, *Fixture.CanonicalPath, true, false, false, true)
				&& Fixture.WriteCanonical(Fixture.Good);
		};
		auto Substituted = Fixture.Acquire(Hooks);
		TestTrue(TEXT("the held installer name is substituted"), bSubstituted);
		TestTrue(TEXT("canonical path substitution is fatal, not retryable"),
			Substituted.IsError()
				&& Substituted.GetError().Code == EWoTBootstrapError::UnsafePath
				&& !WoTInstallerAcquisitionIsIdentityMutation(
					Substituted.GetError()));
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		TestTrue(TEXT("stability fixture is written"),
			Fixture.WriteCanonical(Fixture.Good));
		auto First = Fixture.Inspect();
		if (!TestFalse(TEXT("first stable inspection succeeds"), First.IsError())
			|| !TestTrue(TEXT("first stable identity exists"),
				First.GetValue().Identity.IsSet()))
		{
			return false;
		}
		const FWoTNativeFileIdentity FirstIdentity =
			First.GetValue().Identity.GetValue();
		IFileManager::Get().Delete(*Fixture.CanonicalPath);
		TestTrue(TEXT("stability fixture is replaced"),
			Fixture.WriteCanonical(Fixture.Good));
		int32 Hashes = 0;
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		auto Replaced = Fixture.Acquire(Hooks, nullptr, &FirstIdentity);
		TestTrue(TEXT("replacement before hash is a retry"),
			Replaced.IsError()
				&& WoTInstallerAcquisitionIsIdentityMutation(
					Replaced.GetError()));
		TestEqual(TEXT("unstable replacement is never hashed"), Hashes, 0);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		auto Parent = OpenNativeDirectoryNoFollow(Fixture.Root);
		if (!TestFalse(TEXT("parent identity fixture opens"), Parent.IsError()))
		{
			return false;
		}
		const FWoTNativeFileIdentity Expected =
			Parent.GetValue().GetIdentity();
		Parent = TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Success(
			FWoTNativeDirectoryHandle());
		const FString Original = Fixture.Root + TEXT("-original");
		TestTrue(TEXT("parent is substituted"),
			IFileManager::Get().Move(
				*Original, *Fixture.Root, true, false, false, true)
			&& IFileManager::Get().MakeDirectory(*Fixture.Root, true));
		auto Substituted = Fixture.Inspect(&Expected);
		TestTrue(TEXT("parent substitution is fatal"),
			Substituted.IsError()
				&& WoTInstallerAcquisitionIsParentIdentityMismatch(
					Substituted.GetError()));
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		auto Parent = OpenNativeDirectoryNoFollow(Fixture.Root);
		if (!TestFalse(TEXT("writable parent opens"), Parent.IsError()))
		{
			return false;
		}
		const FWoTNativeFileIdentity Expected =
			Parent.GetValue().GetIdentity();
		auto Writable = WoTProbeAdjacentInstallerParentWritable(
			Fixture.CanonicalPath, &Expected);
		TestTrue(TEXT("authenticated writable parent is detected"),
			!Writable.IsError() && Writable.GetValue());
		TArray<FString> ProbeFiles;
		IFileManager::Get().FindFiles(ProbeFiles,
			*FPaths::Combine(Fixture.Root, TEXT(".wot-write-probe-*")),
			true, false);
		TestEqual(TEXT("the writability probe never leaves a file"),
			ProbeFiles.Num(), 0);
	}
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		auto Parent = OpenNativeDirectoryNoFollow(Fixture.Root);
		if (!TestFalse(TEXT("collision-probe parent opens"), Parent.IsError()))
		{
			return false;
		}
		const FString Occupied = TEXT("occupied-probe");
		auto Existing = CreateNativeFileAtWriteNoFollow(
			Parent.GetValue(), Occupied);
		if (!TestFalse(TEXT("collision-probe leaf is occupied"),
			Existing.IsError()))
		{
			return false;
		}
		Existing = TWoTBootstrapResult<FWoTNativeFileHandle>::Success(
			FWoTNativeFileHandle());
		auto CannotCreate = ProbeNativeDirectoryWritableNoFollow(
			Parent.GetValue(), Occupied);
#if PLATFORM_WINDOWS
		// Win64 probes with a handle-bound create-new. An occupied leaf is not
		// a permission refusal, so the native collision must remain an error.
		TestTrue(TEXT("an occupied create-new leaf preserves its native collision"),
			CannotCreate.IsError()
				&& CannotCreate.GetError().NativeCode == ERROR_ALREADY_EXISTS);
		TestTrue(TEXT("the failed probe does not delete the existing leaf"),
			IFileManager::Get().FileExists(
				*FPaths::Combine(Fixture.Root, Occupied)));
#else
		// Mac probes with faccessat(W_OK) on the folder and never creates a
		// leaf, so an occupied name does not matter.
		TestTrue(TEXT("faccessat reports a writable parent with an occupied leaf"),
			!CannotCreate.IsError() && CannotCreate.GetValue());
		TestTrue(TEXT("the probe leaves the existing leaf untouched"),
			IFileManager::Get().FileExists(
				*FPaths::Combine(Fixture.Root, Occupied)));
		TArray<FString> Entries;
		IFileManager::Get().FindFiles(Entries,
			*FPaths::Combine(Fixture.Root, TEXT("*")), true, false);
		TestEqual(TEXT("the faccessat probe creates nothing"),
			Entries.Num(), 1);
#endif
	}
#if PLATFORM_MAC
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		const FTCHARToUTF8 RootUtf8(*Fixture.Root);
		if (!TestEqual(TEXT("fixture folder becomes read-only"),
			chmod(RootUtf8.Get(), 0555), 0))
		{
			return false;
		}
		auto Parent = OpenNativeDirectoryNoFollow(Fixture.Root);
		const bool bOpened = !Parent.IsError();
		TWoTBootstrapResult<bool> Unwritable = bOpened
			? ProbeNativeDirectoryWritableNoFollow(
				Parent.GetValue(), TEXT("unused-leaf"))
			: TWoTBootstrapResult<bool>::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io, TEXT("read-only parent did not open")));
		chmod(RootUtf8.Get(), 0755);
		TestTrue(TEXT("read-only parent opens"), bOpened);
		TestTrue(TEXT("a read-only folder cannot receive the installer"),
			!Unwritable.IsError() && !Unwritable.GetValue());
	}
#endif
#if PLATFORM_WINDOWS
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		auto Parent = OpenNativeDirectoryNoFollow(Fixture.Root);
		if (!TestFalse(TEXT("rename-race parent opens"), Parent.IsError()))
		{
			return false;
		}
		const FWoTNativeFileIdentity Expected =
			Parent.GetValue().GetIdentity();
		bool bRaceAttempted = false;
		bool bRenamed = false;
		bool bReplacementCreated = false;
		FString ProbeLeafString;
		FString RenamedLeaf;
		auto Probed = WoTProbeAdjacentInstallerParentWritableForTesting(
			Fixture.CanonicalPath, &Expected,
			[&](FStringView ProbeLeaf)
			{
				bRaceAttempted = true;
				ProbeLeafString = FString(ProbeLeaf);
				RenamedLeaf = ProbeLeafString + TEXT("-renamed");
				FWoTBootstrapStatus Renamed = RenameNativeEntryAtNoFollow(
					Parent.GetValue(), ProbeLeaf, Parent.GetValue(),
					RenamedLeaf, false);
				bRenamed = !Renamed.IsError();
				if (bRenamed)
				{
					auto Replacement = CreateNativeFileAtWriteNoFollow(
						Parent.GetValue(), ProbeLeaf);
					bReplacementCreated = !Replacement.IsError();
				}
			});
		TestTrue(TEXT("the handle-bound probe succeeds during a rename race"),
			!Probed.IsError() && Probed.GetValue());
		TestTrue(TEXT("the racing rename is attempted while the handle is held"),
			bRaceAttempted);
		if (bRenamed)
		{
			TestTrue(TEXT("the racer can install a replacement at the old name"),
				bReplacementCreated);
			TestFalse(TEXT("delete-on-close follows the renamed probe object"),
				IFileManager::Get().FileExists(
					*FPaths::Combine(Fixture.Root, RenamedLeaf)));
			TestTrue(TEXT("delete-on-close never targets the racing replacement"),
				IFileManager::Get().FileExists(
					*FPaths::Combine(Fixture.Root, ProbeLeafString)));
		}
	}
#endif
	{
		FWoTInstallerAcquisitionSpecFixture Fixture;
		const FString Legacy = FPaths::Combine(Fixture.StateDir,
			TEXT("installer"));
		TestTrue(TEXT("legacy directory is created"),
			IFileManager::Get().MakeDirectory(*Legacy, true));
		TestTrue(TEXT("legacy partial is written"),
			FFileHelper::SaveStringToFile(TEXT("partial"),
				*FPaths::Combine(Legacy, TEXT("fixture.part"))));
		WoTRemoveLegacyInstallerStateBestEffort(Fixture.StateDir);
		TestFalse(TEXT("legacy installer state is retired"),
			IFileManager::Get().DirectoryExists(*Legacy));
	}
	return !HasAnyErrors();
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerOnlineOnlyAttributesSpec,
	"WoT.Bootstrap.InstallerAcquisitionOnlineOnlyHeldAttributes",
	WoTInstallerAcquisitionSpecFlags)

bool FWoTInstallerOnlineOnlyAttributesSpec::RunTest(const FString& Parameters)
{
	(void)Parameters;
	constexpr uint32 RecallOnOpen = 0x00040000u;
	constexpr uint32 RecallOnDataAccess = 0x00400000u;
	for (const uint32 Attribute :
		{static_cast<uint32>(FILE_ATTRIBUTE_OFFLINE), RecallOnOpen,
			RecallOnDataAccess})
	{
		TestFalse(TEXT("each recall/offline attribute is classified non-local"),
			WoTInstallerSourceAttributesAreFullyLocal(Attribute));
	}
	TestTrue(TEXT("a normal file is classified fully local"),
		WoTInstallerSourceAttributesAreFullyLocal(FILE_ATTRIBUTE_NORMAL));

	FWoTInstallerAcquisitionSpecFixture Fixture;
	if (!TestTrue(TEXT("the installer fixture is ready and written"),
			Fixture.IsReady() && Fixture.WriteCanonical(Fixture.Good)))
	{
		return false;
	}
	int32 ReadCalls = 0;
	WoTSetNativeInstallerSourceReadObserverForTesting(
		[&ReadCalls]() { ++ReadCalls; });
	ON_SCOPE_EXIT
	{
		WoTSetNativeInstallerSourceAttributeQueryForTesting({});
		WoTSetNativeInstallerSourceReadObserverForTesting({});
	};

	WoTSetNativeInstallerSourceAttributeQueryForTesting(
		[](const FWoTNativeFileHandle&)
		{
			return TWoTBootstrapResult<uint32>::Success(
				FILE_ATTRIBUTE_NORMAL | 0x00400000u);
		});
	for (const EWoTInstallerSource Source : {EWoTInstallerSource::Adjacent,
		EWoTInstallerSource::Downloads, EWoTInstallerSource::Picked})
	{
		FWoTInstallerLocation Location;
		Location.Source = Source;
		Location.CanonicalPath = Fixture.CanonicalPath;
		const FString Expected = FString::Printf(TEXT("The installer at %s is an "
			"online-only file. Make it available on this device (in File Explorer, "
			"right-click it and choose 'Always keep on this device'), then start "
			"WoT Reborn again."), *Fixture.CanonicalPath);
		auto Refused = WoTTryAcquireInstallerLocationForTesting(
			Location, Fixture.Pins);
		if (TestFalse(TEXT("the online-only acquisition returns a retryable result"),
				Refused.IsError()))
		{
			TestEqual(TEXT("the source is distinctly online-only"),
				Refused.GetValue().State,
				EWoTPinnedAdjacentInstallerProbe::OnlineOnly);
			TestEqual(TEXT("the production acquisition carries exact guidance"),
				Refused.GetValue().Message, Expected);
		}
	}
	TestEqual(TEXT("online-only adjacent, Downloads, and Browse read no bytes"),
		ReadCalls, 0);

	WoTSetNativeInstallerSourceAttributeQueryForTesting(
		[](const FWoTNativeFileHandle&)
		{
			return TWoTBootstrapResult<uint32>::Success(FILE_ATTRIBUTE_NORMAL);
		});
	FWoTInstallerLocation Local;
	Local.Source = EWoTInstallerSource::Adjacent;
	Local.CanonicalPath = Fixture.CanonicalPath;
	auto Acquired = WoTTryAcquireInstallerLocationForTesting(Local, Fixture.Pins);
	TestTrue(TEXT("a fully local source is authenticated through normal reads"),
		!Acquired.IsError()
			&& Acquired.GetValue().State
				== EWoTPinnedAdjacentInstallerProbe::Valid);
	TestTrue(TEXT("the fully local source reaches native ReadFile"), ReadCalls > 0);
	return !HasAnyErrors();
}
#endif

#endif
