// Lease-pinned materialization of authenticated retail bitmap fonts for Slate.
#pragma once

#include "CoreMinimal.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/Texture2D.h"
#include "Fonts/SlateFontInfo.h"
#include "UObject/StrongObjectPtr.h"
#include "WoTCacheLease.h"
#include "WoTRuntimeFontResource.h"

class UWoTPayloadSubsystem;
class FWoTRequiredMenuFontGeneration;
struct FWoTTexturePayloadData;

struct FWoTFrontEndGlyphSource
{
	UTexture2D* AtlasTexture = nullptr;
	uint32 Page = 0;
	FIntRect AtlasRect;
	int32 Advance = 0;
	int32 BearingX = 0;
	int32 BearingY = 0;
	uint32 Baseline = 0;
	uint32 LineHeight = 0;
};

/** One immutable font-v2 face, its authenticated source atlases, and the
 * transient inline bitmap face Slate consumes without consulting a fallback. */
class WOT_API FWoTFrontEndFontResource final
{
public:
	UFont* Font() const { return SlateFont.Get(); }
	UFont* CanvasFontObject() const { return CanvasFont.Get(); }
	UFontFace* SlateFaceObject() const { return SlateFace.Get(); }
	UFont* NativeMultiplayerFont() const { return NativeSlateFont.Get(); }
	UFontFace* NativeMultiplayerFaceObject() const { return NativeSlateFace.Get(); }
	UFont* DoubledFont() const { return DoubledSlateFont.Get(); }
	UFontFace* DoubledFaceObject() const { return DoubledSlateFace.Get(); }
	FSlateFontInfo SlateInfo(float Size) const;
	bool ResolveGlyphSource(uint32 Codepoint, FWoTFrontEndGlyphSource& Out) const;
	int32 GlyphCount() const { return FontRecord.Glyphs().Num(); }
	int32 PageCount() const { return FontRecord.Pages().Num(); }
	const FString& SourceExport() const { return FontRecord.SourceExport(); }

private:
	FWoTRuntimeFontResource FontRecord;
	TArray<TSharedPtr<const FWoTTexturePayloadData>> AtlasPayloads;
	TArray<TStrongObjectPtr<UTexture2D>> AtlasTextures;
	TStrongObjectPtr<UFont> CanvasFont;
	TStrongObjectPtr<UFontFace> SlateFace;
	TStrongObjectPtr<UFont> SlateFont;
	TStrongObjectPtr<UFontFace> NativeSlateFace;
	TStrongObjectPtr<UFont> NativeSlateFont;
	TStrongObjectPtr<UFontFace> DoubledSlateFace;
	TStrongObjectPtr<UFont> DoubledSlateFont;

	friend class FWoTFrontEndFontOwner;
};

class WOT_API FWoTFrontEndFontOwner final
{
public:
	FWoTFrontEndFontOwner() = default;
	~FWoTFrontEndFontOwner();
	FWoTFrontEndFontOwner(const FWoTFrontEndFontOwner&) = delete;
	FWoTFrontEndFontOwner& operator=(const FWoTFrontEndFontOwner&) = delete;

	FWoTBootstrapStatus Initialize(UWoTPayloadSubsystem& Payload,
		const FWoTCacheGenerationLease& Lease);
#if WITH_AUTOMATION_TESTS
	/** Fixture bridge for the legacy unscoped cache activation helper. The
	 * production entry point above still accepts only a payload-domain lease. */
	FWoTBootstrapStatus InitializeForTesting(UWoTPayloadSubsystem& Payload,
		const FWoTCacheGenerationLease& UnscopedLease);
	static void ResetMaterializationWorkForTesting();
	static uint32 GetAtlasMaterializationAttemptsForTesting();
	static uint32 GetBdfConstructionAttemptsForTesting();
	static uint32 GetFreeTypeValidationAttemptsForTesting();
	static void SetForcePostCacheValidationFailureForTesting(bool bForce);
	static TWeakObjectPtr<UFont> GetLastValidatedFontForTesting();
	bool BeginLeaseRevocationForTesting(
		TArray<TFunction<bool()>>& OutCallbacks) const;
	void FinishLeaseRevocationForTesting(
		TArray<TFunction<bool()>>&& Callbacks) const;
#endif
	TWoTBootstrapResult<FSlateFontInfo> Resolve(FName Face) const;
	/** Lazily materialize exact 1x/2x bitmap strikes for multiplayer text. */
	TWoTBootstrapResult<FSlateFontInfo> ResolveIntegerScaled(
		FName Face, int32 PixelScale);
	FWoTBootstrapStatus EnsureBrowserFont(UWoTPayloadSubsystem& Payload);
	UFont* ResolveFontObject(FName Face) const;
	UFont* ResolveCanvasFontObject(FName Face) const;
	const FWoTFrontEndFontResource* ResolveResource(FName Id) const;
	TWeakPtr<FWoTRequiredMenuFontGeneration> FontGeneration() const;
	bool IsValid() const;
	bool SharesSnapshotWith(const FWoTCacheGenerationLease& Lease) const;
	/** Retain the exact payload snapshot for the presentation lifetime. */
	TUniquePtr<FWoTCacheGenerationLease> RetainPresentationLease() const;
	void Shutdown();

	static const FWoTFrontEndFontOwner* GetActive();

private:
	static bool ValidateFreeTypeFace(FWoTFrontEndFontResource& Resource);
	FWoTBootstrapStatus EnsureFace(FName FaceId);
	FWoTBootstrapStatus MaterializeFace(UWoTPayloadSubsystem& Payload,
		FName FaceId, const FString& LogicalId);
	FWoTBootstrapStatus InitializeRequired(
		UWoTPayloadSubsystem& Payload, const FWoTCacheGenerationLease& Lease,
		TConstArrayView<FString> RequiredLogicalIds);
	TUniquePtr<FWoTCacheGenerationLease> PinnedLease;
	TWeakObjectPtr<UWoTPayloadSubsystem> PayloadOwner;
	TMap<FName, TUniquePtr<FWoTFrontEndFontResource>> Resources;
	TSet<FName> FailedFaces;
	bool bReportedUnsupportedFace = false;
	TSharedPtr<FWoTRequiredMenuFontGeneration> Generation;
	static FWoTFrontEndFontOwner* Active;
};
