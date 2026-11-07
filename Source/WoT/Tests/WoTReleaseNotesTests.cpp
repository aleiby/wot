#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Online/WoTLocalNetworkAccess.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../UI/WoTReleaseNotes.h"
#include "../Updater/WoTReleaseIdentity.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTMenuStyle.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTPreferencesSubsystem.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Text/STextBlock.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"

namespace
{
	constexpr EAutomationTestFlags ReleaseNotesSpecFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FString ReleaseNotesSpecDirectory(const TCHAR* Prefix)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
			FString(Prefix) + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> ReleaseNotesEarlyUI()
	{
		TArray<uint8> Bytes;
		const ANSICHAR* Text = "Schema=EarlyUI-v1\n";
		Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		return ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
			FWoTInventoryLocaleSet::Production());
	}

	bool ActivateReleaseNotesPreferences(UWoTPreferencesSubsystem& Preferences,
		FAutomationTestBase& Test)
	{
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ReleaseNotesEarlyUI();
		if (Parsed.IsError())
		{
			Test.AddError(Parsed.GetError().ToString());
			return false;
		}
		if (Preferences.AdoptAuthenticatedEarlyUI(
			MoveTemp(Parsed).GetValue()).IsError())
		{
			Test.AddError(TEXT("release-note fixture could not adopt EarlyUI"));
			return false;
		}
		if (PublishWoTPayloadBootstrapActivationForTesting().IsError()
			|| Preferences.ApplyFullPreferencesAfterCacheActivation().IsError())
		{
			Test.AddError(TEXT("release-note fixture could not activate preferences"));
			return false;
		}
		return true;
	}

	class FReleaseNotesFeedback final : public IWoTFrontEndFeedbackSink
	{
	public:
		virtual bool IsReady() const override { return true; }
		virtual void MoveSelector() override {}
		virtual void Select() override {}
	};

	TSharedRef<FWoTFrontEndPresentationAuthority> ReleaseNotesAuthority()
	{
		TMap<FName, FString> Labels;
		Labels.Add(FName(TEXT("menuMain.MenuList[2]")), TEXT("Multiplayer"));
		Labels.Add(FName(TEXT("menuMain.MenuList[3]")), TEXT("Controls"));
		Labels.Add(FName(TEXT("menuMain.MenuList[4]")), TEXT("Hardware"));
		Labels.Add(FName(TEXT("menuMain.MenuList[7]")), TEXT("Quit"));
		return FWoTFrontEndPresentationAuthority::CreateForTesting(
			TEXT("release-notes-fixture"),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
			MoveTemp(Labels), MakeShared<FReleaseNotesFeedback>());
	}

	void CollectReleaseNotesText(const TSharedRef<SWidget>& Widget,
		TArray<FString>& OutText)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			OutText.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				CollectReleaseNotesText(Children->GetChildAt(Index), OutText);
			}
		}
	}

	TArray<uint8> ReleaseNotesBytes(const FString& Text)
	{
		FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Bytes;
	}

	bool SelectReleaseNotes(const FString& Json, uint64 Sequence,
		FString& OutText, FAutomationTestBase& Test)
	{
		TWoTBootstrapResult<bool> Selected = WoTReleaseNotes::ParseAndSelect(
			ReleaseNotesBytes(Json), Sequence, OutText);
		if (Selected.IsError())
		{
			Test.AddError(Selected.GetError().ToString());
			return false;
		}
		return Selected.GetValue();
	}

	FWoTReleaseNotesResult ReleaseNotesResult(EWoTReleaseNotesOutcome Outcome,
		const TCHAR* Commit, const TCHAR* Text = TEXT(""))
	{
		FWoTReleaseNotesResult Result;
		Result.Outcome = Outcome;
		Result.ReleaseCommit = Commit;
		Result.ReleaseSequence = 7;
		Result.Text = Text;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseNotesParseAndSelectSpec,
	"WoT.UI.ReleaseNotes.ParseAndSelect", ReleaseNotesSpecFlags)

bool FWoTReleaseNotesParseAndSelectSpec::RunTest(const FString&)
{
	const FString Valid = TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"first\"},{\"release_sequence\":2,\"text\":\"second\\nline\"}]}");
	FString Text;
	TestTrue(TEXT("selection hit"), SelectReleaseNotes(Valid, 2, Text, *this));
	TestEqual(TEXT("selection returns exact text"), Text,
		FString(TEXT("second\nline")));
	const FString BlankLines = TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"one\\n\\ntwo\"}]}");
	TestTrue(TEXT("blank lines are normalized text"),
		SelectReleaseNotes(BlankLines, 1, Text, *this));
	TestEqual(TEXT("blank lines are preserved"), Text,
		FString(TEXT("one\n\ntwo")));
	Text = TEXT("sentinel");
	TestFalse(TEXT("selection miss"), SelectReleaseNotes(Valid, 3, Text, *this));
	TestTrue(TEXT("selection miss clears text"), Text.IsEmpty());

	auto Refused = [this](const FString& Json, const TCHAR* What)
	{
		FString Ignored(TEXT("sentinel"));
		const TWoTBootstrapResult<bool> Result = WoTReleaseNotes::ParseAndSelect(
			ReleaseNotesBytes(Json), 1, Ignored);
		TestTrue(What, Result.IsError());
		TestTrue(FString(What) + TEXT(" clears text"), Ignored.IsEmpty());
	};
	Refused(TEXT("{\"format\":\"wrong\",\"notes\":[]}"),
		TEXT("wrong format is refused"));
	Refused(TEXT("{\"extra\":1,\"format\":\"wot-release-notes-v1\",\"notes\":[]}"),
		TEXT("extra root key is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"extra\":1,\"release_sequence\":1,\"text\":\"x\"}]}"),
		TEXT("extra entry key is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"a\"},{\"release_sequence\":1,\"text\":\"b\"}]}"),
		TEXT("duplicate sequence is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":2,\"text\":\"b\"},{\"release_sequence\":1,\"text\":\"a\"}]}"),
		TEXT("unsorted sequences are refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":0,\"text\":\"x\"}]}"),
		TEXT("sequence zero is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"\"}]}"),
		TEXT("empty text is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"bad\\rtext\"}]}"),
		TEXT("CR is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"bad \"}]}"),
		TEXT("trailing whitespace is refused"));
	Refused(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"bad\\f\\nline\"}]}"),
		TEXT("non-space trailing whitespace is refused"));
	Refused(FString::Printf(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"bad%c\\nline\"}]}"),
		static_cast<TCHAR>(0x00a0)),
		TEXT("Unicode trailing whitespace is refused"));
	Refused(FString::Printf(TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"%s\"}]}"),
		*FString::ChrN(16 * 1024 + 1, TEXT('x'))),
		TEXT("oversize entry is refused"));

	FString TooMany = TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[");
	for (int32 Index = 1; Index <= 65; ++Index)
	{
		if (Index > 1) TooMany += TEXT(",");
		TooMany += FString::Printf(
			TEXT("{\"release_sequence\":%d,\"text\":\"x\"}"), Index);
	}
	TooMany += TEXT("]}");
	Refused(TooMany, TEXT("more than 64 entries are refused"));

	TArray<uint8> OversizeFile;
	OversizeFile.Init(static_cast<uint8>('x'), 256 * 1024 + 1);
	Text = TEXT("sentinel");
	TestTrue(TEXT("oversize file is refused"),
		WoTReleaseNotes::ParseAndSelect(OversizeFile, 1, Text).IsError());
	TestTrue(TEXT("oversize file clears text"), Text.IsEmpty());

	TArray<uint8> InvalidUtf8 = ReleaseNotesBytes(
		TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[]}"));
	InvalidUtf8.Insert(static_cast<uint8>(0xff), 1);
	TestTrue(TEXT("invalid UTF-8 is refused"),
		WoTReleaseNotes::ParseAndSelect(InvalidUtf8, 1, Text).IsError());
	Refused(TEXT("{ \"format\":\"wot-release-notes-v1\",\"notes\":[]}"),
		TEXT("non-canonical JSON is refused"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseNotesMarkerSpec,
	"WoT.FrontEnd.ReleaseNotes.Marker", ReleaseNotesSpecFlags)

bool FWoTReleaseNotesMarkerSpec::RunTest(const FString&)
{
	const FString Directory = ReleaseNotesSpecDirectory(TEXT("ReleaseNotesMarker-"));
	const FString Leaf = Directory / TEXT("WoTUserPreferences");
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ResetWoTPayloadActivationForTesting();
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	UWoTGameInstance* Owner = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Preferences =
		NewObject<UWoTPreferencesSubsystem>(Owner);
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ReleaseNotesEarlyUI();
	if (Parsed.IsError())
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("adopt EarlyUI"), Preferences->AdoptAuthenticatedEarlyUI(
		MoveTemp(Parsed).GetValue()).IsError());
	TestFalse(TEXT("dormant preferences report unseen"),
		Preferences->HasSeenReleaseNotes(TEXT("commit-a")));
	TestTrue(TEXT("dormant preferences refuse a marker"),
		Preferences->SetReleaseNotesSeen(TEXT("commit-a")).IsError());
	TestTrue(TEXT("an empty release key is refused"),
		Preferences->SetReleaseNotesSeen(FString()).IsError());

	TestFalse(TEXT("activate"),
		PublishWoTPayloadBootstrapActivationForTesting().IsError());
	TestFalse(TEXT("load full preferences"),
		Preferences->ApplyFullPreferencesAfterCacheActivation().IsError());
	TestFalse(TEXT("fresh profile is unseen"),
		Preferences->HasSeenReleaseNotes(TEXT("commit-a")));
	TestFalse(TEXT("record release"),
		Preferences->SetReleaseNotesSeen(TEXT("commit-a")).IsError());
	TestTrue(TEXT("recorded release is seen"),
		Preferences->HasSeenReleaseNotes(TEXT("commit-a")));
	TestFalse(TEXT("a prefix is not mistaken for the recorded release"),
		Preferences->HasSeenReleaseNotes(TEXT("commit")));
	TestFalse(TEXT("a different release remains unseen"),
		Preferences->HasSeenReleaseNotes(TEXT("commit-b")));
	TestFalse(TEXT("empty key is never seen"),
		Preferences->HasSeenReleaseNotes(FString()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseNotesFrontEndSpec,
	"WoT.FrontEnd.ReleaseNotes.Notice", ReleaseNotesSpecFlags)

bool FWoTReleaseNotesFrontEndSpec::RunTest(const FString&)
{
	// On the Mac, entering Play would raise the Local Network heads-up, which
	// replaces the online disclosure this spec stacks against.
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	const FString Directory = ReleaseNotesSpecDirectory(TEXT("ReleaseNotesFrontEnd-"));
	const FString Leaf = Directory / TEXT("WoTUserPreferences");
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ResetWoTPayloadActivationForTesting();
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	UWoTGameInstance* Owner = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Preferences =
		NewObject<UWoTPreferencesSubsystem>(Owner);
	if (!ActivateReleaseNotesPreferences(*Preferences, *this)) return false;

	UWoTBootstrapSubsystem* BeforeBootstrap =
		NewObject<UWoTBootstrapSubsystem>(Owner);
	BeforeBootstrap->DispatchReleaseNotesForTesting(ReleaseNotesResult(
		EWoTReleaseNotesOutcome::Selected, TEXT("commit-before"),
		TEXT("- A visible change.")));
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences)
		.BootstrapForTesting(BeforeBootstrap);
	TestTrue(TEXT("a completion before Construct opens on main-menu entry"),
		Root->IsReleaseNotesNoticeVisibleForTesting());
	TestTrue(TEXT("the shared notice widget exists"),
		Root->ReleaseNotesNoticeWidgetForTesting().IsValid());
	TArray<FString> NoticeText;
	CollectReleaseNotesText(
		Root->ReleaseNotesNoticeWidgetForTesting().ToSharedRef(), NoticeText);
	TestTrue(TEXT("the notice shows its heading"),
		NoticeText.Contains(TEXT("What's new in this version")));
	TestTrue(TEXT("the notice shows the loaded notes body"),
		NoticeText.Contains(TEXT("- A visible change.")));
	TestTrue(TEXT("the Continue button owns initial keyboard focus"),
		Root->ReleaseNotesContinueWidgetForTesting().IsValid()
		&& Root->InitialFocusWidget().Get()
			== Root->ReleaseNotesContinueWidgetForTesting().Get());
	TestTrue(TEXT("Continue is handled"),
		Root->ContinueReleaseNotesForTesting().IsEventHandled());
	TestFalse(TEXT("Continue hides the notice"),
		Root->IsReleaseNotesNoticeVisibleForTesting());
	TestTrue(TEXT("Continue restores the selected main-menu focus target"),
		Root->MainMenuButtonForTesting(0).IsValid()
		&& Root->InitialFocusWidget().Get()
			== Root->MainMenuButtonForTesting(0).Get());
	TestTrue(TEXT("Continue writes the release marker"),
		Preferences->HasSeenReleaseNotes(TEXT("commit-before")));

	UWoTBootstrapSubsystem* AfterBootstrap =
		NewObject<UWoTBootstrapSubsystem>(Owner);
	const TSharedRef<SWoTFrontEndRoot> After = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences)
		.BootstrapForTesting(AfterBootstrap);
	TestFalse(TEXT("no completion means no notice after Construct"),
		After->IsReleaseNotesNoticeVisibleForTesting());
	AfterBootstrap->DispatchReleaseNotesForTesting(ReleaseNotesResult(
		EWoTReleaseNotesOutcome::Selected, TEXT("commit-after"),
		TEXT("after construct")));
	TestTrue(TEXT("a completion after Construct opens the notice"),
		After->IsReleaseNotesNoticeVisibleForTesting());

	UWoTBootstrapSubsystem* PlayBootstrap =
		NewObject<UWoTBootstrapSubsystem>(Owner);
	const TSharedRef<SWoTFrontEndRoot> DuringPlay = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences)
		.BootstrapForTesting(PlayBootstrap);
	DuringPlay->ActivateMainMenuEntryForTesting(0);
	PlayBootstrap->DispatchReleaseNotesForTesting(ReleaseNotesResult(
		EWoTReleaseNotesOutcome::Selected, TEXT("commit-play"),
		TEXT("arrived during play")));
	TestFalse(TEXT("arrival while Play is up does not show"),
		DuringPlay->IsReleaseNotesNoticeVisibleForTesting());
	DuringPlay->BackFromPlayForTesting();
	TestTrue(TEXT("return to idle Main shows the retained result"),
		DuringPlay->IsReleaseNotesNoticeVisibleForTesting());

	UWoTBootstrapSubsystem* StackedBootstrap =
		NewObject<UWoTBootstrapSubsystem>(Owner);
	StackedBootstrap->DispatchReleaseNotesForTesting(ReleaseNotesResult(
		EWoTReleaseNotesOutcome::Selected, TEXT("commit-stacked"),
		TEXT("waited behind another notice")));
	const TSharedRef<SWoTFrontEndRoot> Stacked = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences)
		.BootstrapForTesting(StackedBootstrap)
		.ShowOnlineDisclosureForTesting(true);
	TestFalse(TEXT("release notes do not stack with another notice"),
		Stacked->IsReleaseNotesNoticeVisibleForTesting());
	Stacked->CancelOnlineDisclosureForTesting();
	TestTrue(TEXT("release notes show after the competing notice is dismissed"),
		Stacked->IsReleaseNotesNoticeVisibleForTesting());

	struct FRefusedOutcome
	{
		EWoTReleaseNotesOutcome Outcome;
		const TCHAR* Commit;
	};
	const FRefusedOutcome Refused[] = {
		{EWoTReleaseNotesOutcome::Invalid, TEXT("commit-invalid")},
		{EWoTReleaseNotesOutcome::Unavailable, TEXT("commit-unavailable")},
		{EWoTReleaseNotesOutcome::NoEntry, TEXT("commit-no-entry")},
	};
	for (const FRefusedOutcome& Case : Refused)
	{
		UWoTBootstrapSubsystem* RefusedBootstrap =
			NewObject<UWoTBootstrapSubsystem>(Owner);
		RefusedBootstrap->DispatchReleaseNotesForTesting(
			ReleaseNotesResult(Case.Outcome, Case.Commit));
		const TSharedRef<SWoTFrontEndRoot> RefusedRoot = SNew(SWoTFrontEndRoot)
			.PlayerController(nullptr)
			.PresentationAuthorityForTesting(ReleaseNotesAuthority())
			.PreferencesForTesting(Preferences)
			.BootstrapForTesting(RefusedBootstrap);
		TestFalse(FString::Printf(TEXT("outcome %s never shows"),
			LexToString(Case.Outcome)),
			RefusedRoot->IsReleaseNotesNoticeVisibleForTesting());
		TestFalse(FString::Printf(TEXT("outcome %s is never marked seen"),
			LexToString(Case.Outcome)),
			Preferences->HasSeenReleaseNotes(Case.Commit));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateFailedFrontEndSpec,
	"WoT.FrontEnd.UpdateFailed.Notice", ReleaseNotesSpecFlags)

bool FWoTUpdateFailedFrontEndSpec::RunTest(const FString&)
{
	const FString Directory = ReleaseNotesSpecDirectory(TEXT("UpdateFailed-"));
	const FString Leaf = Directory / TEXT("WoTUserPreferences");
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ResetWoTPayloadActivationForTesting();
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::
			SetUpdateFailedNoticePersistenceFailureForTesting(false);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	UWoTGameInstance* Owner = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Preferences =
		NewObject<UWoTPreferencesSubsystem>(Owner);
	if (!ActivateReleaseNotesPreferences(*Preferences, *this)) return false;
	UWoTBootstrapSubsystem* Bootstrap =
		NewObject<UWoTBootstrapSubsystem>(Owner);
	Bootstrap->DispatchReleaseNotesForTesting(ReleaseNotesResult(
		EWoTReleaseNotesOutcome::Selected, TEXT("current-release"),
		TEXT("current release notes")));

	FWoTUpdaterStateRoot NoticeRoot;
	NoticeRoot.UserStateRoot = Directory;
	FWoTBootstrapStatus Recorded = RecordUpdateFailedNotice(NoticeRoot, 7, 5);
	if (!TestFalse(TEXT("rollback records its one-time notice"), Recorded.IsError())
		&& Recorded.IsError()) AddError(Recorded.GetError().ToString());
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences)
		.BootstrapForTesting(Bootstrap);
	TestTrue(TEXT("the update-failed notice is shown first"),
		Root->IsUpdateFailedNoticeVisibleForTesting());
	TestFalse(TEXT("update-failed and release-note notices never stack"),
		Root->IsReleaseNotesNoticeVisibleForTesting());
	TestTrue(TEXT("the update-failed notice widget exists"),
		Root->UpdateFailedNoticeWidgetForTesting().IsValid());
	TArray<FString> Text;
	if (Root->UpdateFailedNoticeWidgetForTesting().IsValid())
	{
		CollectReleaseNotesText(
			Root->UpdateFailedNoticeWidgetForTesting().ToSharedRef(), Text);
	}
	TestTrue(TEXT("the notice names failed N and restored M exactly"),
		Text.Contains(TEXT("The update to version 7 could not start on this computer, so you are back on version 5.")));
	UWoTPreferencesSubsystem::
		SetUpdateFailedNoticePersistenceFailureForTesting(true);
	TestTrue(TEXT("the update-failed Continue action is handled"),
		Root->ContinueUpdateFailedForTesting().IsEventHandled());
	TestFalse(TEXT("the notice closes after acknowledgement"),
		Root->IsUpdateFailedNoticeVisibleForTesting());
	uint64 PendingFailed = 0;
	uint64 PendingRestored = 0;
	TestTrue(TEXT("the forced persistence failure leaves the durable notice pending"),
		Preferences->GetPendingUpdateFailedNotice(
			PendingFailed, PendingRestored));
	TestFalse(TEXT("Continue dismisses the failed sequence for this session"),
		Root->IsUpdateFailedNoticeVisibleForTesting());
	UWoTPreferencesSubsystem::
		SetUpdateFailedNoticePersistenceFailureForTesting(false);
	TestTrue(TEXT("release notes follow after the higher-priority notice"),
		Root->IsReleaseNotesNoticeVisibleForTesting());
	Root->ContinueReleaseNotesForTesting();

	const TSharedRef<SWoTFrontEndRoot> SameFailure = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences);
	TestTrue(TEXT("a persistence failure permits the notice next session"),
		SameFailure->IsUpdateFailedNoticeVisibleForTesting());
	SameFailure->ContinueUpdateFailedForTesting();

	Recorded =
		UWoTPreferencesSubsystem::SetUpdateFailedNoticePendingForTesting(8, 5);
	if (Recorded.IsError()) return false;
	const TSharedRef<SWoTFrontEndRoot> LaterFailure = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences);
	TestTrue(TEXT("a later failed sequence gets its own notice"),
		LaterFailure->IsUpdateFailedNoticeVisibleForTesting());
	TestFalse(TEXT("the later failure still does not stack notices"),
		LaterFailure->IsReleaseNotesNoticeVisibleForTesting());
	LaterFailure->ContinueUpdateFailedForTesting();

	// Every release candidate so far is unsequenced (0), so the first real
	// update can roll back to version 0. The notice must still be recorded and
	// shown, naming no version number (the writer once refused 0).
	Recorded = RecordUpdateFailedNotice(NoticeRoot, 9, 0);
	if (!TestFalse(TEXT("a rollback to an unsequenced build records its notice"),
		Recorded.IsError())) return false;
	TestFalse(TEXT("the unsequenced-restore notice verifies"),
		VerifyUpdateFailedNotice(NoticeRoot, 9, 0).IsError());
	const TSharedRef<SWoTFrontEndRoot> UnsequencedFailure = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences);
	TestTrue(TEXT("a rollback to an unsequenced build shows its notice"),
		UnsequencedFailure->IsUpdateFailedNoticeVisibleForTesting());
	TArray<FString> UnsequencedText;
	if (UnsequencedFailure->UpdateFailedNoticeWidgetForTesting().IsValid())
	{
		CollectReleaseNotesText(
			UnsequencedFailure->UpdateFailedNoticeWidgetForTesting().ToSharedRef(),
			UnsequencedText);
	}
	TestTrue(TEXT("an unsequenced restore reads as the previous version"),
		UnsequencedText.Contains(TEXT("The update to version 9 could not start on this computer, so you are back on the previous version.")));
	UnsequencedFailure->ContinueUpdateFailedForTesting();

	// A versioned running build names its own human version, the one the menu
	// corner shows, rather than a sequence number for a release never run here.
	const FString IdentityPath = Directory / TEXT("release-identity.json");
	TestTrue(TEXT("the versioned running identity writes"), FFileHelper::SaveStringToFile(
		TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"abc123456\",\"release_sequence\":5,\"version\":1}"),
		*IdentityPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(TEXT("the running release version writes"), FFileHelper::SaveStringToFile(
		TEXT("{\"format\":\"wot-release-version-v1\",\"human_version\":\"0.1.2\",\"version\":1}"),
		*(Directory / TEXT("release-version.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	WoTReleaseIdentity::FScopedPathOverrideForTesting IdentityOverride(IdentityPath);
	Recorded = RecordUpdateFailedNotice(NoticeRoot, 10, 5);
	if (!TestFalse(TEXT("a versioned rollback records its notice"), Recorded.IsError()))
	{
		return false;
	}
	const TSharedRef<SWoTFrontEndRoot> VersionedFailure = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(ReleaseNotesAuthority())
		.PreferencesForTesting(Preferences);
	TArray<FString> VersionedText;
	if (VersionedFailure->UpdateFailedNoticeWidgetForTesting().IsValid())
	{
		CollectReleaseNotesText(
			VersionedFailure->UpdateFailedNoticeWidgetForTesting().ToSharedRef(),
			VersionedText);
	}
	TestTrue(TEXT("a versioned restore names its human version"),
		VersionedText.Contains(TEXT("The update could not start on this computer, so you are back on version 0.1.2.")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateFailedNoticeDirectorySpec,
	"WoT.FrontEnd.UpdateFailed.NoticeDirectory", ReleaseNotesSpecFlags)

bool FWoTUpdateFailedNoticeDirectorySpec::RunTest(const FString&)
{
	const FString GenericUpdaterRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("WoT Root With Space"));
	const FString CanonicalMacPreferences = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("canonical-mac-preferences"));
	const FString ExpectedPreferences = FPaths::Combine(
		GenericUpdaterRoot, TEXT("Preferences"));
	FString MacPolicy;
	TestTrue(TEXT("the Mac notice policy resolves R/Preferences"),
		WoTResolveUpdateFailureNoticeDirectoryForTesting(
			GenericUpdaterRoot, CanonicalMacPreferences, true, MacPolicy));
	TestEqual(TEXT("the Mac notice policy uses the shared preferences leaf"),
		MacPolicy, ExpectedPreferences);

	FString WindowsPolicy;
	TestTrue(TEXT("the Windows notice policy resolves R/Preferences"),
		WoTResolveUpdateFailureNoticeDirectoryForTesting(
			GenericUpdaterRoot, CanonicalMacPreferences, false, WindowsPolicy));
	TestEqual(TEXT("the Windows notice policy uses the shared preferences leaf"),
		WindowsPolicy, ExpectedPreferences);

#if PLATFORM_MAC
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FPaths::Combine(
		ExpectedPreferences, TEXT("WoTUserPreferences")));
	WoTSetUpdateFailureNoticeDirectoryForTesting(FString());
	FString ProducerDirectory;
	FString UiMarker;
	// Policy check only: both sides resolve the real directory; nothing is
	// written to it.
	WoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting(true);
	const bool bProducerResolved = WoTResolveUpdateFailureNoticeDirectory(
		GenericUpdaterRoot, ProducerDirectory);
	const bool bUiResolved = UWoTPreferencesSubsystem::SideFilePath(
		TEXT("WoTUpdateFailedNotice"), UiMarker);
	WoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting(false);
	TestTrue(TEXT("the separate updater producer resolves its directory"),
		bProducerResolved);
	TestTrue(TEXT("the UI independently resolves its marker"), bUiResolved);
	TestEqual(TEXT("separate producer and UI resolutions agree on Mac"),
		ProducerDirectory, FPaths::GetPath(UiMarker));
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
#endif
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
