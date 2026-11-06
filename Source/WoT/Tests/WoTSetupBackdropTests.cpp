// The setup (bootstrap) screen's backdrop: the blurred main-menu capture, the
// compiled-in default, the generated gradient, the saved file, the order
// Current() picks them in, and the 2x screen column.
#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "../UI/SWoTBootstrapScreen.h"
#include "../UI/WoTSetupBackdrop.h"
#include "../WoTPreferencesSubsystem.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
constexpr EAutomationTestFlags WoTSetupBackdropSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FString WoTSetupBackdropSpecPath(const TCHAR* Leaf)
{
	return FPaths::Combine(FPaths::AutomationTransientDir(),
		TEXT("WoTSetupBackdrop"), Leaf);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSetupBackdropFromCaptureSpec,
	"WoT.UI.SetupBackdrop.FromCapture", WoTSetupBackdropSpecFlags)

bool FWoTSetupBackdropFromCaptureSpec::RunTest(const FString&)
{
	// Left half black, right half white, at a typical 16:9 capture size.
	const FIntPoint Size(1280, 720);
	TArray<FColor> Capture;
	Capture.SetNumUninitialized(Size.X * Size.Y);
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			Capture[Y * Size.X + X] = X < Size.X / 2 ? FColor::Black : FColor::White;
		}
	}
	const TArray<FColor> Backdrop = WoTSetupBackdrop::FromCapture(Capture, Size);
	if (!TestEqual(TEXT("the backdrop is 640 x 360"), Backdrop.Num(),
		WoTSetupBackdrop::Width * WoTSetupBackdrop::Height))
	{
		return false;
	}
	const int32 Row = WoTSetupBackdrop::Height / 2 * WoTSetupBackdrop::Width;
	const FColor Left = Backdrop[Row];
	const FColor Right = Backdrop[Row + WoTSetupBackdrop::Width - 1];
	const FColor Seam = Backdrop[Row + WoTSetupBackdrop::Width / 2];
	TestTrue(TEXT("the black half stays black"), Left.R <= 2);
	// Mean luminance ~127 is far above the screen-like 14: the gain floors
	// at 0.5, so white lands at about half.
	TestTrue(TEXT("a bright scene is toned down (gain floor 0.5)"),
		Right.R >= 120 && Right.R <= 135);
	TestTrue(TEXT("the seam is blurred between them"),
		Seam.R > Left.R + 15 && Seam.R + 15 < Right.R);
	bool bOpaque = true;
	for (const FColor& Color : Backdrop)
	{
		bOpaque &= Color.A == 255;
	}
	TestTrue(TEXT("every backdrop pixel is opaque"), bOpaque);
	// A night scene is brought to the menu screen's own brightness (~14).
	TArray<FColor> Dark;
	Dark.Init(FColor(7, 7, 7, 255), Size.X * Size.Y);
	const TArray<FColor> Matched = WoTSetupBackdrop::FromCapture(Dark, Size);
	TestTrue(TEXT("a dark scene matches the menu screen's brightness"),
		Matched.Num() > 0 && Matched[0].R >= 12 && Matched[0].R <= 16);
	TestTrue(TEXT("a capture whose size does not match its pixels is refused"),
		WoTSetupBackdrop::FromCapture(Capture, FIntPoint(1280, 719)).IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSetupBackdropFallbackSpec,
	"WoT.UI.SetupBackdrop.Fallback", WoTSetupBackdropSpecFlags)

bool FWoTSetupBackdropFallbackSpec::RunTest(const FString&)
{
	const TArray<FColor> Backdrop = WoTSetupBackdrop::Fallback();
	if (!TestEqual(TEXT("the generated backdrop is 640 x 360"), Backdrop.Num(),
		WoTSetupBackdrop::Width * WoTSetupBackdrop::Height))
	{
		return false;
	}
	auto At = [&Backdrop](int32 X, int32 Y) { return Backdrop[Y * WoTSetupBackdrop::Width + X]; };
	const int32 MidX = WoTSetupBackdrop::Width / 2;
	TestTrue(TEXT("the top is lighter than the bottom"),
		At(MidX, 2).B > At(MidX, WoTSetupBackdrop::Height - 3).B + 10);
	TestTrue(TEXT("the corners are darker than the centre line (vignette)"),
		At(0, 2).B < At(MidX, 2).B);
	bool bDark = true;
	for (const FColor& Color : Backdrop)
	{
		bDark &= Color.R <= 50 && Color.G <= 50 && Color.B <= 50 && Color.A == 255;
	}
	TestTrue(TEXT("the generated backdrop stays dark behind white text"), bDark);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSetupBackdropFileSpec,
	"WoT.UI.SetupBackdrop.File", WoTSetupBackdropSpecFlags)

bool FWoTSetupBackdropFileSpec::RunTest(const FString&)
{
	IFileManager& Files = IFileManager::Get();
	const FString Saved = WoTSetupBackdropSpecPath(TEXT("backdrop.png"));
	Files.Delete(*Saved, false, true, true);
	TestTrue(TEXT("a missing backdrop is stale"), WoTSetupBackdrop::IsStale(Saved));
	TArray<FColor> Loaded;
	TestFalse(TEXT("a missing backdrop does not load"),
		WoTSetupBackdrop::Load(Saved, Loaded));

	const TArray<FColor> Backdrop = WoTSetupBackdrop::Fallback();
	if (!TestTrue(TEXT("a backdrop saves"), WoTSetupBackdrop::Save(Backdrop, Saved)))
	{
		return false;
	}
	TestTrue(TEXT("a saved backdrop loads"), WoTSetupBackdrop::Load(Saved, Loaded));
	TestTrue(TEXT("the PNG round trip is exact"), Loaded == Backdrop);
	TArray<FString> Temporaries;
	Files.FindFiles(Temporaries, *FPaths::Combine(FPaths::GetPath(Saved), TEXT("*.tmp")),
		/*Files*/ true, /*Directories*/ false);
	TestTrue(TEXT("no temporary file is left behind"), Temporaries.IsEmpty());
	TestFalse(TEXT("a backdrop newer than the executable is current"),
		WoTSetupBackdrop::IsStale(Saved));
	// A recent but corrupt file is stale, so the next capture repairs it.
	const FString Corrupt = WoTSetupBackdropSpecPath(TEXT("corrupt.png"));
	FFileHelper::SaveStringToFile(TEXT("\x89PNG corrupt"), *Corrupt);
	TestTrue(TEXT("a recent but corrupt backdrop is stale"),
		WoTSetupBackdrop::IsStale(Corrupt));
	Files.SetTimeStamp(*Saved, FDateTime(2000, 1, 1));
	TestTrue(TEXT("a backdrop older than the executable is stale"),
		WoTSetupBackdrop::IsStale(Saved));

	// A PNG of the wrong size and a non-image are both refused.
	const FString Small = WoTSetupBackdropSpecPath(TEXT("small.png"));
	TArray<FColor> Tiny;
	Tiny.Init(FColor::Red, 16);
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(4, 4,
		TArrayView64<const FColor>(Tiny.GetData(), Tiny.Num()), Png);
	FFileHelper::SaveArrayToFile(Png, *Small);
	TestFalse(TEXT("a wrong-size PNG is refused"), WoTSetupBackdrop::Load(Small, Loaded));
	const FString Garbage = WoTSetupBackdropSpecPath(TEXT("garbage.png"));
	FFileHelper::SaveStringToFile(TEXT("not an image"), *Garbage);
	TestFalse(TEXT("a non-image is refused"), WoTSetupBackdrop::Load(Garbage, Loaded));
	TestFalse(TEXT("a wrong-size backdrop does not save"),
		WoTSetupBackdrop::Save(Tiny, Small + TEXT(".out")));

	// The header is read before decoding: a PNG declaring a huge size is
	// refused without being expanded.
	FIntPoint Declared;
	TestTrue(TEXT("a real PNG header reads its size"),
		WoTSetupBackdrop::ReadPngSize(TArray<uint8>(Png.GetData(), static_cast<int32>(Png.Num())), Declared)
		&& Declared == FIntPoint(4, 4));
	TArray<uint8> Huge(Png.GetData(), static_cast<int32>(Png.Num()));
	Huge[16] = 0x00; Huge[17] = 0x01; Huge[18] = 0x00; Huge[19] = 0x00;   // width 65536
	Huge[20] = 0x00; Huge[21] = 0x01; Huge[22] = 0x00; Huge[23] = 0x00;   // height 65536
	const FString Bomb = WoTSetupBackdropSpecPath(TEXT("bomb.png"));
	FFileHelper::SaveArrayToFile(Huge, *Bomb);
	TestFalse(TEXT("a PNG declaring the wrong size is refused before decoding"),
		WoTSetupBackdrop::Load(Bomb, Loaded));
	TestFalse(TEXT("a non-PNG has no PNG size"),
		WoTSetupBackdrop::ReadPngSize(TArray<uint8>({1, 2, 3}), Declared));

	for (const FString& Path : {Saved, Small, Garbage, Corrupt, Bomb})
	{
		Files.Delete(*Path, false, true, true);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSetupBackdropDefaultSpec,
	"WoT.UI.SetupBackdrop.Default", WoTSetupBackdropSpecFlags)

bool FWoTSetupBackdropDefaultSpec::RunTest(const FString&)
{
	TArray<FColor> Backdrop;
	if (!TestTrue(TEXT("the compiled-in default decodes"),
			WoTSetupBackdrop::Default(Backdrop))
		|| !TestEqual(TEXT("the compiled-in default is 640 x 360"), Backdrop.Num(),
			WoTSetupBackdrop::Width * WoTSetupBackdrop::Height))
	{
		return false;
	}
	bool bOpaque = true;
	double LuminanceSum = 0.0;
	for (const FColor& Color : Backdrop)
	{
		bOpaque &= Color.A == 255;
		LuminanceSum += 0.2126 * Color.R + 0.7152 * Color.G + 0.0722 * Color.B;
	}
	const double Mean = LuminanceSum / Backdrop.Num();
	TestTrue(TEXT("every default pixel is opaque"), bOpaque);
	// Made like a capture, so it sits at the menu screen's own brightness (~14).
	TestTrue(FString::Printf(TEXT("the default stays dark behind white text (mean %.1f)"),
		Mean), Mean >= 8.0 && Mean <= 24.0);
	TestFalse(TEXT("the default is an image, not the generated gradient"),
		Backdrop == WoTSetupBackdrop::Fallback());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSetupBackdropCurrentSpec,
	"WoT.UI.SetupBackdrop.Current", WoTSetupBackdropSpecFlags)

bool FWoTSetupBackdropCurrentSpec::RunTest(const FString&)
{
	// An isolated preference path: the saved capture lives beside it, never
	// beside the user's real preferences.
	const FString Directory = WoTSetupBackdropSpecPath(
		*(FString(TEXT("Current-")) + FGuid::NewGuid().ToString()));
	IFileManager& Files = IFileManager::Get();
	Files.MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	ON_SCOPE_EXIT
	{
		WoTSetupBackdrop::SetDefaultPngForTesting(nullptr);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	FString Saved;
	if (!TestTrue(TEXT("the saved backdrop path resolves"), WoTSetupBackdrop::Path(Saved))
		|| !TestTrue(TEXT("the saved backdrop path is the spec's own"),
			Saved.StartsWith(Directory)))
	{
		return false;
	}
	TArray<FColor> ShippedDefault;
	if (!TestTrue(TEXT("the compiled-in default decodes"), WoTSetupBackdrop::Default(ShippedDefault)))
	{
		return false;
	}
	const TArray<FColor> Gradient = WoTSetupBackdrop::Fallback();

	// A fresh install: no capture yet, so the setup screen shows the default.
	TestFalse(TEXT("no capture exists yet"), Files.FileExists(*Saved));
	TestTrue(TEXT("without a capture, Current() is the default"),
		WoTSetupBackdrop::Current() == ShippedDefault);

	// The first main-menu visit saves the player's own capture, which wins.
	TArray<FColor> Capture;
	Capture.Init(FColor(40, 20, 10, 255), WoTSetupBackdrop::Width * WoTSetupBackdrop::Height);
	if (!TestTrue(TEXT("a capture saves"), WoTSetupBackdrop::Save(Capture, Saved)))
	{
		return false;
	}
	TestTrue(TEXT("with a capture, Current() is the capture"),
		WoTSetupBackdrop::Current() == Capture);

	// A capture that does not load falls back to the default, not the gradient.
	FFileHelper::SaveStringToFile(TEXT("\x89PNG corrupt"), *Saved);
	TestTrue(TEXT("a corrupt capture falls back to the default"),
		WoTSetupBackdrop::Current() == ShippedDefault);

	// The gradient only when the default itself is unusable: not a PNG, or a
	// PNG of the wrong size. Neither crashes.
	Files.Delete(*Saved, false, true, true);
	TArray<uint8> Garbage = {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g'};
	WoTSetupBackdrop::SetDefaultPngForTesting(&Garbage);
	TArray<FColor> Unusable;
	TestFalse(TEXT("a non-PNG default does not decode"), WoTSetupBackdrop::Default(Unusable));
	TestTrue(TEXT("an unusable default and no capture: the gradient"),
		WoTSetupBackdrop::Current() == Gradient);
	TArray<FColor> Tiny;
	Tiny.Init(FColor::Red, 16);
	TArray64<uint8> TinyPng;
	FImageUtils::PNGCompressImageArray(4, 4,
		TArrayView64<const FColor>(Tiny.GetData(), Tiny.Num()), TinyPng);
	TArray<uint8> WrongSize(TinyPng.GetData(), static_cast<int32>(TinyPng.Num()));
	WoTSetupBackdrop::SetDefaultPngForTesting(&WrongSize);
	TestFalse(TEXT("a wrong-size default does not decode"), WoTSetupBackdrop::Default(Unusable));
	TestTrue(TEXT("a wrong-size default and no capture: the gradient"),
		WoTSetupBackdrop::Current() == Gradient);
	TestTrue(TEXT("the capture still wins over an unusable default"),
		WoTSetupBackdrop::Save(Capture, Saved) && WoTSetupBackdrop::Current() == Capture);

	WoTSetupBackdrop::SetDefaultPngForTesting(nullptr);
	TArray<FColor> Restored;
	TestTrue(TEXT("clearing the override restores the compiled-in default"),
		WoTSetupBackdrop::Default(Restored) && Restored == ShippedDefault);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSetupBackdropScreenSpec,
	"WoT.UI.SetupBackdrop.ScreenColumn", WoTSetupBackdropSpecFlags)

bool FWoTSetupBackdropScreenSpec::RunTest(const FString&)
{
	// The setup screen is twice its first size (720 -> 1440 column), and a
	// narrower landscape window shrinks the column instead of clipping it.
	const TSharedRef<SWoTBootstrapScreen> Screen = SNew(SWoTBootstrapScreen);
	TestEqual(TEXT("unarranged: the full 2x column"),
		Screen->ContentWidthForTesting(0.0f), 1440.0f);
	TestEqual(TEXT("16:9 (1920 units): the full 2x column"),
		Screen->ContentWidthForTesting(1920.0f), 1440.0f);
	TestEqual(TEXT("4:3 (1440 units): shrinks inside the margins"),
		Screen->ContentWidthForTesting(1440.0f), 1344.0f);
	TestEqual(TEXT("a window narrower than its margins never goes negative"),
		Screen->ContentWidthForTesting(50.0f), 0.0f);
	return !HasAnyErrors();
}

#endif
