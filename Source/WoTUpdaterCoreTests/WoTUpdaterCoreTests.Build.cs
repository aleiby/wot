using UnrealBuildTool;

public class WoTUpdaterCoreTests : ModuleRules
{
	public WoTUpdaterCoreTests(ReadOnlyTargetRules Target) : base(Target)
	{
		// The Mac LaunchServices fixture must be an AppKit-only Objective-C++
		// translation unit. Keep this module's private PCH deliberately UE-free
		// so it cannot reintroduce the Carbon FVector collision.
		PCHUsage = PCHUsageMode.NoSharedPCHs;
		PrivatePCHHeaderFile = "Private/WoTUpdaterCoreTestsPCH.h";

		PrivateIncludePathModuleNames.Add("Launch");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Projects",
			"WoTUpdaterCore"
		});
		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.Add("AppKit");
		}
	}
}
