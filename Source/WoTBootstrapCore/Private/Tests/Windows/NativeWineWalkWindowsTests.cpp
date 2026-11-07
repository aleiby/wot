// Windows spec: under Wine only Wine's own drive and mount mappings pass the
// no-follow walk; every other reparse point stays a refused link.
//
// Wine reports its drive roots and Unix mount points with the junction tag
// (IO_REPARSE_TAG_MOUNT_POINT), so a junction here carries exactly the tag the
// Wine allowance looks for. It sits at neither a drive root nor a volume
// boundary, so it must stay refused with Wine forced on as well as off, both
// opened absolutely and walked through. A symbolic link stays refused too, and
// each refusal records its reparse tag.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "../NativeHandleTestFactory.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTWineWalkSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

constexpr uint32 WoTTagMountPoint = 0xA0000003u;
constexpr uint32 WoTTagSymbolicLink = 0xA000000Cu;

FString WoTWineWalkBackslashes(FString Path)
{
	Path.ReplaceInline(TEXT("/"), TEXT("\\"));
	return Path;
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeWineLinkStillRefusedSpec,
	"WoT.BootstrapCore.Identity.WineLinkStillRefused", WoTWineWalkSpecFlags)

bool FWoTNativeWineLinkStillRefusedSpec::RunTest(const FString&)
{
	const FString Root = WoTNativeHandleTestFactory::CreateTemporaryRoot();
	ON_SCOPE_EXIT
	{
		WoTSetRunningUnderWineForTesting(TOptional<bool>());
		WoTNativeHandleTestFactory::RemoveTree(Root);
	};
	const FString Real = Root + TEXT("/Real");
	const FString Junction = Root + TEXT("/Junction");
	const FString Link = Root + TEXT("/Link");
	const uint8 Bytes[] = {'w', 'o', 't'};
	if (!TestTrue(TEXT("the fixture directory and junction are created"),
		WoTNativeHandleTestFactory::CreateDirectory(Real)
			&& WoTNativeHandleTestFactory::WriteFile(Real + TEXT("/Data.bin"), Bytes)
			&& WoTNativeHandleTestFactory::CreateDirectoryAncestorAlias(
				WoTWineWalkBackslashes(Junction), WoTWineWalkBackslashes(Real))))
	{
		return false;
	}
	// Symbolic links need developer mode on Windows; skip only that case.
	const bool bHaveLink = WoTNativeHandleTestFactory::CreateSymbolicLink(Link, Real);
	if (!bHaveLink)
	{
		AddInfo(TEXT("symbolic links are unavailable here; the symlink case is skipped"));
	}

	for (const bool bWine : {true, false})
	{
		WoTSetRunningUnderWineForTesting(bWine);
		const FString Mode = bWine ? TEXT("forced Wine") : TEXT("native");

		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Absolute =
			OpenNativeDirectoryNoFollow(Junction);
		if (TestTrue(Mode + TEXT(": an absolute open of a junction is refused"),
			Absolute.IsError()))
		{
			TestTrue(Mode + TEXT(": as an unsafe path"),
				Absolute.GetError().Code == EWoTBootstrapError::UnsafePath);
			TestEqual(Mode + TEXT(": recording the junction tag"),
				Absolute.GetError().NativeCode, WoTTagMountPoint);
		}

		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Parent =
			OpenNativeDirectoryNoFollow(Root);
		if (!TestFalse(Mode + TEXT(": the temporary root opens"), Parent.IsError()))
		{
			AddError(Parent.GetError().ToString());
			return false;
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> Walked =
			OpenNativeFileAtReadNoFollow(Parent.GetValue(), TEXT("Junction/Data.bin"));
		if (TestTrue(Mode + TEXT(": walking through a junction is refused"),
			Walked.IsError()))
		{
			TestTrue(Mode + TEXT(": the walk refusal is an unsafe path"),
				Walked.GetError().Code == EWoTBootstrapError::UnsafePath);
			TestEqual(Mode + TEXT(": the walk refusal records the junction tag"),
				Walked.GetError().NativeCode, WoTTagMountPoint);
		}

		if (bHaveLink)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Linked =
				OpenNativeDirectoryNoFollow(Link);
			if (TestTrue(Mode + TEXT(": a symbolic link is refused"), Linked.IsError()))
			{
				TestEqual(Mode + TEXT(": recording the symbolic link tag"),
					Linked.GetError().NativeCode, WoTTagSymbolicLink);
			}
		}

		// A relative open that ENDS on the junction is refused the same way.
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Final =
			OpenNativeDirectoryAtNoFollow(Parent.GetValue(), TEXT("Junction"));
		if (TestTrue(Mode + TEXT(": a relative open ending on a junction is refused"),
			Final.IsError()))
		{
			TestEqual(Mode + TEXT(": recording the junction tag"),
				Final.GetError().NativeCode, WoTTagMountPoint);
		}

		// The real directory still opens and walks: the allowance changed nothing
		// else, and a held directory re-queries to the identity it was adopted with.
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Held = OpenNativeDirectoryNoFollow(Real);
		if (TestFalse(Mode + TEXT(": the real directory still opens"), Held.IsError()))
		{
			TWoTBootstrapResult<FWoTNativeFileIdentity> Requeried =
				Held.GetValue().QueryCurrentIdentity();
			TestTrue(Mode + TEXT(": a held directory re-queries to the same identity"),
				!Requeried.IsError() && Requeried.GetValue() == Held.GetValue().GetIdentity());
		}
		TestFalse(Mode + TEXT(": walking through the real directory still works"),
			OpenNativeFileAtReadNoFollow(Parent.GetValue(), TEXT("Real/Data.bin")).IsError());
	}
	return !HasAnyErrors();
}

#endif
