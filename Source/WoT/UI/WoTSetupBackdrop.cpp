#include "WoTSetupBackdrop.h"

#include "../WoTBootstrapHeartbeat.h"
#include "../WoTPreferencesSubsystem.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "CoreGlobals.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TextureResource.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
// The saved backdrop is tiny; anything larger is not ours.
constexpr int64 WoTSetupBackdropMaxFileBytes = 1024 * 1024;
// Separable box blur, three passes (close to a Gaussian), then the capture's
// mean luminance is scaled to the menu screen's own. Measured 2026-10-05 on
// the live menu: the screen averages ~13/255 (dark navy), the raw linear
// capture of the same view ~7; lifting toward a bright target (48) read as
// flat grey to the player, so the target is the screen itself.
constexpr int32 WoTSetupBackdropBlurRadius = 3;
constexpr int32 WoTSetupBackdropBlurPasses = 3;
constexpr float WoTSetupBackdropTargetMean = 14.0f;
constexpr float WoTSetupBackdropMinGain = 0.5f;
constexpr float WoTSetupBackdropMaxGain = 3.0f;

// The scene is rendered at the backdrop size (no downsampling needed).
constexpr int32 WoTSetupBackdropCaptureWidth = WoTSetupBackdrop::Width;
constexpr int32 WoTSetupBackdropCaptureHeight = WoTSetupBackdrop::Height;

// The one pending capture (a transient component owned by the controller) and
// its generation: cancelling or re-arming bumps the generation, so a next-tick
// read that is already scheduled for an older capture does nothing at all.
TWeakObjectPtr<USceneCaptureComponent2D> GWoTSetupBackdropCapture;
uint64 GWoTSetupBackdropGeneration = 0;

// Packaged Test builds compile logging out; the durable heartbeat log is the
// only place a field capture can say why it did or did not happen.
void WoTSetupBackdropHeartbeat(const FString& Detail)
{
	(void)WoTRecordBootstrapHeartbeat(TEXT("setup-backdrop"),
		EWoTBootstrapPhase::Ready, 0, {}, {}, Detail);
}

void WoTSetupBackdropBoxBlur(TArray<FVector3f>& Pixels)
{
	TArray<FVector3f> Scratch;
	Scratch.SetNumUninitialized(Pixels.Num());
	auto Pass = [](const TArray<FVector3f>& From, TArray<FVector3f>& To,
		int32 Count, int32 Lines, bool bRows)
	{
		for (int32 Line = 0; Line < Lines; ++Line)
		{
			auto At = [&](int32 Index) -> int32
			{
				const int32 Clamped = FMath::Clamp(Index, 0, Count - 1);
				return bRows ? Line * Count + Clamped : Clamped * Lines + Line;
			};
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FVector3f Sum = FVector3f::ZeroVector;
				for (int32 Offset = -WoTSetupBackdropBlurRadius;
					Offset <= WoTSetupBackdropBlurRadius; ++Offset)
				{
					Sum += From[At(Index + Offset)];
				}
				To[At(Index)] = Sum / static_cast<float>(
					2 * WoTSetupBackdropBlurRadius + 1);
			}
		}
	};
	for (int32 Iteration = 0; Iteration < WoTSetupBackdropBlurPasses; ++Iteration)
	{
		Pass(Pixels, Scratch, WoTSetupBackdrop::Width, WoTSetupBackdrop::Height, true);
		Pass(Scratch, Pixels, WoTSetupBackdrop::Height, WoTSetupBackdrop::Width, false);
	}
}

uint8 WoTSetupBackdropChannel(float Value)
{
	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value), 0, 255));
}
}

TArray<FColor> WoTSetupBackdrop::FromCapture(
	TConstArrayView<FColor> Pixels, FIntPoint Size)
{
	TArray<FColor> Result;
	if (Size.X <= 0 || Size.Y <= 0
		|| static_cast<int64>(Pixels.Num()) != static_cast<int64>(Size.X) * Size.Y)
	{
		return Result;
	}
	// Box downsample: each backdrop pixel averages its source rectangle.
	TArray<FVector3f> Linearized;
	Linearized.SetNumUninitialized(Width * Height);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 Y0 = static_cast<int32>(static_cast<int64>(Y) * Size.Y / Height);
		const int32 Y1 = FMath::Max(Y0 + 1,
			static_cast<int32>(static_cast<int64>(Y + 1) * Size.Y / Height));
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 X0 = static_cast<int32>(static_cast<int64>(X) * Size.X / Width);
			const int32 X1 = FMath::Max(X0 + 1,
				static_cast<int32>(static_cast<int64>(X + 1) * Size.X / Width));
			FVector3f Sum = FVector3f::ZeroVector;
			for (int32 SourceY = Y0; SourceY < FMath::Min(Y1, Size.Y); ++SourceY)
			{
				for (int32 SourceX = X0; SourceX < FMath::Min(X1, Size.X); ++SourceX)
				{
					const FColor& Color = Pixels[SourceY * Size.X + SourceX];
					Sum += FVector3f(Color.R, Color.G, Color.B);
				}
			}
			const int32 Count = FMath::Max(1,
				(FMath::Min(Y1, Size.Y) - Y0) * (FMath::Min(X1, Size.X) - X0));
			Linearized[Y * Width + X] = Sum / static_cast<float>(Count);
		}
	}
	WoTSetupBackdropBoxBlur(Linearized);
	double LuminanceSum = 0.0;
	for (const FVector3f& Color : Linearized)
	{
		LuminanceSum += 0.2126 * Color.X + 0.7152 * Color.Y + 0.0722 * Color.Z;
	}
	const float Mean = static_cast<float>(LuminanceSum / Linearized.Num());
	const float Gain = FMath::Clamp(WoTSetupBackdropTargetMean / FMath::Max(Mean, 1.0f),
		WoTSetupBackdropMinGain, WoTSetupBackdropMaxGain);
	Result.SetNumUninitialized(Width * Height);
	for (int32 Index = 0; Index < Linearized.Num(); ++Index)
	{
		const FVector3f Color = Linearized[Index] * Gain;
		Result[Index] = FColor(WoTSetupBackdropChannel(Color.X),
			WoTSetupBackdropChannel(Color.Y), WoTSetupBackdropChannel(Color.Z), 255);
	}
	return Result;
}

TArray<FColor> WoTSetupBackdrop::Fallback()
{
	// Deep slate at the top to near-black at the bottom, a soft vignette, and
	// a little deterministic noise so the stretched gradient does not band.
	const FVector3f Top(24.0f, 30.0f, 40.0f);
	const FVector3f Bottom(6.0f, 7.0f, 10.0f);
	TArray<FColor> Result;
	Result.SetNumUninitialized(Width * Height);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const float V = static_cast<float>(Y) / static_cast<float>(Height - 1);
		const FVector3f Base = FMath::Lerp(Top, Bottom, V);
		for (int32 X = 0; X < Width; ++X)
		{
			const float U = static_cast<float>(X) / static_cast<float>(Width - 1);
			const float DX = (U - 0.5f) * 2.0f;
			const float DY = (V - 0.5f) * 2.0f;
			const float Vignette = 1.0f
				- 0.35f * FMath::Clamp((DX * DX + DY * DY) * 0.5f, 0.0f, 1.0f);
			const uint32 Hash = static_cast<uint32>(X) * 73856093u
				^ static_cast<uint32>(Y) * 19349663u;
			const float Noise = static_cast<float>(Hash % 5u) - 2.0f;
			const FVector3f Color = Base * Vignette + FVector3f(Noise);
			Result[Y * Width + X] = FColor(WoTSetupBackdropChannel(Color.X),
				WoTSetupBackdropChannel(Color.Y), WoTSetupBackdropChannel(Color.Z), 255);
		}
	}
	return Result;
}

bool WoTSetupBackdrop::Path(FString& OutPath)
{
	return UWoTPreferencesSubsystem::SideFilePath(TEXT("WoTSetupBackdrop.png"), OutPath);
}

bool WoTSetupBackdrop::Save(TConstArrayView<FColor> Backdrop, const FString& InPath)
{
	if (Backdrop.Num() != Width * Height || InPath.IsEmpty())
	{
		return false;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Width, Height,
		TArrayView64<const FColor>(Backdrop.GetData(), Backdrop.Num()), Png);
	if (Png.IsEmpty())
	{
		return false;
	}
	if (Png.Num() > TNumericLimits<int32>::Max())
	{
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.Append(Png.GetData(), static_cast<int32>(Png.Num()));
	return UWoTPreferencesSubsystem::SaveSideFileBytes(InPath, Bytes);
}

bool WoTSetupBackdrop::ReadPngSize(TConstArrayView<uint8> Bytes, FIntPoint& OutSize)
{
	// Signature, then the IHDR chunk: length (13), "IHDR", width, height (big-endian).
	static const uint8 Signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (Bytes.Num() < 24 || FMemory::Memcmp(Bytes.GetData(), Signature, 8) != 0
		|| Bytes[12] != 'I' || Bytes[13] != 'H' || Bytes[14] != 'D' || Bytes[15] != 'R')
	{
		return false;
	}
	auto BigEndian = [&Bytes](int32 Offset)
	{
		return (static_cast<uint32>(Bytes[Offset]) << 24)
			| (static_cast<uint32>(Bytes[Offset + 1]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 8)
			| static_cast<uint32>(Bytes[Offset + 3]);
	};
	const uint32 PngWidth = BigEndian(16);
	const uint32 PngHeight = BigEndian(20);
	if (PngWidth == 0 || PngHeight == 0 || PngWidth > 0x7fffffffu || PngHeight > 0x7fffffffu)
	{
		return false;
	}
	OutSize = FIntPoint(static_cast<int32>(PngWidth), static_cast<int32>(PngHeight));
	return true;
}

bool WoTSetupBackdrop::Load(const FString& InPath, TArray<FColor>& OutBackdrop)
{
	OutBackdrop.Reset();
	TArray<uint8> Compressed;
	FImage Image;
	FIntPoint Declared;
	// The header is checked before decoding: a small, highly compressible file
	// must not expand into a huge image.
	if (!UWoTPreferencesSubsystem::LoadSideFileBytes(
			InPath, WoTSetupBackdropMaxFileBytes, Compressed)
		|| Compressed.IsEmpty()
		|| !ReadPngSize(Compressed, Declared) || Declared != FIntPoint(Width, Height)
		|| !FImageUtils::DecompressImage(Compressed.GetData(), Compressed.Num(), Image)
		|| Image.SizeX != Width || Image.SizeY != Height || Image.NumSlices != 1)
	{
		return false;
	}
	Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	const TArrayView64<FColor> Colors = Image.AsBGRA8();
	if (Colors.Num() != Width * Height)
	{
		return false;
	}
	OutBackdrop.Append(Colors.GetData(), static_cast<int32>(Colors.Num()));
	for (FColor& Color : OutBackdrop)
	{
		Color.A = 255;
	}
	return true;
}

TArray<FColor> WoTSetupBackdrop::Current()
{
	FString Saved;
	TArray<FColor> Backdrop;
	if (Path(Saved) && Load(Saved, Backdrop))
	{
		return Backdrop;
	}
	return Fallback();
}

bool WoTSetupBackdrop::IsStale(const FString& InPath)
{
	IFileManager& Files = IFileManager::Get();
	const FDateTime Saved = Files.GetTimeStamp(*InPath);
	TArray<FColor> Existing;
	if (Saved == FDateTime::MinValue() || !Load(InPath, Existing))
	{
		return true;
	}
	const FDateTime Executable = Files.GetTimeStamp(FPlatformProcess::ExecutablePath());
	return Executable != FDateTime::MinValue() && Saved < Executable;
}

TSharedPtr<FSlateDynamicImageBrush> WoTSetupBackdrop::MakeBrush(
	TConstArrayView<FColor> Backdrop)
{
	if (Backdrop.Num() != Width * Height || !FSlateApplication::IsInitialized()
		|| FSlateApplication::Get().GetRenderer() == nullptr)
	{
		return nullptr;
	}
	static int32 Generation = 0;
	TArray<uint8> Bgra;
	Bgra.Append(reinterpret_cast<const uint8*>(Backdrop.GetData()),
		Backdrop.Num() * sizeof(FColor));
	return FSlateDynamicImageBrush::CreateWithImageData(
		FName(*FString::Printf(TEXT("WoTSetupBackdrop%d"), ++Generation)),
		FVector2D(Width, Height), Bgra);
}

void WoTSetupBackdrop::CaptureIfStale(APlayerController* Controller)
{
	FString Target;
	if (GIsAutomationTesting || IsRunningCommandlet() || IsRunningDedicatedServer())
	{
		return;
	}
	UWorld* World = Controller != nullptr ? Controller->GetWorld() : nullptr;
	APlayerCameraManager* Camera =
		Controller != nullptr ? Controller->PlayerCameraManager.Get() : nullptr;
	const TCHAR* Skip = World == nullptr || Camera == nullptr ? TEXT("no-camera")
		: !FApp::CanEverRender() ? TEXT("cannot-render")
		: GWoTSetupBackdropCapture.IsValid() ? TEXT("already-armed")
		: !Path(Target) ? TEXT("no-path")
		: !IsStale(Target) ? TEXT("current")
		: nullptr;
	if (Skip != nullptr)
	{
		WoTSetupBackdropHeartbeat(FString::Printf(TEXT("skip reason=%s"), Skip));
		return;
	}

	// Reading the game viewport from inside its own draw deadlocked the game
	// thread (probe 2026-10-04: status stopped answering the moment the capture
	// fired). A scene capture renders the camera's view into its own target,
	// which is read outside any draw on the next tick.
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(Controller);
	RenderTarget->InitCustomFormat(WoTSetupBackdropCaptureWidth,
		WoTSetupBackdropCaptureHeight, PF_B8G8R8A8,
		// Linear: the sRGB target read back the scene encoded a second time
		// (top band 39/47/70 against the screen's 13/15/25, the "flat grey"
		// report); the linear read tracks the screen's hue (7/9/19) at about
		// half its brightness, which the mean-luminance match restores.
		/*bInForceLinearGamma*/ true);
	RenderTarget->UpdateResourceImmediate(/*bClearRenderTarget*/ true);
	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(Controller);
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->TextureTarget = RenderTarget;
	const FMinimalViewInfo& View = Camera->GetCameraCacheView();
	Capture->FOVAngle = View.FOV;
	// The camera's own exposure and grading, so the capture matches the
	// screen rather than the capture defaults (which rendered far brighter).
	Capture->PostProcessSettings = View.PostProcessSettings;
	Capture->PostProcessBlendWeight = View.PostProcessBlendWeight;
	Capture->SetWorldLocationAndRotation(View.Location, View.Rotation);
	Capture->RegisterComponentWithWorld(World);
	Capture->CaptureScene();
	GWoTSetupBackdropCapture = Capture;
	const uint64 Generation = ++GWoTSetupBackdropGeneration;
	WoTSetupBackdropHeartbeat(TEXT("armed"));

	TWeakObjectPtr<USceneCaptureComponent2D> WeakCapture(Capture);
	TWeakObjectPtr<UTextureRenderTarget2D> WeakTarget(RenderTarget);
	World->GetTimerManager().SetTimerForNextTick(
		[WeakCapture, WeakTarget, Target, Generation]()
	{
		if (Generation != GWoTSetupBackdropGeneration)
		{
			return;   // cancelled (EndPlay) or superseded
		}
		USceneCaptureComponent2D* Pending = WeakCapture.Get();
		UTextureRenderTarget2D* Rendered = WeakTarget.Get();
		if (Pending == nullptr)
		{
			GWoTSetupBackdropCapture.Reset();
			WoTSetupBackdropHeartbeat(TEXT("read-failed capture-gone"));
			return;
		}
		FTextureRenderTargetResource* Resource = Rendered != nullptr
			? Rendered->GameThread_GetRenderTargetResource() : nullptr;
		TArray<FColor> Colors;
		const bool bRead = Resource != nullptr && Resource->ReadPixels(Colors,
			FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX));
		Pending->DestroyComponent();
		GWoTSetupBackdropCapture.Reset();
		if (!bRead)
		{
			WoTSetupBackdropHeartbeat(TEXT("read-failed"));
			return;
		}
		const TArray<FColor> Backdrop = FromCapture(Colors,
			FIntPoint(WoTSetupBackdropCaptureWidth, WoTSetupBackdropCaptureHeight));
		const bool bSaved = !Backdrop.IsEmpty() && Save(Backdrop, Target);
		WoTSetupBackdropHeartbeat(FString::Printf(
			TEXT("captured pixels=%d saved=%d"), Colors.Num(), bSaved ? 1 : 0));
	});
}

void WoTSetupBackdrop::CancelPendingCapture()
{
	if (USceneCaptureComponent2D* Pending = GWoTSetupBackdropCapture.Get())
	{
		++GWoTSetupBackdropGeneration;   // the scheduled read becomes a no-op
		Pending->DestroyComponent();
		WoTSetupBackdropHeartbeat(TEXT("cancelled"));
	}
	GWoTSetupBackdropCapture.Reset();
}
