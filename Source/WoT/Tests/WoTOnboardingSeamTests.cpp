#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTOnboardingSeams.h"
#include "Engine/Texture2D.h"

// The onboarding seams' contract is that they are inert when unbound: the
// plan's parity-off gate (Task 7) leans on exactly this, so a build without
// the plugin behaves like one with it disabled. These specs exercise fresh
// local instances (the struct is plain), so they stay true after later
// tasks bind handlers and register rows on the live singleton. No worlds.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingSeamFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

/** The art's own size in texels. A headless editor never builds platform
    data for a loaded asset, so there the imported source answers; a payload
    texture has no source and answers from its platform data. */
FIntPoint WoTOnboardingSeamTexelSize(const UTexture2D& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return FIntPoint(static_cast<int32>(Texture.Source.GetSizeX()),
			static_cast<int32>(Texture.Source.GetSizeY()));
	}
#endif
	return FIntPoint(Texture.GetSizeX(), Texture.GetSizeY());
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamUnboundBroadcastSpec,
	"WoT.Onboarding.SeamUnboundBroadcast", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamUnboundBroadcastSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams Seams;
	TestFalse(TEXT("no item listener"), Seams.ClientItemAdded.IsBound());
	TestFalse(TEXT("no killed-by listener"), Seams.ClientKilledBy.IsBound());
	TestFalse(TEXT("no guide listener"), Seams.ShowGuideRequested.IsBound());
	// Unbound queries read as "no retail window open"; callers gate on the
	// binding before asking.
	TestFalse(TEXT("no window-open query"),
		static_cast<bool>(Seams.IsClientWindowOpen));
	TestFalse(TEXT("no selected-item query"),
		static_cast<bool>(Seams.SelectedItemClass));
	TestFalse(TEXT("no killed-by-item query"), static_cast<bool>(Seams.KilledByItemClass));
	TestFalse(TEXT("no retail texture resolver"),
		static_cast<bool>(Seams.ResolveRetailTexture));
	TestFalse(TEXT("no item icon resolver"),
		static_cast<bool>(Seams.ResolveItemIcon));
	// Broadcasting into the void must be safe and effect-free; the hook sites
	// in the inventory component and the player controller rely on it.
	Seams.ClientItemAdded.Broadcast(FName(TEXT("EmberLob")));
	Seams.ClientKilledBy.Broadcast(FName(TEXT("EmberLob")));
	Seams.ShowGuideRequested.Broadcast();
	TestTrue(TEXT("the seam registry is one process-wide instance"),
		&FWoTOnboardingSeams::Get() == &FWoTOnboardingSeams::Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamRegistriesEmptySpec,
	"WoT.Onboarding.SeamRegistriesEmpty", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamRegistriesEmptySpec::RunTest(const FString&)
{
	FWoTOnboardingSeams Seams;
	TestEqual(TEXT("menu registry starts empty"), Seams.MenuRows.Num(), 0);
	TestEqual(TEXT("settings registry starts empty"),
		Seams.SettingsRows.Num(), 0);
	// A registered row round-trips both callbacks (consumed by Tasks 4-5).
	int32 Invocations = 0;
	Seams.MenuRows.Add({
		[]() { return FString(TEXT("Fixture Row")); },
		[&Invocations]() { ++Invocations; }});
	TestEqual(TEXT("row label resolves lazily"), Seams.MenuRows[0].Label(),
		FString(TEXT("Fixture Row")));
	Seams.MenuRows[0].Invoke();
	TestEqual(TEXT("row invoke ran once"), Invocations, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamResolverRoundTripSpec,
	"WoT.Onboarding.SeamResolverRoundTrip", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamResolverRoundTripSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams Seams;
	TestEqual(TEXT("unbound resolver answers empty"),
		Seams.ResolveText(TEXT("f"), TEXT("s"), TEXT("p")), FString());
	Seams.ResolveRetailText = [](const FString& File, const FString& Section,
		const FString& Property)
	{
		return File + TEXT("/") + Section + TEXT(".") + Property;
	};
	TestEqual(TEXT("installed resolver sees every address part"),
		Seams.ResolveText(TEXT("Pack"), TEXT("FixtureSection"),
			TEXT("Title")),
		FString(TEXT("Pack/FixtureSection.Title")));

	// The live singleton: the game module installed the retail-text wrapper
	// at startup. A section no retail file defines resolves empty through the
	// whole stack, with or without retail data present, so no retail prose is
	// pinned here.
	TestTrue(TEXT("game module installed the retail resolver"),
		static_cast<bool>(FWoTOnboardingSeams::Get().ResolveRetailText));
	TestEqual(TEXT("unknown retail section resolves empty"),
		FWoTOnboardingSeams::Get().ResolveText(TEXT("WoT"),
			TEXT("WoTOnboardingNoSuchSection"), TEXT("Title")),
		FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamArtResolversSpec,
	"WoT.Onboarding.SeamArtResolvers", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamArtResolversSpec::RunTest(const FString&)
{
	// The game module installs both art resolvers at startup. The plugin cuts
	// the frame, gem bar and dividers out of these textures by the original
	// window's regions, so their sizes are part of the contract.
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	if (!TestTrue(TEXT("retail texture resolver installed"),
			static_cast<bool>(Seams.ResolveRetailTexture))
		|| !TestTrue(TEXT("item icon resolver installed"),
			static_cast<bool>(Seams.ResolveItemIcon)))
	{
		return false;
	}
	struct FExpectedArt
	{
		const TCHAR* Name;
		int32 Width;
		int32 Height;
	};
	// Either filtering answers the same art; only its sampling differs.
	for (const bool bSmooth : {false, true})
	{
		const TCHAR* Filtering = bSmooth ? TEXT("smooth") : TEXT("crisp");
		for (const FExpectedArt& Art : {FExpectedArt{TEXT("InfoBoxC1"), 128, 128},
			FExpectedArt{TEXT("InfoBoxC2"), 128, 256},
			FExpectedArt{TEXT("IconMask"), 64, 64},
			FExpectedArt{TEXT("StatusIconFrame"), 64, 64}})
		{
			const UTexture2D* Texture =
				Seams.ResolveRetailTexture(FName(Art.Name), bSmooth);
			if (TestNotNull(FString::Printf(TEXT("%s %s resolves"), Filtering,
					Art.Name), Texture))
			{
				const FIntPoint Size = WoTOnboardingSeamTexelSize(*Texture);
				TestEqual(FString::Printf(TEXT("%s %s width"), Filtering, Art.Name),
					Size.X, Art.Width);
				TestEqual(FString::Printf(TEXT("%s %s height"), Filtering, Art.Name),
					Size.Y, Art.Height);
			}
		}
		TestNull(FString::Printf(TEXT("%s: an unnamed texture resolves nothing"),
			Filtering),
			Seams.ResolveRetailTexture(FName(TEXT("WoTOnboardingNoSuchArt")), bSmooth));
		const UTexture2D* Icon =
			Seams.ResolveItemIcon(FName(TEXT("AngrealInvFireball")), bSmooth);
		if (TestNotNull(FString::Printf(TEXT("%s: an item class's StatusIcon resolves"),
				Filtering), Icon))
		{
			const FIntPoint IconSize = WoTOnboardingSeamTexelSize(*Icon);
			TestEqual(FString::Printf(TEXT("%s item icon width"), Filtering),
				IconSize.X, 64);
			TestEqual(FString::Printf(TEXT("%s item icon height"), Filtering),
				IconSize.Y, 64);
		}
		TestNull(FString::Printf(TEXT("%s: an unknown class has no icon"), Filtering),
			Seams.ResolveItemIcon(FName(TEXT("WoTOnboardingNoSuchItem")), bSmooth));
		TestNull(FString::Printf(TEXT("%s: no class has no icon"), Filtering),
			Seams.ResolveItemIcon(NAME_None, bSmooth));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
