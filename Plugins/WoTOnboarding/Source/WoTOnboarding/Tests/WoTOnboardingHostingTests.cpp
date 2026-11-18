#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTPickupCard.h"
#include "WoTOnboardingPickupCards.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTGameViewportClient.h"
#include "WoTLocalPlayerHooks.h"
#include "WoTPlayerController.h"

#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SVirtualWindow.h"

// H6: the first-pickup card, added to the game viewport exactly as the module
// hosts it (a passive overlay at its own z-order), follows a local player's
// UI host there and back. Synthetic, invented card text.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHostedCardSpec,
	"WoT.VR.Hooks.H6.OnboardingCard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTOnboardingHostedCardSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this))
	{
		return false;
	}
	UWoTGameViewportClient* Viewport = Cast<UWoTGameViewportClient>(F.Viewport);
	if (!TestNotNull(TEXT("the game's own viewport client"), Viewport))
	{
		return false;
	}
	const TSharedRef<SVirtualWindow> Window = SNew(SVirtualWindow).Size(FVector2D(1024.0f, 768.0f));
	const TSharedRef<SOverlay> Surface = SNew(SOverlay);
	Window->SetContent(Surface);
	FSlateApplication::Get().RegisterVirtualWindow(Window);
	ON_SCOPE_EXIT
	{
		FWoTLocalPlayerHooks::DetachAll(*F.Player);
		FSlateApplication::Get().UnregisterVirtualWindow(Window);
	};
	TArray<EWoTUIContent> Kinds;
	FWoTUIHost UI;
	UI.Show = [&Surface, &Kinds](const TSharedRef<SWidget>& Screen, int32 ZOrder, EWoTUIContent Kind)
	{
		Surface->AddSlot(ZOrder)[Screen];
		Kinds.Add(Kind);
		return true;
	};
	UI.Hide = [&Surface](const TSharedRef<SWidget>& Screen)
	{
		Surface->RemoveSlot(Screen);
	};
	UI.Scope = [&Window]() -> TSharedPtr<SWidget> { return Window; };
	FWoTLocalPlayerConsumer Host;
	Host.UIHost = UI;

	FWoTPickupCardText Text;
	Text.ItemClass = FName(TEXT("EmberLob"));
	Text.Title = TEXT("Ember Lob");
	Text.Summary = TEXT("Throws a small ember.");
	const TSharedRef<SWoTPickupCard> Card = SNew(SWoTPickupCard).Text(Text).Lifetime(4.0f);
	auto OnViewport = [Viewport, &Card]()
	{
		return Viewport->GetOverlayContent().ContainsByPredicate(
			[&Card](const TPair<TSharedRef<SWidget>, int32>& Entry)
			{
				return &Entry.Key.Get() == &Card.Get() && Entry.Value == 1850;
			});
	};
	auto Hosted = [&Window, &Card]()
	{
		return FSlateApplication::Get().FindWidgetWindow(Card) == Window;
	};

	// Without a host the card is the viewport's, as always.
	Viewport->AddViewportWidgetContent(Card, 1850);
	TestTrue(TEXT("without a host the card is on the viewport"), OnViewport() && !Hosted());
	Viewport->RemoveViewportWidgetContent(Card);

	// With one, the card the module adds goes to the host, as an optional
	// module's screen, and takes no input: play carries on.
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(*F.Player,
		TEXT("CardHost"), Host)))
	{
		return false;
	}
	Viewport->AddViewportWidgetContent(Card, 1850);
	TestTrue(TEXT("the card opens in the host"), Hosted() && !OnViewport());
	TestTrue(TEXT("the card is an optional module's screen"),
		Kinds.Num() == 1 && Kinds[0] == EWoTUIContent::Extension);
	TestFalse(TEXT("a passive card leaves gameplay input alone"),
		FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));
	// What a host can tell a passive card by (the VR host shows such a screen
	// beside its HUD and never gives it input): no focus, never hit.
	TestFalse(TEXT("the card takes no focus"), Card->SupportsKeyboardFocus());
	TestTrue(TEXT("the card is never hit"), Card->GetVisibility() == EVisibility::HitTestInvisible);

	// Detaching returns it to the viewport at its z-order.
	FWoTLocalPlayerHooks::Detach(*F.Player, TEXT("CardHost"));
	TestTrue(TEXT("the card returns to the viewport"), OnViewport() && !Hosted());
	Viewport->RemoveViewportWidgetContent(Card);
	TestFalse(TEXT("the card closes as before"), OnViewport());
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
