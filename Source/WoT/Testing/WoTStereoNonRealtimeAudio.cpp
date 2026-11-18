#include "WoTStereoNonRealtimeAudio.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
#if IS_MONOLITHIC

#include "AudioMixerDevice.h"
#include "AudioMixerPlatformNonRealtime.h"
#include "Modules/ModuleManager.h"

namespace
{
	class FStereoNonRealtimePlatform final : public Audio::FMixerPlatformNonRealtime
	{
	public:
		virtual bool GetOutputDeviceInfo(const uint32 InDeviceIndex,
			Audio::FAudioPlatformDeviceInfo& OutInfo) override
		{
			if (!FMixerPlatformNonRealtime::GetOutputDeviceInfo(InDeviceIndex, OutInfo))
			{
				return false;
			}
			// The panner sizes its map from this list and the mix from NumChannels.
			OutInfo.OutputChannelArray.SetNum(
				FMath::Min(OutInfo.OutputChannelArray.Num(), OutInfo.NumChannels));
			return true;
		}
	};

	class FStereoNonRealtimeModule final : public IAudioDeviceModule
	{
	public:
		virtual bool IsAudioMixerModule() const override { return true; }

		virtual FAudioDevice* CreateAudioDevice() override
		{
			return new Audio::FMixerDevice(new FStereoNonRealtimePlatform());
		}
	};

	IModuleInterface* CreateStereoNonRealtimeModule()
	{
		return new FStereoNonRealtimeModule();
	}
}

void WoTStereoNonRealtimeAudio::Register()
{
	FModuleManager::RegisterStaticallyLinkedModule(
		TEXT("WoTStereoNonRealtime"), &CreateStereoNonRealtimeModule);
}

#else

// The renderer's constructor isn't exported, so only a monolithic build can
// derive from it. The rig runs packaged (monolithic) Test builds only.
void WoTStereoNonRealtimeAudio::Register()
{
}

#endif
#endif
