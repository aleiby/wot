using UnrealBuildTool;

public class WoTVRGate : ModuleRules
{
	public WoTVRGate(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The XR gate loads at PostConfigInit, before the engine pre-initialises
		// HMD modules, so it stays on Core. HeadMountedDisplay supplies only the
		// HMD module interface header (the modular feature name and the module's
		// key name, both resolved without an import).
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HeadMountedDisplay"
		});
	}
}
