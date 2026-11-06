// The setup (bootstrap) screen's backdrop: the blurred main-menu capture, the
// generated first-run gradient, the saved file, and the 2x screen column.
#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "../UI/SWoTBootstrapScreen.h"
#include "../UI/WoTSetupBackdrop.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
