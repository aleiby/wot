#pragma once

#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"
#include "HAL/CriticalSection.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "WoTBootstrapCore/Error.h"

/** The only display modes accepted by the player-preference grammar. */
enum class EWoTWindowMode : uint8
{
	Windowed,
	WindowedFullscreen,
	Fullscreen,
};

/** Closed locale identities. No preference value is ever interpreted as a path. */
enum class EWoTShippedLocale : uint8
{
	EnUS,
	FrFR,
	DeDE,
	EsES,
	ItIT,
};

struct FWoTEarlyUIV1
{
	uint16 Width = 1280;
	uint16 Height = 720;
	EWoTWindowMode WindowMode = EWoTWindowMode::Windowed;
	float UIScale = 1.0f;
	EWoTShippedLocale Locale = EWoTShippedLocale::EnUS;
	bool bReducedMotion = false;
	bool bHighContrast = false;
	bool bScreenReader = false;
	float BootstrapVolume = 1.0f;
	float InputSensitivity = 1.0f;

	WOTLAUNCH_API bool operator==(const FWoTEarlyUIV1& Other) const;
};

/** Signed/inventory-owned bounds supplied to the parser, never file-owned. */
struct FWoTEarlyUILimits
{
	FWoTEarlyUIV1 Defaults;
	uint16 MinWidth = 640;
	uint16 MaxWidth = 7680;
	uint16 MinHeight = 480;
	uint16 MaxHeight = 4320;
	float MinUIScale = 0.75f;
	float MaxUIScale = 2.0f;
	float MinBootstrapVolume = 0.0f;
	float MaxBootstrapVolume = 1.0f;
	float MinInputSensitivity = 0.1f;
	float MaxInputSensitivity = 10.0f;
	uint32 MaxSerializedBytes = 4096;

	WOTLAUNCH_API static FWoTEarlyUILimits Production();
};

/** The release inventory chooses which closed locale IDs are actually shipped. */
struct FWoTInventoryLocaleSet
{
	TArray<EWoTShippedLocale> Locales;

	WOTLAUNCH_API bool Contains(EWoTShippedLocale Locale) const;
	WOTLAUNCH_API static FWoTInventoryLocaleSet Production();
};

/** A value can receive this type only by passing the complete typed parser. */
struct FWoTAuthenticatedEarlyUI final
{
public:
	FWoTAuthenticatedEarlyUI(FWoTAuthenticatedEarlyUI&&) = default;
	FWoTAuthenticatedEarlyUI& operator=(FWoTAuthenticatedEarlyUI&&) = default;
	FWoTAuthenticatedEarlyUI(const FWoTAuthenticatedEarlyUI&) = delete;
	FWoTAuthenticatedEarlyUI& operator=(const FWoTAuthenticatedEarlyUI&) = delete;

	const FWoTEarlyUIV1& Get() const { return Value; }

private:
	explicit FWoTAuthenticatedEarlyUI(FWoTEarlyUIV1&& InValue)
		: Value(MoveTemp(InValue))
	{
	}

	FWoTEarlyUIV1 Value;

	// WOTLAUNCH_API required: this friend is the function's first declaration,
	// and a linkage mismatch with the exported declaration below is C2375 on
	// MSVC (fourth occurrence of the pattern; see 63a6cdb6 / c4efda52).
	friend WOTLAUNCH_API TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> ParseEarlyUIV1(
		TConstArrayView<uint8>, const FWoTEarlyUILimits&,
		const FWoTInventoryLocaleSet&);
};

/** The only native projection: bounded width, height, and the closed mode enum. */
struct FWoTEarlyUINativeProjectionSink
{
	TFunctionRef<FWoTBootstrapStatus(
		uint16 Width, uint16 Height, EWoTWindowMode WindowMode)> Apply;
};

WOTLAUNCH_API TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> ParseEarlyUIV1(
	TConstArrayView<uint8> Bytes, const FWoTEarlyUILimits& Limits,
	const FWoTInventoryLocaleSet& LocaleSet);

/**
 * Process-lifetime one-slot handoff. Projection observes the published value
 * without consuming it; take is legal only after the invalidate-first
 * projection has run.
 */
class WOTLAUNCH_API FWoTEarlyUIAuthority final
{
public:
	FWoTEarlyUIAuthority() = default;
	FWoTEarlyUIAuthority(const FWoTEarlyUIAuthority&) = delete;
	FWoTEarlyUIAuthority& operator=(const FWoTEarlyUIAuthority&) = delete;

	FWoTBootstrapStatus PublishOnce(FWoTAuthenticatedEarlyUI&& Value);
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> TakeOnce();
	FWoTBootstrapStatus Shutdown();

#if WITH_AUTOMATION_TESTS
	void ResetForTesting();
#endif

private:
	enum class EState : uint8
	{
		Empty,
		Published,
		Taken,
		Shutdown,
	};

	FWoTBootstrapStatus ApplyNativeProjectionOnce(
		const FWoTEarlyUINativeProjectionSink& Sink);

	FCriticalSection Guard;
	TOptional<FWoTAuthenticatedEarlyUI> PublishedValue;
	EState State = EState::Empty;
	bool bNativeProjectionApplied = false;

	// WOTLAUNCH_API for the same reason as the ParseEarlyUIV1 friend above.
	friend WOTLAUNCH_API bool WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
		const FWoTEarlyUINativeProjectionSink&, FString*);
};

WOTLAUNCH_API FWoTEarlyUIAuthority& GetEarlyUIAuthority();
