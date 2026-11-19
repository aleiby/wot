using UnrealBuildTool;

public class WoTBots : ModuleRules
{
	public WoTBots(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"WoT",
			"Json",
			"NavigationSystem",
			"WoTRetailReader",
			"WoTBootstrapCore",
			"OnlineSubsystem",
			"OnlineSubsystemUtils"
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
