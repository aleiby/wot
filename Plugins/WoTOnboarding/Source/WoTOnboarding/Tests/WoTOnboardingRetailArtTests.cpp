#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTRetailArt.h"
#include "WoTOnboardingSeams.h"

#include "Engine/Texture2D.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

// The original window art the onboarding widgets paint (plan Task 8): which
// pieces draw it and which fall back to flat panels, the icon cache, and the
// gem bar's geometry. The art comes through injected seams over transient
// textures, never the player's files. Helper names carry the
// WoTOnboardingRetailArt prefix for the shared unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingRetailArtFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

/** Saves the art seams and puts them back. */
struct FWoTOnboardingRetailArtSeamScope
{
	TFunction<UTexture2D*(FName, bool)> SavedTexture;
	TFunction<UTexture2D*(FName, bool)> SavedIcon;

	FWoTOnboardingRetailArtSeamScope()
	{
		const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
		SavedTexture = Seams.ResolveRetailTexture;
		SavedIcon = Seams.ResolveItemIcon;
	}

	~FWoTOnboardingRetailArtSeamScope()
	{
		FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
		Seams.ResolveRetailTexture = SavedTexture;
		Seams.ResolveItemIcon = SavedIcon;
	}
};

bool WoTOnboardingRetailArtDraws(const TSharedPtr<FWoTRetailArt>& Art,
	EWoTRetailArtPiece Piece)
{
	const TSharedRef<SWoTRetailArt> Widget =
		SNew(SWoTRetailArt).Piece(Piece).Art(Art);
	return Widget->DrawsRetailArt();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtPiecesSpec,
	"WoT.Onboarding.RetailArtPieces", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtPiecesSpec::RunTest(const FString&)
{
	const FWoTOnboardingRetailArtSeamScope Scope;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const EWoTRetailArtPiece Pieces[] = {EWoTRetailArtPiece::Window,
		EWoTRetailArtPiece::Divider, EWoTRetailArtPiece::GemBar,
		EWoTRetailArtPiece::Band};

	// No art seam (a build without the game module's art): flat panels.
	Seams.ResolveRetailTexture = nullptr;
	Seams.ResolveItemIcon = nullptr;
	const TSharedRef<FWoTRetailArt> None = FWoTRetailArt::Resolve();
	for (const EWoTRetailArtPiece Piece : Pieces)
	{
		TestFalse(TEXT("without the art every piece draws flat"),
			WoTOnboardingRetailArtDraws(None, Piece));
		TestFalse(TEXT("a null art draws flat too"),
			WoTOnboardingRetailArtDraws(nullptr, Piece));
	}
	TestNull(TEXT("no bevel without the art"), None->IconMask());
	TestNull(TEXT("no selection frame without the art"), None->SelectionFrame());
	TestNull(TEXT("no icon without the seam"),
		None->ItemIcon(FName(TEXT("EmberLob")), false));

	// The art resolves: every piece draws it.
	const TStrongObjectPtr<UTexture2D> Corner(UTexture2D::CreateTransient(128, 128));
	const TStrongObjectPtr<UTexture2D> Other(UTexture2D::CreateTransient(128, 256));
	const TStrongObjectPtr<UTexture2D> Small(UTexture2D::CreateTransient(64, 64));
	TArray<TPair<FName, bool>> Asked;
	Seams.ResolveRetailTexture = [&](FName Name, bool bSmooth) -> UTexture2D*
	{
		Asked.Emplace(Name, bSmooth);
		if (Name == FName(TEXT("InfoBoxC1"))) return Corner.Get();
		if (Name == FName(TEXT("InfoBoxC2"))) return Other.Get();
		return Small.Get();
	};
	const TSharedRef<FWoTRetailArt> Full = FWoTRetailArt::Resolve();
	for (const EWoTRetailArtPiece Piece : Pieces)
	{
		TestTrue(TEXT("with the art every piece draws it"),
			WoTOnboardingRetailArtDraws(Full, Piece));
	}
	TestNotNull(TEXT("the bevel resolves"), Full->IconMask());
	TestNotNull(TEXT("the selection frame resolves"), Full->SelectionFrame());
	TestTrue(TEXT("the frame pieces resolve crisp"),
		Asked.Contains(TPair<FName, bool>(FName(TEXT("InfoBoxC1")), false))
		&& Asked.Contains(TPair<FName, bool>(FName(TEXT("InfoBoxC2")), false))
		&& Asked.Contains(TPair<FName, bool>(FName(TEXT("IconMask")), false)));
	TestTrue(TEXT("the selection frame resolves smooth"),
		Asked.Contains(TPair<FName, bool>(FName(TEXT("StatusIconFrame")), true)));

	// Only the corner texture missing: the window falls back alone.
	Seams.ResolveRetailTexture = [&](FName Name, bool) -> UTexture2D*
	{
		return Name == FName(TEXT("InfoBoxC1")) ? nullptr : Other.Get();
	};
	const TSharedRef<FWoTRetailArt> NoCorners = FWoTRetailArt::Resolve();
	TestFalse(TEXT("the window needs its corners"),
		WoTOnboardingRetailArtDraws(NoCorners, EWoTRetailArtPiece::Window));
	TestTrue(TEXT("the divider does not"),
		WoTOnboardingRetailArtDraws(NoCorners, EWoTRetailArtPiece::Divider));

	// The fixed heights the layouts reserve.
	const TSharedRef<SWoTRetailArt> Divider =
		SNew(SWoTRetailArt).Piece(EWoTRetailArtPiece::Divider).Art(Full);
	Divider->SlatePrepass(1.0f);
	TestEqual(TEXT("a divider is 16 units tall"),
		static_cast<float>(Divider->GetDesiredSize().Y), SWoTRetailArt::DividerHeight);
	const TSharedRef<SWoTRetailArt> GemBar =
		SNew(SWoTRetailArt).Piece(EWoTRetailArtPiece::GemBar).Art(Full);
	GemBar->SlatePrepass(1.0f);
	TestEqual(TEXT("a gem bar is 32 units tall"),
		static_cast<float>(GemBar->GetDesiredSize().Y), SWoTRetailArt::GemBarHeight);
	TestFalse(TEXT("the art never hit-tests"),
		GemBar->GetVisibility().IsHitTestVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtIconsSpec,
	"WoT.Onboarding.RetailArtIcons", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtIconsSpec::RunTest(const FString&)
{
	const FWoTOnboardingRetailArtSeamScope Scope;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TStrongObjectPtr<UTexture2D> Icon(UTexture2D::CreateTransient(64, 64));
	int32 Calls = 0;
	Seams.ResolveRetailTexture = nullptr;
	Seams.ResolveItemIcon = [&](FName ClassName, bool) -> UTexture2D*
	{
		++Calls;
		return ClassName == FName(TEXT("EmberLob")) ? Icon.Get() : nullptr;
	};
	const TSharedRef<FWoTRetailArt> Art = FWoTRetailArt::Resolve();
	const FSlateBrush* Crisp = Art->ItemIcon(FName(TEXT("EmberLob")), false);
	if (!TestNotNull(TEXT("a known class has an icon"), Crisp))
	{
		return false;
	}
	const FVector2f IconSize = FVector2f(Crisp->GetImageSize());
	TestTrue(TEXT("the icon keeps its size"),
		IconSize.X == 64.0f && IconSize.Y == 64.0f);
	TestTrue(TEXT("a repeat lookup is cached"),
		Art->ItemIcon(FName(TEXT("EmberLob")), false) == Crisp && Calls == 1);
	TestNotNull(TEXT("the smooth icon resolves on its own"),
		Art->ItemIcon(FName(TEXT("EmberLob")), true));
	TestEqual(TEXT("smooth and crisp resolve once each"), Calls, 2);
	TestNull(TEXT("an unknown class has none"),
		Art->ItemIcon(FName(TEXT("NotAnItem")), false));
	TestNull(TEXT("a miss is cached as a miss"),
		Art->ItemIcon(FName(TEXT("NotAnItem")), false));
	TestEqual(TEXT("the miss resolved once"), Calls, 3);
	TestNull(TEXT("None has no icon and asks nothing"), Art->ItemIcon(NAME_None, false));
	TestEqual(TEXT("None asked nothing"), Calls, 3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtGemsSpec,
	"WoT.Onboarding.RetailArtGems", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtGemsSpec::RunTest(const FString&)
{
	const TConstArrayView<FWoTRetailGem> Gems = SWoTRetailArt::Gems();
	if (!TestEqual(TEXT("five gems"), Gems.Num(), 5))
	{
		return false;
	}
	const TCHAR* const Order[] = {TEXT("Earth"), TEXT("Air"), TEXT("Fire"),
		TEXT("Water"), TEXT("Spirit")};
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TestEqual(TEXT("the gems run in the item-info window's order"),
			Gems[Index].Element, FName(Order[Index]));
		TestEqual(TEXT("each label is that element's window string"),
			FString(Gems[Index].LabelKey), FString(Order[Index]) + TEXT("Str"));
		TestEqual(TEXT("each lit gem sits beside its unlit one"),
			Gems[Index].On.Min.X - Gems[Index].Off.Min.X, 64.0f);
	}
	// The middle gem centred, the rest 64 apart.
	TestEqual(TEXT("the middle gem is centred"), SWoTRetailArt::GemX(422.0f, 2), 179.0f);
	TestEqual(TEXT("the first gem"), SWoTRetailArt::GemX(422.0f, 0), 51.0f);
	TestEqual(TEXT("the last gem"), SWoTRetailArt::GemX(422.0f, 4), 307.0f);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
