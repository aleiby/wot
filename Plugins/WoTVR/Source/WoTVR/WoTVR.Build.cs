using UnrealBuildTool;

public class WoTVR : ModuleRules
{
	public WoTVR(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore"
		});

		// The game module is reached only through its local-player hooks
		// (WoTLocalPlayerHooks.h): the plugin links no game class, so disabling
		// or omitting it costs the game nothing. Stock OpenXR is reached only
		// through the session abstraction (WoTVRSession.h); the rig and the
		// policies never call XR directly.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			// The headset menus paint offscreen with the widget renderer.
			"UMG",
			"HeadMountedDisplay",
			"RHI",
			"RenderCore",
			"XRBase",
			"OpenXRHMD",
			"WoT",
			"WoTVRGate"
		});

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenXR");
	}
}
