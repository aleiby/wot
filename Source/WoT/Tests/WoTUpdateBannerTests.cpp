#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Payload/WoTPayloadActivation.h"
#include "../UI/WoTUpdateBanner.h"
#include "../Updater/WoTReleaseIdentity.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTMenuStyle.h"
#include "../WoTSessionSubsystem.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Input/HittestGrid.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "OnlineSessionSettings.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Types/PaintArgs.h"
#include "Types/NavigationMetaData.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr EAutomationTestFlags UpdateBannerSpecFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	class FUpdateBannerFeedback final : public IWoTFrontEndFeedbackSink
	{
	public:
		virtual bool IsReady() const override { return true; }
		virtual void MoveSelector() override {}
		virtual void Select() override {}
	};

	TSharedRef<FWoTFrontEndPresentationAuthority> UpdateBannerAuthority()
	{
		TMap<FName, FString> Labels;
		Labels.Add(FName(TEXT("menuMain.MenuList[2]")), TEXT("Multiplayer"));
		Labels.Add(FName(TEXT("menuMain.MenuList[3]")), TEXT("Controls"));
		Labels.Add(FName(TEXT("menuMain.MenuList[4]")), TEXT("Hardware"));
		Labels.Add(FName(TEXT("menuMain.MenuList[7]")), TEXT("Quit"));
		return FWoTFrontEndPresentationAuthority::CreateForTesting(
			TEXT("update-banner-fixture"),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
			MoveTemp(Labels), MakeShared<FUpdateBannerFeedback>());
	}

	UWoTBootstrapSubsystem* UpdateBannerBootstrap(uint64 RunningSequence = 5)
	{
		UWoTGameInstance* Owner = NewObject<UWoTGameInstance>(GEngine);
		UWoTBootstrapSubsystem* Bootstrap =
			NewObject<UWoTBootstrapSubsystem>(Owner);
		Bootstrap->SetRunningReleaseSequenceForTesting(RunningSequence);
		return Bootstrap;
	}

	void PublishUpdateState(UWoTBootstrapSubsystem& Bootstrap,
		EWoTUpdateStatus Status, uint64 Sequence = 0,
		const TCHAR* Reason = nullptr, const TCHAR* Url = nullptr)
	{
		FWoTUpdateState State;
		State.Status = Status;
		State.ReleaseSequence = Sequence;
		if (Reason != nullptr) State.Reason = Reason;
		if (Url != nullptr) State.ReleasePageUrl = Url;
		else if (Status == EWoTUpdateStatus::Ready) State.ReleasePageUrl = TEXT("https://example.invalid/release/ready");
		Bootstrap.DispatchUpdateStateForTesting(MoveTemp(State));
	}

	void CollectUpdateBannerText(const TSharedRef<SWidget>& Widget,
		TArray<FString>& OutText)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			OutText.Add(
				StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				CollectUpdateBannerText(Children->GetChildAt(Index), OutText);
			}
		}
	}

	void CollectUpdateBannerTextWidgets(const TSharedRef<SWidget>& Widget,
		TArray<TSharedRef<STextBlock>>& OutWidgets)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			OutWidgets.Add(StaticCastSharedRef<STextBlock>(Widget));
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				CollectUpdateBannerTextWidgets(
					Children->GetChildAt(Index), OutWidgets);
			}
		}
	}

	bool FakeUpdateTokensAreHarnessGuarded(const FString& RelativePath,
		TConstArrayView<const TCHAR*> Tokens)
	{
		FString Source;
		if (!FFileHelper::LoadFileToString(Source,
			*FPaths::Combine(FPaths::ProjectDir(), RelativePath)))
		{
			return false;
		}
		TArray<bool> SeenTokens;
		SeenTokens.Init(false, Tokens.Num());
		TArray<FString> Lines;
		Source.ParseIntoArrayLines(Lines, false);
		TArray<bool> DirectHarnessGuards;
		int32 HarnessGuardCount = 0;
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStart();
			if (Trimmed.StartsWith(TEXT("#if ")))
			{
				const bool bDirectHarnessGuard = Trimmed.Equals(
					TEXT("#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES"),
					ESearchCase::CaseSensitive);
				DirectHarnessGuards.Add(bDirectHarnessGuard);
				HarnessGuardCount += bDirectHarnessGuard ? 1 : 0;
				continue;
			}
			if (Trimmed.StartsWith(TEXT("#endif")))
			{
				if (!DirectHarnessGuards.IsEmpty())
				{
					HarnessGuardCount -= DirectHarnessGuards.Pop() ? 1 : 0;
				}
				continue;
			}
			for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); ++TokenIndex)
			{
				if (!Line.Contains(Tokens[TokenIndex],
					ESearchCase::CaseSensitive))
				{
					continue;
				}
				if (HarnessGuardCount == 0) return false;
				SeenTokens[TokenIndex] = true;
			}
		}
		return !SeenTokens.Contains(false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateBannerPresentationSpec,
	"WoT.FrontEnd.UpdateBanner.Presentation", UpdateBannerSpecFlags)

bool FWoTUpdateBannerPresentationSpec::RunTest(const FString&)
{
	struct FCase
	{
		EWoTUpdateStatus Status;
		const TCHAR* Reason;
		const TCHAR* Url;
		bool bVisible;
		const TCHAR* Primary;
		const TCHAR* ReasonLine;
		const TCHAR* MultiplayerLine;
		EWoTUpdateBannerAction Action;
		const TCHAR* Button;
	};
	const FCase Cases[] = {
		{EWoTUpdateStatus::None, nullptr, nullptr, false, TEXT(""), TEXT(""),
			TEXT(""), EWoTUpdateBannerAction::None, TEXT("")},
		{EWoTUpdateStatus::Checking, nullptr, nullptr, false, TEXT(""), TEXT(""),
			TEXT(""), EWoTUpdateBannerAction::None, TEXT("")},
		{EWoTUpdateStatus::Downloading, nullptr, nullptr, false, TEXT(""), TEXT(""),
			TEXT(""), EWoTUpdateBannerAction::None, TEXT("")},
		{EWoTUpdateStatus::Ready, nullptr, nullptr, true,
			TEXT("Update ready; restart to install"), TEXT("Open WoT Reborn again to finish installing."),
			TEXT("Players on the new version can't join you until you update."),
			EWoTUpdateBannerAction::Restart, TEXT("Quit to install")},
		// A release page never turns Ready back into a download prompt.
		{EWoTUpdateStatus::Ready, nullptr, TEXT("https://example.invalid/release/12"), true,
			TEXT("Update ready; restart to install"), TEXT("Open WoT Reborn again to finish installing."),
			TEXT("Players on the new version can't join you until you update."),
			EWoTUpdateBannerAction::Restart, TEXT("Quit to install")},
		{EWoTUpdateStatus::Available, TEXT("install-root-not-writable"),
			TEXT("https://example.invalid/release/12"), true,
			TEXT("A new version is available."),
			TEXT("This folder can't be updated in place; download the new version from the release page."),
			TEXT("Players on the new version can't join you until you update."),
			EWoTUpdateBannerAction::OpenReleasePage, TEXT("Open release page")},
		// A failed download or staging step tells the player instead of the
		// Downloading banner silently disappearing.
		{EWoTUpdateStatus::Available, TEXT("download-failed"),
			TEXT("https://example.invalid/release/12"), true,
			TEXT("A new version is available."),
			TEXT("The update couldn't be downloaded. Check your free disk space and connection; WoT Reborn will try again next time you start it."),
			TEXT("Players on the new version can't join you until you update."),
			EWoTUpdateBannerAction::OpenReleasePage, TEXT("Open release page")},
		{EWoTUpdateStatus::Available, TEXT("translocated"),
			TEXT("https://example.invalid/release/12"), true,
			TEXT("A new version is available."),
			TEXT("Move WoT to your Applications folder (or any folder) to enable automatic updates."),
			TEXT("Players on the new version can't join you until you update."),
			EWoTUpdateBannerAction::OpenReleasePage, TEXT("Open release page")},
		{EWoTUpdateStatus::Available, TEXT("reserved-live-leaf"),
			TEXT("https://example.invalid/release/12"), true,
			TEXT("A new version is available."),
			TEXT("This copy can't update itself; download the new version from the release page."),
			TEXT("Players on the new version can't join you until you update."),
			EWoTUpdateBannerAction::OpenReleasePage, TEXT("Open release page")},
		{EWoTUpdateStatus::Available, TEXT("update-in-progress"),
			TEXT("https://example.invalid/release/12"), false, TEXT(""), TEXT(""),
			TEXT(""), EWoTUpdateBannerAction::None, TEXT("")},
		{EWoTUpdateStatus::Available, TEXT("update-state-busy"),
			TEXT("https://example.invalid/release/12"), false, TEXT(""), TEXT(""),
			TEXT(""), EWoTUpdateBannerAction::None, TEXT("")},
		{EWoTUpdateStatus::KnownBad, nullptr, nullptr, true,
			TEXT("The new version could not start on this computer; you'll get the next version."),
			TEXT(""), TEXT(""), EWoTUpdateBannerAction::None, TEXT("")},
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Cases); ++Index)
	{
		const FCase& Case = Cases[Index];
		FWoTUpdateState State;
		State.Status = Case.Status;
		State.ReleaseSequence = 12;
		if (Case.Reason != nullptr) State.Reason = Case.Reason;
		if (Case.Url != nullptr) State.ReleasePageUrl = Case.Url;
		const FWoTUpdateBannerPresentation Banner =
			WoTBuildUpdateBannerPresentation(State);
		const FString Prefix = FString::Printf(TEXT("case %d "), Index);
		TestEqual(Prefix + TEXT("visibility"), Banner.bVisible, Case.bVisible);
		TestEqual(Prefix + TEXT("primary"), Banner.PrimaryLine,
			FString(Case.Primary));
		TestEqual(Prefix + TEXT("reason"), Banner.ReasonLine,
			FString(Case.ReasonLine));
		TestEqual(Prefix + TEXT("multiplayer"), Banner.MultiplayerLine,
			FString(Case.MultiplayerLine));
		TestEqual(Prefix + TEXT("action"), static_cast<int32>(Banner.Action),
			static_cast<int32>(Case.Action));
		TestEqual(Prefix + TEXT("button"), Banner.ButtonLabel,
			FString(Case.Button));
	}

	for (const TCHAR* Url : {TEXT(""), TEXT("https:///x"), TEXT("https://"),
		TEXT("https://example.invalid/a b"),
		TEXT("https://example.invalid/\"release"),
		TEXT("https://example.invalid/\trelease"),
		TEXT("https://example.invalid/\nrelease"),
		TEXT("https://example.invalid\\release"),
		TEXT("javascript:alert(1)"), TEXT("http://example.invalid/release"),
		TEXT("https://user@example.invalid/release"),
		TEXT("HTTPS://example.invalid/release"),
		TEXT("https://example.invalid/%zz"), TEXT("file:///release"),
		TEXT("https://example.com/path[bad]"),
		TEXT("https://example.com/#a#b"), TEXT("https://example.com/?q"),
		TEXT("https://[::1]/x"), TEXT("https://127.0.0.1/x"),
		TEXT("https://a..b/x"), TEXT("https://-a.com/x"),
		TEXT("https://a-.com/x"), TEXT("https://a.com:0/x"),
		TEXT("https://a.com:65536/x"), TEXT("https://0x7f000001/x"),
		TEXT("https://0x7f.0.0.1/x"), TEXT("https://2130706433/x"),
		TEXT("https://127.1/x"), TEXT("https://localhost/x"),
		TEXT("https://example.c0m/x")})
	{
		FWoTUpdateState State;
		State.Status = EWoTUpdateStatus::Available;
		State.ReleaseSequence = 12;
		State.ReleasePageUrl = Url;
		const FWoTUpdateBannerPresentation Banner =
			WoTBuildUpdateBannerPresentation(State);
		TestEqual(FString::Printf(TEXT("unsafe URL has no action: %s"), Url),
			static_cast<int32>(Banner.Action),
			static_cast<int32>(EWoTUpdateBannerAction::None));
		TestTrue(FString::Printf(TEXT("unsafe URL has no button: %s"), Url),
			Banner.ButtonLabel.IsEmpty());
	}
	const FString OversizedUrl = TEXT("https://example.com/")
		+ FString::ChrN(2049, TEXT('a'));
	TestFalse(TEXT("release URL over 2048 characters is rejected"),
		WoTIsHttpsReleasePageUrl(OversizedUrl));
	TestTrue(TEXT("strict GitHub release URL is accepted"),
		WoTIsHttpsReleasePageUrl(
			TEXT("https://github.com/Open-WoT/wot/releases/tag/v13.2-rc_1")));

	// A release that names a human version is called by it, as the main-menu
	// corner calls the running one; a bare sequence is never shown.
	FWoTUpdateState Named;
	Named.Status = EWoTUpdateStatus::Available;
	Named.ReleaseSequence = 12;
	Named.HumanVersion = TEXT("0.1.2");
	Named.Reason = TEXT("translocated");
	const FWoTUpdateBannerPresentation NamedBanner =
		WoTBuildUpdateBannerPresentation(Named);
	TestEqual(TEXT("human version names the available release"),
		NamedBanner.PrimaryLine, FString(TEXT("Version 0.1.2 is available.")));
	TestEqual(TEXT("human version names the multiplayer gap"),
		NamedBanner.MultiplayerLine,
		FString(TEXT("Players on version 0.1.2 can't join you until you update.")));
	Named.Status = EWoTUpdateStatus::Ready;
	TestEqual(TEXT("Ready names the human version too"),
		WoTBuildUpdateBannerPresentation(Named).MultiplayerLine,
		FString(TEXT("Players on version 0.1.2 can't join you until you update.")));
	Named.Status = EWoTUpdateStatus::KnownBad;
	TestEqual(TEXT("KnownBad names the human version too"),
		WoTBuildUpdateBannerPresentation(Named).PrimaryLine,
		FString(TEXT("Version 0.1.2 could not start on this computer; you'll get the next version.")));
	Named.Status = EWoTUpdateStatus::Available;
	Named.HumanVersion = TEXT("v0.1.2 <b>");
	TestEqual(TEXT("an undisplayable human version is never shown"),
		WoTBuildUpdateBannerPresentation(Named).PrimaryLine,
		FString(TEXT("A new version is available.")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateBannerStructureSpec,
	"WoT.FrontEnd.UpdateBanner.Structure", UpdateBannerSpecFlags)

bool FWoTUpdateBannerStructureSpec::RunTest(const FString&)
{
	FString Source;
	if (!TestTrue(TEXT("front-end source is readable"),
		FFileHelper::LoadFileToString(Source, *FPaths::Combine(
			FPaths::ProjectDir(), TEXT("Source/WoT/WoTFrontEndSlate.cpp")))))
	{
		return false;
	}
	const int32 Start = Source.Find(
		TEXT("TSharedRef<SWidget> SWoTFrontEndRoot::BuildUpdateBanner()"),
		ESearchCase::CaseSensitive);
	const int32 End = Source.Find(
		TEXT("TSharedRef<SWidget> SWoTFrontEndRoot::BuildPlayerConfigScreen()"),
		ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 1);
	if (!TestTrue(TEXT("banner implementation is bounded"),
		Start != INDEX_NONE && End > Start))
	{
		return false;
	}
	const FString BannerSource = Source.Mid(Start, End - Start);
	TestFalse(TEXT("banner text never uses AutoWrapText"),
		BannerSource.Contains(TEXT(".AutoWrapText("), ESearchCase::CaseSensitive));
	TestTrue(TEXT("banner box has a definite width override"),
		BannerSource.Contains(TEXT("SAssignNew(UpdateBannerWidget, SBox)"),
			ESearchCase::CaseSensitive)
		&& BannerSource.Contains(TEXT(".WidthOverride_Lambda("),
			ESearchCase::CaseSensitive));
	TestTrue(TEXT("banner uses a non-wrapping horizontal row"),
		BannerSource.Contains(TEXT("SNew(SHorizontalBox)"),
			ESearchCase::CaseSensitive)
		&& !BannerSource.Contains(TEXT("SNew(SWrapBox)"),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateBannerVersionLabelSpec,
	"WoT.FrontEnd.UpdateBanner.MainMenuVersionLabel", UpdateBannerSpecFlags)

bool FWoTUpdateBannerVersionLabelSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("UpdateBannerVersion-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Directory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	const FString IdentityPath = Directory / TEXT("release-identity.json");
	TestTrue(TEXT("stamped identity writes"), FFileHelper::SaveStringToFile(
		TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456def\",\"release_sequence\":5,\"version\":1}"),
		*IdentityPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	WoTReleaseIdentity::FScopedPathOverrideForTesting Override(IdentityPath);
	auto MakeRoot = []()
	{
		const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
			.PlayerController(nullptr)
			.PresentationAuthorityForTesting(UpdateBannerAuthority())
			.BootstrapForTesting(UpdateBannerBootstrap());
		Root->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f),
			FSlateLayoutTransform()), 0.0, 0.0f);
		Root->SlatePrepass(1.0f);
		return Root;
	};
	auto RenderedLabel = [](const TSharedRef<SWoTFrontEndRoot>& Root)
	{
		const TSharedPtr<SWidget> Widget = Root->VersionLabelWidgetForTesting();
		return Widget.IsValid()
			? StaticCastSharedPtr<STextBlock>(Widget)->GetText().ToString()
			: FString();
	};

	const TSharedRef<SWoTFrontEndRoot> Unversioned = MakeRoot();
	TestEqual(TEXT("an unversioned stamped package shows dev and its commit"),
		Unversioned->VersionLabelForTesting(), FString(TEXT("dev abc123456")));
	TestEqual(TEXT("the main menu renders the label"),
		RenderedLabel(Unversioned), FString(TEXT("dev abc123456")));

	TestTrue(TEXT("release version writes"), FFileHelper::SaveStringToFile(
		TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.2\",\"version\":1}"),
		*(Directory / TEXT("release-version.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestEqual(TEXT("a versioned package shows its human version"),
		RenderedLabel(MakeRoot()), FString(TEXT("v0.1.2")));
	return !HasAnyErrors();
}

/** Empties the live onboarding menu-row registry; restores on exit. The
    banner's focus/navigation contract is with the LAST RETAIL entry (Quit),
    so an installed improvement module's appended row must not become the
    neighbor under test. */
struct FWoTUpdateBannerMenuRowsGuard
{
	TArray<FWoTOnboardingRow> Saved;

	FWoTUpdateBannerMenuRowsGuard()
	{
		Saved = MoveTemp(FWoTOnboardingSeams::Get().MenuRows);
		FWoTOnboardingSeams::Get().MenuRows.Reset();
	}

	~FWoTUpdateBannerMenuRowsGuard()
	{
		FWoTOnboardingSeams::Get().MenuRows = MoveTemp(Saved);
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateBannerLiveActionSpec,
	"WoT.FrontEnd.UpdateBanner.LiveActions", UpdateBannerSpecFlags)

bool FWoTUpdateBannerLiveActionSpec::RunTest(const FString&)
{
	FWoTUpdateBannerMenuRowsGuard MenuRowsGuard;
	UWoTBootstrapSubsystem* Bootstrap = UpdateBannerBootstrap();
	int32 Launches = 0;
	FString LaunchedUrl;
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(UpdateBannerAuthority())
		.BootstrapForTesting(Bootstrap)
		.ReleasePageLaunchForTesting_Lambda(
			[&Launches, &LaunchedUrl](const FString& Url)
			{
				++Launches;
				LaunchedUrl = Url;
			});
	Root->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	Root->SlatePrepass(1.0f);
	const uint64 Rebuilds = Root->ScreenRebuildCountForTesting();
	const TSharedPtr<SWidget> InitialFocus = Root->InitialFocusWidget();
	TestTrue(TEXT("None starts with no banner"),
		Root->UpdateBannerWidgetForTesting().IsValid()
			&& Root->UpdateBannerWidgetForTesting()->GetVisibility()
				== EVisibility::Collapsed);

	PublishUpdateState(*Bootstrap, EWoTUpdateStatus::Ready, 12);
	Root->SlatePrepass(1.0f);
	TestEqual(TEXT("live publication does not rebuild the screen"),
		Root->ScreenRebuildCountForTesting(), Rebuilds);
	TestTrue(TEXT("live Ready appears through bound Slate attributes"),
		Root->UpdateBannerWidgetForTesting()->GetVisibility().IsVisible());
	TestTrue(TEXT("live Ready preserves the main-menu focus target"),
		Root->InitialFocusWidget() == InitialFocus
			&& InitialFocus == Root->MainMenuButtonForTesting(0));
	TArray<FString> ReadyText;
	CollectUpdateBannerText(
		Root->UpdateBannerWidgetForTesting().ToSharedRef(), ReadyText);
	TestTrue(TEXT("live Ready renders the primary line"), ReadyText.Contains(TEXT("Update ready; restart to install")));
	TestTrue(TEXT("live Ready renders the multiplayer line"), ReadyText.Contains(
		TEXT("Players on the new version can't join you until you update.")));

	const TSharedPtr<SButton> BannerButton =
		Root->UpdateBannerButtonForTesting();
	const TSharedPtr<SWoTMenuButton> LastMenu = Root->MainMenuButtonForTesting(3);
	if (TestTrue(TEXT("Ready exposes a keyboard-focusable button"),
		BannerButton.IsValid() && BannerButton->SupportsKeyboardFocus())
		&& TestTrue(TEXT("last menu button exists"), LastMenu.IsValid()))
	{
		const TSharedPtr<FNavigationMetaData> Down =
			LastMenu->GetMetaData<FNavigationMetaData>();
		const TSharedPtr<FNavigationMetaData> Up =
			BannerButton->GetMetaData<FNavigationMetaData>();
		TestTrue(TEXT("Down from the last menu entry reaches the banner button"),
			Down.IsValid()
				&& Down->GetBoundaryRule(EUINavigation::Down)
					== EUINavigationRule::Explicit
				&& Down->GetFocusRecipient(EUINavigation::Down).Pin()
					== BannerButton);
		TestTrue(TEXT("Up from the banner button returns to the last menu entry"),
			Up.IsValid()
				&& Up->GetBoundaryRule(EUINavigation::Up)
					== EUINavigationRule::Explicit
				&& Up->GetFocusRecipient(EUINavigation::Up).Pin() == LastMenu);
	}

	TestTrue(TEXT("Quit click is handled"),
		Root->ActivateUpdateBannerForTesting().IsEventHandled());
	TestEqual(TEXT("Restart calls the subsystem exactly once"),
		Bootstrap->RestartRequestCountForTesting(), 1);
	TestEqual(TEXT("failed Restart reports the required error"),
		Root->ViewModelForTesting().GetStatusText(),
		FString(TEXT("WoT Reborn could not quit. Close it, then open it again to finish installing.")));
	TestTrue(TEXT("failed Restart is an error notice"),
		Root->ViewModelForTesting().IsStatusError());
	Root->SlatePrepass(1.0f);
	TArray<FString> FailedRestartText;
	CollectUpdateBannerText(
		Root->UpdateBannerWidgetForTesting().ToSharedRef(), FailedRestartText);
	TestTrue(TEXT("failed Restart renders the required error on the main menu"),
		FailedRestartText.Contains(TEXT(
			"WoT Reborn could not quit. Close it, then open it again to finish installing.")));
	TestEqual(TEXT("Quit never opens the release page"), Launches, 0);

	PublishUpdateState(*Bootstrap, EWoTUpdateStatus::Available, 13,
		TEXT("translocated"),
		TEXT("https://github.com/Open-WoT/wot/releases/tag/v13"));
	Root->SlatePrepass(1.0f);
	TestTrue(TEXT("Open release page click is handled"),
		Root->ActivateUpdateBannerForTesting().IsEventHandled());
	TestEqual(TEXT("https release page launches exactly once"), Launches, 1);
	TestEqual(TEXT("the seam receives the exact signed URL"), LaunchedUrl,
		FString(TEXT("https://github.com/Open-WoT/wot/releases/tag/v13")));

	const TCHAR* UnsafeUrls[] = {
		TEXT("https:///x"), TEXT("https://"),
		TEXT("https://example.invalid/a b"),
		TEXT("https://example.invalid/\"release"),
		TEXT("https://example.invalid/\trelease"),
		TEXT("https://example.invalid/\nrelease"),
		TEXT("https://example.invalid\\release"),
		TEXT("javascript:alert(1)"), TEXT("http://example.invalid/releases/14"),
		TEXT("https://user@example.invalid/releases/14"),
		TEXT("https://example.com/path[bad]"),
		TEXT("https://example.com/#a#b"), TEXT("https://example.com/?q"),
		TEXT("https://[::1]/x"), TEXT("https://a..b/x"),
		TEXT("https://-a.com/x"), TEXT("https://a.com:0/x"),
		TEXT("https://a.com:65536/x"), TEXT("https://a.com/%zz"),
		TEXT("https://0x7f000001/x"), TEXT("https://0x7f.0.0.1/x")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(UnsafeUrls); ++Index)
	{
		PublishUpdateState(*Bootstrap, EWoTUpdateStatus::Available,
			14 + static_cast<uint64>(Index), TEXT("reserved-live-leaf"),
			UnsafeUrls[Index]);
		Root->SlatePrepass(1.0f);
		TestTrue(FString::Printf(TEXT("unsafe URL %d hides the button"), Index),
			Root->UpdateBannerButtonForTesting()->GetVisibility()
				== EVisibility::Collapsed);
		TestFalse(FString::Printf(TEXT("unsafe URL %d has no action"), Index),
			Root->ActivateUpdateBannerForTesting().IsEventHandled());
		TestEqual(FString::Printf(TEXT("unsafe URL %d is never launched"), Index),
			Launches, 1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateBannerModalGeometrySpec,
	"WoT.FrontEnd.UpdateBanner.FocusAndGeometry", UpdateBannerSpecFlags)

bool FWoTUpdateBannerModalGeometrySpec::RunTest(const FString&)
{
	FWoTUpdateBannerMenuRowsGuard MenuRowsGuard;
	if (!TestTrue(TEXT("Slate application is initialized"),
		FSlateApplication::IsInitialized())) return false;
	FSlateApplication& SlateApp = FSlateApplication::Get();

	UWoTBootstrapSubsystem* FocusBootstrap = UpdateBannerBootstrap();
	PublishUpdateState(*FocusBootstrap, EWoTUpdateStatus::Ready, 17);
	const TSharedRef<SWoTFrontEndRoot> FocusRoot = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(UpdateBannerAuthority())
		.BootstrapForTesting(FocusBootstrap);
	const FVector2D FocusSize(1280.0f, 720.0f);
	const TSharedRef<SWindow> FocusWindow = SNew(SWindow)
		.ClientSize(FocusSize).CreateTitleBar(false)
		.SupportsMaximize(false).SupportsMinimize(false)[FocusRoot];
	SlateApp.AddWindow(FocusWindow, false);
	const auto SettleFocusLayout = [&]()
	{
		FocusRoot->Tick(FGeometry::MakeRoot(
			FocusSize, FSlateLayoutTransform()), 0.0, 0.0f);
		FocusWindow->SlatePrepass(1.0f);
		SlateApp.Tick();
	};
	SettleFocusLayout();
	const TSharedPtr<SButton> FocusBannerButton =
		FocusRoot->UpdateBannerButtonForTesting();
	const TSharedPtr<SWoTMenuButton> LastMenu =
		FocusRoot->MainMenuButtonForTesting(3);
	if (!TestTrue(TEXT("focus transition widgets exist"),
		FocusBannerButton.IsValid() && LastMenu.IsValid()))
	{
		SlateApp.RequestDestroyWindow(FocusWindow);
		SlateApp.Tick();
		return false;
	}
	struct FFocusLossCase
	{
		const TCHAR* Label;
		EWoTUpdateStatus Status;
		const TCHAR* Reason;
		const TCHAR* Url;
	};
	const FFocusLossCase FocusLossCases[] = {
		{TEXT("Ready to None"), EWoTUpdateStatus::None, nullptr, nullptr},
		{TEXT("Ready to KnownBad"), EWoTUpdateStatus::KnownBad, nullptr, nullptr},
		{TEXT("Ready to Available without URL"), EWoTUpdateStatus::Available,
			TEXT("install-root-not-writable"), TEXT("")},
	};
	for (const FFocusLossCase& Case : FocusLossCases)
	{
		PublishUpdateState(*FocusBootstrap, EWoTUpdateStatus::Ready, 17);
		SettleFocusLayout();
		SlateApp.SetKeyboardFocus(FocusBannerButton, EFocusCause::SetDirectly);
		TestTrue(FString::Printf(TEXT("%s starts on the real banner button"),
			Case.Label), SlateApp.GetKeyboardFocusedWidget() == FocusBannerButton);
		PublishUpdateState(*FocusBootstrap, Case.Status, 18,
			Case.Reason, Case.Url);
		SettleFocusLayout();
		TestTrue(FString::Printf(TEXT("%s moves focus to Quit"), Case.Label),
			SlateApp.GetKeyboardFocusedWidget() == LastMenu);
	}
	PublishUpdateState(*FocusBootstrap, EWoTUpdateStatus::Ready, 19);
	SettleFocusLayout();
	SlateApp.SetKeyboardFocus(FocusBannerButton, EFocusCause::SetDirectly);
	FocusRoot->ShowUpdateFailedNoticeForFocusTesting();
	SettleFocusLayout();
	const TSharedPtr<SWidget> ModalPrimary =
		FocusRoot->UpdateFailedContinueWidgetForTesting();
	TestTrue(TEXT("a modal moves real Slate focus to its primary button"),
		ModalPrimary.IsValid()
			&& SlateApp.GetKeyboardFocusedWidget() == ModalPrimary);
	TestTrue(TEXT("a modal notice disables the banner button"),
		!FocusBannerButton->IsEnabled());
	TestFalse(TEXT("a modal notice blocks the banner handler"),
		FocusRoot->ActivateUpdateBannerForTesting().IsEventHandled());
	TestEqual(TEXT("a modal notice cannot request Restart"),
		FocusBootstrap->RestartRequestCountForTesting(), 0);
	SlateApp.RequestDestroyWindow(FocusWindow);
	SlateApp.Tick();

	const FVector2D Sizes[] = {
		FVector2D(1280.0f, 720.0f), FVector2D(1920.0f, 1080.0f),
		FVector2D(640.0f, 480.0f)};
	for (const FVector2D Size : Sizes)
	{
		UWoTBootstrapSubsystem* Bootstrap = UpdateBannerBootstrap();
		const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
			.PlayerController(nullptr)
			.PresentationAuthorityForTesting(UpdateBannerAuthority())
			.BootstrapForTesting(Bootstrap);
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.ClientSize(Size).CreateTitleBar(false)
			.SupportsMaximize(false).SupportsMinimize(false)[Root];
		SlateApp.AddWindow(Window, false);
		const FGeometry RootGeometry = FGeometry::MakeRoot(
			Size, FSlateLayoutTransform(1.0f));
		const auto SettleLayout = [&]()
		{
			// Exercise six complete frames so a desired-size/allocation feedback
			// loop cannot hide behind an early, wider prepass.
			for (int32 Pass = 0; Pass < 6; ++Pass)
			{
				Root->Tick(RootGeometry, 0.0, 0.0f);
				Window->SlatePrepass(1.0f);
				FHittestGrid HitTestGrid;
				FSlateWindowElementList DrawElements(Window);
				const FPaintArgs PaintArgs(&Window.Get(), HitTestGrid,
					FVector2f::ZeroVector, 0.0, 0.0f);
				Root->Paint(PaintArgs, RootGeometry,
					RootGeometry.GetLayoutBoundingRect(), DrawElements, 0,
					FWidgetStyle(), true);
			}
		};
		SettleLayout();
		FSlateRect BaselineMenu[4];
		for (int32 EntryIndex = 0; EntryIndex < 4; ++EntryIndex)
		{
			BaselineMenu[EntryIndex] = Root->MainMenuButtonForTesting(EntryIndex)
				->GetCachedGeometry().GetLayoutBoundingRect();
		}
		const FString Prefix = FString::Printf(TEXT("%.0fx%.0f "), Size.X, Size.Y);
		for (int32 StateIndex = 0; StateIndex < 3; ++StateIndex)
		{
			if (StateIndex == 0)
			{
				PublishUpdateState(*Bootstrap, EWoTUpdateStatus::Ready, 18);
			}
			else if (StateIndex == 1)
			{
				PublishUpdateState(*Bootstrap, EWoTUpdateStatus::Available, 18,
					TEXT("install-root-not-writable"),
					TEXT("https://example.invalid/releases/18"));
			}
			else
			{
				PublishUpdateState(*Bootstrap, EWoTUpdateStatus::KnownBad, 18);
			}
			SettleLayout();
			const TCHAR* StateName = StateIndex == 0 ? TEXT("Ready ")
				: (StateIndex == 1 ? TEXT("Available ") : TEXT("KnownBad "));
			const FString StatePrefix = Prefix + StateName;
			FSlateRect MenuBounds;
			for (int32 EntryIndex = 0; EntryIndex < 4; ++EntryIndex)
			{
				const FSlateRect Current = Root->MainMenuButtonForTesting(EntryIndex)
					->GetCachedGeometry().GetLayoutBoundingRect();
				const FSlateRect& Baseline = BaselineMenu[EntryIndex];
				TestTrue(FString::Printf(
					TEXT("%sentry %d geometry is unchanged"), *StatePrefix, EntryIndex),
					FMath::IsNearlyEqual(Current.Left, Baseline.Left, 0.5f)
						&& FMath::IsNearlyEqual(Current.Top, Baseline.Top, 0.5f)
						&& FMath::IsNearlyEqual(Current.Right, Baseline.Right, 0.5f)
						&& FMath::IsNearlyEqual(Current.Bottom, Baseline.Bottom, 0.5f));
				if (EntryIndex == 0)
				{
					MenuBounds = Current;
				}
				else
				{
					MenuBounds.Left = FMath::Min(MenuBounds.Left, Current.Left);
					MenuBounds.Top = FMath::Min(MenuBounds.Top, Current.Top);
					MenuBounds.Right = FMath::Max(MenuBounds.Right, Current.Right);
					MenuBounds.Bottom = FMath::Max(MenuBounds.Bottom, Current.Bottom);
				}
			}
			const FSlateRect Banner = Root->UpdateBannerWidgetForTesting()
				->GetCachedGeometry().GetLayoutBoundingRect();
			const FSlateRect RootBounds =
				Root->GetCachedGeometry().GetLayoutBoundingRect();
			TestTrue(StatePrefix + TEXT("banner does not intersect the menu list"),
				Banner.Top >= MenuBounds.Bottom - 1.0f);
			TestTrue(StatePrefix + TEXT("banner remains inside the supported client"),
				Banner.Left >= RootBounds.Left - 1.0f
					&& Banner.Top >= RootBounds.Top - 1.0f
					&& Banner.Right <= RootBounds.Right + 1.0f
					&& Banner.Bottom <= RootBounds.Bottom + 1.0f);
			const float BannerWidth = Banner.Right - Banner.Left;
			const float WidthCap = FMath::Min(1200.0f, Size.X * 0.9f);
			TestTrue(FString::Printf(
				TEXT("%sbanner has the definite capped width (actual=%.2f expected=%.2f root=%.2fx%.2f)"),
				*StatePrefix, BannerWidth, WidthCap,
				RootBounds.Right - RootBounds.Left, RootBounds.Bottom - RootBounds.Top),
				FMath::IsNearlyEqual(BannerWidth, WidthCap, 1.0f));
			TestTrue(StatePrefix + TEXT("banner is horizontally centered"),
				FMath::IsNearlyEqual((Banner.Left + Banner.Right) * 0.5f,
					(RootBounds.Left + RootBounds.Right) * 0.5f, 1.0f));
			const float BottomMargin = RootBounds.Bottom - Banner.Bottom;
			const float LayoutScale = Root->GetCachedGeometry()
				.GetAccumulatedLayoutTransform().GetScale();
			const float LocalRootHeight =
				(RootBounds.Bottom - RootBounds.Top) / LayoutScale;
			const float RequestedMargin = static_cast<float>(
				static_cast<int32>(LocalRootHeight * 0.08f)) * LayoutScale;
			if (Size.X >= 1280.0f)
			{
				TestTrue(StatePrefix + TEXT("banner uses the eight-percent bottom anchor"),
					FMath::IsNearlyEqual(BottomMargin, RequestedMargin, 1.0f));
			}
			else
			{
				TestTrue(StatePrefix + TEXT("small-client margin only clamps for separation"),
					BottomMargin >= -1.0f
						&& BottomMargin <= RequestedMargin + 1.0f);
			}

			const FWoTUpdateBannerPresentation Presentation =
				Root->UpdateBannerPresentationForTesting();
			TArray<TSharedRef<STextBlock>> TextWidgets;
			CollectUpdateBannerTextWidgets(
				Root->UpdateBannerWidgetForTesting().ToSharedRef(), TextWidgets);
			FSlateRect TextBounds;
			bool bHasTextBounds = false;
			for (const TSharedRef<STextBlock>& TextWidget : TextWidgets)
			{
				const FString Text = TextWidget->GetText().ToString();
				if (Text.IsEmpty() || (Text != Presentation.PrimaryLine
					&& Text != Presentation.ReasonLine
					&& Text != Presentation.MultiplayerLine))
				{
					continue;
				}
				const FSlateRect Bounds = TextWidget->GetCachedGeometry()
					.GetLayoutBoundingRect();
				if (!bHasTextBounds)
				{
					TextBounds = Bounds;
					bHasTextBounds = true;
				}
				else
				{
					TextBounds.Left = FMath::Min(TextBounds.Left, Bounds.Left);
					TextBounds.Top = FMath::Min(TextBounds.Top, Bounds.Top);
					TextBounds.Right = FMath::Max(TextBounds.Right, Bounds.Right);
					TextBounds.Bottom = FMath::Max(TextBounds.Bottom, Bounds.Bottom);
				}
				const FVector2D SingleLine = FWoTMenuLayout::MeasureText(
					TextWidget->GetText(), TextWidget->GetFont());
				if (Size.X >= 1280.0f && Text == Presentation.PrimaryLine)
				{
					TestTrue(StatePrefix + TEXT("primary text stays on one line"),
						TextWidget->GetDesiredSize().Y <= SingleLine.Y + 1.0f);
				}
				if (Text == Presentation.ReasonLine && !Text.IsEmpty()
					&& Size.X >= 1280.0f)
				{
					const float MaximumLines = Size.X >= 1920.0f ? 1.0f : 2.0f;
					TestTrue(FString::Printf(
						TEXT("%slong reason obeys its line limit (actual=%.2f line=%.2f max=%.0f text-width=%.2f allotted=%.2f)"),
						*StatePrefix, TextWidget->GetDesiredSize().Y,
						SingleLine.Y, MaximumLines, SingleLine.X,
						Bounds.Right - Bounds.Left),
						TextWidget->GetDesiredSize().Y
							<= SingleLine.Y * MaximumLines + 1.0f);
				}
			}
			if (Presentation.Action != EWoTUpdateBannerAction::None)
			{
				const FSlateRect Button = Root->UpdateBannerButtonForTesting()
					->GetCachedGeometry().GetLayoutBoundingRect();
				TestTrue(FString::Printf(
					TEXT("%sbutton stays to the right of the text (text=[%.2f %.2f %.2f %.2f] button=[%.2f %.2f %.2f %.2f])"),
					*StatePrefix, TextBounds.Left, TextBounds.Top,
					TextBounds.Right, TextBounds.Bottom, Button.Left, Button.Top,
					Button.Right, Button.Bottom),
					bHasTextBounds && Button.Left >= TextBounds.Right - 1.0f
						&& Button.Top < TextBounds.Bottom
						&& Button.Bottom > TextBounds.Top);
			}
		}
		SlateApp.RequestDestroyWindow(Window);
		SlateApp.Tick();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMultiplayerReleaseSequenceSpec,
	"WoT.FrontEnd.UpdateBanner.MultiplayerReleaseSequence", UpdateBannerSpecFlags)

bool FWoTMultiplayerReleaseSequenceSpec::RunTest(const FString&)
{
	UWoTGameInstance* Owner = NewObject<UWoTGameInstance>(GEngine);
	UWoTSessionSubsystem* Sessions = NewObject<UWoTSessionSubsystem>(Owner);
	Sessions->SetRunningReleaseSequenceForTesting(7);
	FWoTHostRequest Request;
	Request.MapPackage = TEXT("/Game/WoT/Maps/Arena_01");
	Request.ServerName = TEXT("Sequence Host");
	const FOnlineSessionSettings Settings =
		Sessions->BuildSessionSettingsForTesting(Request);
	FString AdvertisedSequence;
	TestTrue(TEXT("host settings advertise WOT_RELSEQ"), Settings.Get(
		FName(TEXT("WOT_RELSEQ")), AdvertisedSequence));
	TestEqual(TEXT("host settings advertise the running release sequence"),
		AdvertisedSequence, FString(TEXT("7")));
	const FOnlineSessionSetting* SequenceSetting =
		Settings.Settings.Find(FName(TEXT("WOT_RELSEQ")));
	TestTrue(TEXT("WOT_RELSEQ is advertised to browser providers"),
		SequenceSetting != nullptr
			&& SequenceSetting->AdvertisementType
				== EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

	FOnlineSessionSearchResult Raw;
	Raw.Session.SessionSettings.Set(FName(TEXT("WOT_SERVER_NAME")),
		FString(TEXT("Remote")),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Raw.Session.SessionSettings.Set(FName(TEXT("WOT_NETVER")),
		FString(TEXT("different-net-version")),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Raw.Session.SessionSettings.Set(FName(TEXT("WOT_RELSEQ")),
		FString(TEXT("9")),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	FWoTSessionListEntry Parsed = Sessions->InjectSearchResultForTesting(
		nullptr, false, Raw);
	TestTrue(TEXT("search parsing preserves the mismatch"),
		Parsed.bVersionMismatch);
	TestEqual(TEXT("search parsing preserves the host release sequence"),
		Parsed.HostReleaseSequence, uint64(9));
	Raw.Session.SessionSettings.Set(FName(TEXT("WOT_RELSEQ")),
		FString(TEXT("9x")),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Parsed = Sessions->InjectSearchResultForTesting(nullptr, false, Raw);
	TestEqual(TEXT("malformed host release sequence is unknown"),
		Parsed.HostReleaseSequence, uint64(0));

	UWoTBootstrapSubsystem* Bootstrap = UpdateBannerBootstrap(7);
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(UpdateBannerAuthority())
		.BootstrapForTesting(Bootstrap);
	FWoTSessionListEntry Entry;
	Entry.ServerName = TEXT("Remote");
	Entry.bVersionMismatch = true;
	struct FTextCase
	{
		uint64 HostSequence;
		const TCHAR* Row;
		const TCHAR* Notice;
	};
	const FTextCase TextCases[] = {
		{9, TEXT("Remote (newer version)"),
			TEXT("That server is on a newer version. Update to join.")},
		{5, TEXT("Remote (older version)"),
			TEXT("That server is on an older version and cannot be joined.")},
		{7, TEXT("Remote (other version)"),
			TEXT("That server runs a different game version and cannot be joined.")},
		{0, TEXT("Remote (other version)"),
			TEXT("That server runs a different game version and cannot be joined.")},
	};
	for (const FTextCase& Case : TextCases)
	{
		Entry.HostReleaseSequence = Case.HostSequence;
		TestEqual(TEXT("mismatch row names relative release age"),
			Root->SessionServerTextForTesting(Entry), FString(Case.Row));
		TestEqual(TEXT("join refusal names relative release age"),
			Root->SessionVersionMismatchNoticeForTesting(Entry),
			FString(Case.Notice));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFakeUpdateShippingGuardSpec,
	"WoT.FrontEnd.UpdateBanner.FakeStateShippingGuard", UpdateBannerSpecFlags)

bool FWoTFakeUpdateShippingGuardSpec::RunTest(const FString&)
{
	static const TCHAR* HeaderTokens[] = {
		TEXT("WoTParseFakeUpdateState"),
		TEXT("WoTFakeUpdateStateFromCommandLine")};
	static const TCHAR* ImplementationTokens[] = {
		TEXT("WoTParseFakeUpdateState"),
		TEXT("WoTFakeUpdateStateFromCommandLine"),
		TEXT("wotfakeupdatestate=")};
	static const TCHAR* FrontEndTokens[] = {
		TEXT("WoTFakeUpdateStateFromCommandLine"), TEXT("fake-update-action")};
	TestTrue(TEXT("fake-state declarations are harness-guarded"),
		FakeUpdateTokensAreHarnessGuarded(
			TEXT("Source/WoT/UI/WoTUpdateBanner.h"), HeaderTokens));
	TestTrue(TEXT("fake-state implementation and switch are harness-guarded"),
		FakeUpdateTokensAreHarnessGuarded(
			TEXT("Source/WoT/UI/WoTUpdateBanner.cpp"), ImplementationTokens));
	TestTrue(TEXT("front-end fake override and action are harness-guarded"),
		FakeUpdateTokensAreHarnessGuarded(
			TEXT("Source/WoT/WoTFrontEndSlate.cpp"), FrontEndTokens));
	return !HasAnyErrors();
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFakeUpdateStateSpec,
	"WoT.FrontEnd.UpdateBanner.GuardedFakeState", UpdateBannerSpecFlags)

bool FWoTFakeUpdateStateSpec::RunTest(const FString&)
{
	struct FValidCase
	{
		const TCHAR* Value;
		EWoTUpdateStatus Status;
		uint64 Sequence;
		const TCHAR* Reason;
	};
	const FValidCase Valid[] = {
		{TEXT("ready:41"), EWoTUpdateStatus::Ready, 41, TEXT("")},
		{TEXT("available:42"), EWoTUpdateStatus::Available, 42, TEXT("")},
		{TEXT("available:43:translocated"), EWoTUpdateStatus::Available, 43,
			TEXT("translocated")},
		{TEXT("knownbad:44"), EWoTUpdateStatus::KnownBad, 44, TEXT("")},
	};
	for (const FValidCase& Case : Valid)
	{
		FWoTUpdateState State;
		TestTrue(FString::Printf(TEXT("valid fake parses: %s"), Case.Value),
			WoTParseFakeUpdateState(Case.Value, State));
		TestEqual(TEXT("fake status"), static_cast<int32>(State.Status),
			static_cast<int32>(Case.Status));
		TestEqual(TEXT("fake sequence"), State.ReleaseSequence, Case.Sequence);
		TestEqual(TEXT("fake reason"), State.Reason, FString(Case.Reason));
		if (Case.Status == EWoTUpdateStatus::Available)
		{
			TestTrue(TEXT("fake Available supplies an inert https page for its button"),
				WoTIsHttpsReleasePageUrl(State.ReleasePageUrl));
		}
	}
	for (const TCHAR* Invalid : {TEXT(""), TEXT("ready"), TEXT("ready:0"),
		TEXT("ready:-1"), TEXT("ready:1x"), TEXT("Ready:1"),
		TEXT("ready:1:reason"), TEXT("knownbad:1:reason"),
		TEXT("available:1:"), TEXT("available:1:reason:extra"),
		TEXT("downloading:1"), TEXT(" ready:1"), TEXT("ready:1 ")})
	{
		FWoTUpdateState State;
		TestFalse(FString::Printf(TEXT("invalid fake is refused: [%s]"), Invalid),
			WoTParseFakeUpdateState(Invalid, State));
	}

	const FString OriginalCommandLine(FCommandLine::Get());
	ON_SCOPE_EXIT { FCommandLine::Set(*OriginalCommandLine); };
	const FString Parent = FPaths::Combine(FPaths::AutomationTransientDir(),
		TEXT("UpdateBannerFake-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	IFileManager::Get().MakeDirectory(*InstallRoot, true);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};
	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	FCommandLine::Set(*(OriginalCommandLine
		+ TEXT(" -wotfakeupdatestate=ready:73")));
	UWoTBootstrapSubsystem* Bootstrap = UpdateBannerBootstrap();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(UpdateBannerAuthority())
		.BootstrapForTesting(Bootstrap);
	TestTrue(TEXT("fake Ready renders the banner"),
		Root->UpdateBannerPresentationForTesting().bVisible);
	TestTrue(TEXT("fake Ready button is handled"),
		Root->ActivateUpdateBannerForTesting().IsEventHandled());
	TestEqual(TEXT("fake Ready never calls the live restart action"),
		Bootstrap->RestartRequestCountForTesting(), 0);
	FString Heartbeat;
	TestTrue(TEXT("fake action heartbeat is readable"),
		FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
			InstallRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName())));
	TestTrue(TEXT("fake action records a heartbeat"),
		Heartbeat.Contains(TEXT("\"event\":\"fake-update-action\"")));
	return !HasAnyErrors();
}
#endif

#endif // WITH_AUTOMATION_TESTS
