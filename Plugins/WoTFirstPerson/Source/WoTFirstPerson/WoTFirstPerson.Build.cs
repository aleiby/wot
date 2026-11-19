using UnrealBuildTool;

public class WoTFirstPerson : ModuleRules
{
	public WoTFirstPerson(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});
		// The extension seam is the only game-module surface used here.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"WoT",
			"SlateCore",
			// Tracked hands: the engine's hand-tracker interface and the tuning files.
			"HeadMountedDisplay",
			"Json"
		});
	}
}
