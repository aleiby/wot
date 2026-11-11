#include "Modules/ModuleManager.h"
#include "Assets/WoTRuntimeAssets.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Inventory/WoTInventory.h"
#include "Updater/WoTGameBootstrapWorker.h"
#include "Updater/WoTReleaseIdentity.h"
#if PLATFORM_MAC
#include "Updater/Mac/WoTMacUpdateSwap.h"
#endif
#include "World/WoTWorldMaterializer.h"
#include "WoTBootFailureReceipt.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTNetVersion.h"
#include "WoTMacUninstall.h"
#include "WoTMenuStyle.h"
#include "WoTOnboardingSeams.h"
#include "WoTRandom.h"
#include "WoTRetailText.h"
#include "Testing/WoTStereoNonRealtimeAudio.h"
#include "HAL/PlatformMisc.h"
#include "Misc/MessageDialog.h"
#include "WoTUpdaterCore/UpdaterPaths.h"
#include "WoTUpdaterCore/UpdaterPlatform.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTBootstrapCore/TestOverrides.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#if WOT_GUARDED_BOOTSTRAP && WOT_WITH_EOS && WITH_EOS_SDK
#include "IEOSSDKManager.h"
#include "Online/WoTEOSCachePolicy.h"
#endif
#if !WOT_GUARDED_BOOTSTRAP
#include "RetailData/WoTDevRetailFamilies.h"
#endif

#include <cstdlib>

#if WOT_GUARDED_BOOTSTRAP && WOT_WITH_BOOTSTRAP_TEST_HARNESSES
#include <cstdio>
// Test packages compile logging out; stderr is the only channel that still
// works when the heartbeat itself cannot open. Shipping compiles this away.
[[maybe_unused]] static void WoTGuardedStartDiagnostic(const TCHAR* Step, const FString& Detail)
{
	fprintf(stderr, "WoT guarded start [%s]: %s\n",
		TCHAR_TO_UTF8(Step), TCHAR_TO_UTF8(*Detail));
	fflush(stderr);
}
#endif

namespace
{
/** The HUD's own texture options, so a crisp request shares the texture the
    HUD draws; a smooth one asks for bilinear filtering instead. */
FWoTRuntimeTextureOptions WoTOnboardingTextureOptions(bool bSmooth)
{
	FWoTRuntimeTextureOptions Options;
	if (bSmooth)
	{
		Options.Filter = TF_Bilinear;
	}
	return Options;
}

/** The original UI art the onboarding seam names. The item-info window's
    textures live in the WOT package's UI group, which imports under
    Textures/WOT like every WOT texture (BindStatusIcons builds the same
    path); the gold selected-slot frame is the one the HUD draws, read from
    the inventory default that names it. */
UTexture2D* WoTOnboardingRetailTexture(FName Name, bool bSmooth)
{
	FSoftObjectPath Path;
	if (Name == FName(TEXT("StatusIconFrame")))
	{
		Path = GetDefault<AWoTInventory>()->StatusIconFrame.ToSoftObjectPath();
	}
	else if (Name == FName(TEXT("InfoBoxC1")) || Name == FName(TEXT("InfoBoxC2"))
		|| Name == FName(TEXT("IconMask")))
	{
		const FString Leaf = Name.ToString();
		Path = FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/WOT/%s.%s"), *Leaf, *Leaf));
	}
	return Path.IsNull() ? nullptr : UWoTRuntimeAssets::ResolveTexturePath(
		Path, WoTOnboardingTextureOptions(bSmooth));
}

UTexture2D* WoTOnboardingItemIcon(FName ClassName, bool bSmooth)
{
	const AWoTInventory* Default = AWoTInventory::FindDefaultByOriginalName(ClassName);
	return Default && !Default->StatusIcon.IsNull()
		? UWoTRuntimeAssets::ResolveTexturePath(Default->StatusIcon.ToSoftObjectPath(),
			WoTOnboardingTextureOptions(bSmooth))
		: nullptr;
}

/** The local player's controller: the onboarding windows draw on its
    viewport, over the front end and over a match alike. */
APlayerController* WoTOnboardingLocalController()
{
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
	UWorld* World = Viewport ? Viewport->GetWorld() : nullptr;
	return World ? World->GetFirstPlayerController() : nullptr;
}
} // namespace

class FWoTGameModule final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		WoTRandom::InitializeFromTime();
		WoTNetVersion::Bind();
		// Onboarding seam: the plugin asks for retail display text by
		// (file, section, property) without linking any game class. The
		// wrapped mechanism resolves the owning file from the section, so the
		// file argument is address documentation. Behavior-neutral: nothing
		// in the game module reads through this slot.
		FWoTOnboardingSeams::Get().ResolveRetailText =
			[](const FString&, const FString& Section, const FString& Property)
			{
				FWoTRetailText::EnsureLoaded();
				const FString* Value =
					FWoTRetailText::FindSectionValue(Section, Property);
				return Value ? *Value : FString();
			};
		// Onboarding seam: the original game's faces, resolved lazily per call
		// so nothing here touches fonts or payloads at startup. The exact
		// bitmap strikes come from a presentation authority for the local
		// player, as the help and in-game menu windows take theirs; the
		// widget keeps it alive through the chrome and collapses with its
		// revocation. Unset while the guarded faces are unavailable or
		// revoked; a consumer then draws with its own fallback.
		FWoTOnboardingSeams::Get().ResolveMenuChrome =
			[]() -> TOptional<FWoTOnboardingMenuChrome>
			{
				APlayerController* Controller = WoTOnboardingLocalController();
				const TSharedPtr<FWoTFrontEndPresentationAuthority> Authority =
					Controller ? FWoTFrontEndPresentationAuthority::Create(Controller)
					           : TSharedPtr<FWoTFrontEndPresentationAuthority>();
				if (!Authority.IsValid() || !Authority->IsReady())
				{
					return TOptional<FWoTOnboardingMenuChrome>();
				}
				FWoTOnboardingMenuChrome Chrome;
				Chrome.PixelFont = [Authority](FName Face, int32 PixelScale)
				{
					return Authority->MultiplayerFont(
						Face, static_cast<float>(PixelScale));
				};
				Chrome.BindWidget = [Authority](const TSharedRef<SWidget>& Widget)
				{
					Authority->BindWidget(Widget);
				};
				return Chrome;
			};
		// Onboarding seams: the original UI art and the item icons, resolved
		// the way the HUD and the item-info window resolve theirs.
		FWoTOnboardingSeams::Get().ResolveRetailTexture = &WoTOnboardingRetailTexture;
		FWoTOnboardingSeams::Get().ResolveItemIcon = &WoTOnboardingItemIcon;
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
		WoTStereoNonRealtimeAudio::Register();
#endif
#if !WOT_GUARDED_BOOTSTRAP
		// Startup is a game-thread, non-CDO hook shared by editor, PIE and dev
		// games. Guarded targets compile both the entry point and this call out.
		WoTDevRetailFamiliesEnsureActivated();
#endif

#if WOT_GUARDED_BOOTSTRAP || (!WITH_EDITOR && !IS_PROGRAM)
		const FWoTResolvedExecutableLayout ExecutableLayout =
			WoTResolveExecutableLayout();
		FWoTBootstrapStatus ProfileSession =
			WoTAcquireProcessProfileSessionShared(
				ExecutableLayout.UserStateRoot);
		if (ProfileSession.IsError())
		{
			const FString Message = WoTProfileSessionFailureMessage(
				ExecutableLayout.UserStateRoot, ProfileSession.GetError());
			FPlatformMisc::LowLevelOutputDebugStringf(
				TEXT("%s\n"), *Message);
			if (!FApp::IsUnattended())
			{
				FPlatformMisc::MessageBoxExt(
					EAppMsgType::Ok, *Message, TEXT("WoT Reborn"));
			}
			std::_Exit(EXIT_FAILURE);
		}
		WoTAdoptResolvedExecutableLayout(ExecutableLayout);
		// The lifetime shared hold above precedes every started write. A second
		// launcher can see pending during startup, but cannot roll back started.
		if (!ExecutableLayout.InstallRoot.IsEmpty() && !ExecutableLayout.LiveLeaf.IsEmpty())
		{
			auto UpdateRoot = ResolveUpdaterStateRoot(ExecutableLayout.UserStateRoot,
				ExecutableLayout.InstallRoot, ExecutableLayout.LiveLeaf);
			if (!UpdateRoot.IsError())
			{
			#if PLATFORM_MAC
				const uint64 Running =
					WoTReleaseIdentity::LoadRunning().ReleaseSequence;
				if (!WoTMacCheckUpdateTrialAtStartup(UpdateRoot.GetValue(),
					Running, WoTMacDefaultUpdateSwapEffects()))
				{
					std::_Exit(EXIT_FAILURE);
				}
				WoTMacRememberUpdateRootForQuit(
					UpdateRoot.GetValue(), Running);
			#endif
				auto Started = WoTMarkUpdateTrialStarted(UpdateRoot.GetValue());
				if (Started.IsError())
				{
					FPlatformMisc::MessageBoxExt(EAppMsgType::Ok,
						TEXT("WoT Reborn couldn't record the update start. Free some disk space, check that this folder is writable, and try again."), TEXT("WoT Reborn"));
					std::_Exit(EXIT_FAILURE);
				}
			}
		}
	#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
		// The funnel's bad-build fixture. A Mac bundle can't be signed with a
		// loose file at its root, so there it sits in sealed Contents/Resources.
		#if PLATFORM_MAC
		const TCHAR* BadBuildMarker = TEXT("Contents/Resources/wot-test-fail-after-started");
		#else
		const TCHAR* BadBuildMarker = TEXT("wot-test-fail-after-started");
		#endif
		if (IFileManager::Get().FileExists(*FPaths::Combine(
			ExecutableLayout.InstalledTreeRoot, BadBuildMarker)))
			std::_Exit(EXIT_FAILURE);
	#endif
#endif
#if WOT_GUARDED_BOOTSTRAP
	#if WOT_WITH_EOS && WITH_EOS_SDK
		// OnlineSubsystemEOS otherwise derives its cache from UserSettingsDir
		// (LOCALAPPDATA on Windows). Install the override before any named EOS
		// subsystem can create its platform, and keep its UTF-8 storage alive.
		IEOSSDKManager* EOSSDKManager = IEOSSDKManager::Get();
		if (EOSSDKManager == nullptr
			|| EOSSDKManager->GetActivePlatforms().Num() != 0)
		{
			const FString Message = TEXT(
				"WoT Reborn could not secure its online data folder.");
			FPlatformMisc::LowLevelOutputDebugStringf(TEXT("%s\n"), *Message);
			FPlatformMisc::MessageBoxExt(
				EAppMsgType::Ok, *Message, TEXT("WoT Reborn"));
			std::_Exit(EXIT_FAILURE);
		}
		FWoTBootstrapStatus EOSCacheRegistered =
			WoTRegisterEOSCachePolicyForStartup(
				EOSSDKManager, ExecutableLayout.UserStateRoot,
				EOSCachePolicy, EOSCachePolicyHandle);
		if (EOSCacheRegistered.IsError())
		{
			const FString Message = TEXT(
				"WoT Reborn could not secure its online data folder.");
			FPlatformMisc::LowLevelOutputDebugStringf(TEXT("%s\n"), *Message);
			FPlatformMisc::MessageBoxExt(
				EAppMsgType::Ok, *Message, TEXT("WoT Reborn"));
			std::_Exit(EXIT_FAILURE);
		}
	#endif
	#if WOT_GUARDED_BOOTSTRAP && WOT_WITH_BOOTSTRAP_TEST_HARNESSES
		WoTGuardedStartDiagnostic(TEXT("layout"), FString::Printf(
			TEXT("valid=%d tree=%s exe=%s user=%s user_identity=%d disable=%s"),
			ExecutableLayout.IsValid() ? 1 : 0, *ExecutableLayout.InstalledTreeRoot,
			*ExecutableLayout.ExecutableTreeRelativePath,
			*ExecutableLayout.UserStateRoot,
			ExecutableLayout.bHasUserStateRootIdentity ? 1 : 0,
			*ExecutableLayout.UpdateDisableReason));
	#endif
#endif

#if WOT_GUARDED_BOOTSTRAP || WITH_AUTOMATION_TESTS
		// Editor automation cannot define WOT_GUARDED_BOOTSTRAP globally, but it
		// must still exercise the permanent dispatcher rather than calling
		// the project callback directly. The callback owns no captured startup
		// state: automation may adopt its synthetic authority after module load,
		// and every dispatch observes the current fail-closed authority object.
		FString RegistrationError;
			if (!WoTRegisterGuardedWorldMaterializer(
			&WoTMaterializeAuthenticatedWorld,
			&WoTCommitAuthenticatedWorldExposure, &RegistrationError))
			{
			#if WOT_GUARDED_BOOTSTRAP
				(void)WoTWriteBootFailureReceipt(
					EWoTBootstrapDiagnosticCode::BootstrapFailure,
					EWoTBootstrapPhase::EstablishingIdentity);
			#endif
				UE_LOG(LogWoTBootstrap, Fatal,
				TEXT("Guarded world materializer registration failed: %s"),
				*RegistrationError);
		}
#endif

	#if PLATFORM_MAC && WOT_GUARDED_BOOTSTRAP && !WITH_EDITOR && !IS_PROGRAM
		WoTInstallMacUninstallMenuItem();
	#endif
	}

	virtual void ShutdownModule() override
	{
	#if PLATFORM_MAC && WOT_GUARDED_BOOTSTRAP && !WITH_EDITOR && !IS_PROGRAM
		WoTMacApplyRememberedUpdateAtQuit();
		WoTShutdownMacUninstallMenuItem();
	#endif
	#if WOT_GUARDED_BOOTSTRAP && WOT_WITH_EOS && WITH_EOS_SDK
		if (EOSCachePolicyHandle.IsValid())
		{
			if (IEOSSDKManager* EOSSDKManager = IEOSSDKManager::Get())
			{
				EOSSDKManager->OnPreCreatePlatform.Remove(EOSCachePolicyHandle);
			}
			EOSCachePolicyHandle.Reset();
		}
		EOSCachePolicy.Reset();
	#endif
		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	#if WOT_GUARDED_BOOTSTRAP && WOT_WITH_EOS && WITH_EOS_SDK
	TUniquePtr<FWoTEOSCachePolicy> EOSCachePolicy;
	FDelegateHandle EOSCachePolicyHandle;
	#endif
};

#if IS_PROGRAM
// Program targets (WoTGuardedEngineTests) link this module for guarded-branch
// compile coverage; the program supplies its own primary module.
IMPLEMENT_MODULE(FWoTGameModule, WoT);
#else
IMPLEMENT_PRIMARY_GAME_MODULE(FWoTGameModule, WoT, "WoT");
#endif
