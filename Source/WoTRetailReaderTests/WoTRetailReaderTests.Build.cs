using UnrealBuildTool;

public class WoTRetailReaderTests : ModuleRules
{
	public WoTRetailReaderTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateIncludePathModuleNames.Add("Launch");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Projects",
			"WoTBootstrapCore",
			"WoTRetailReader"
		});
	}
}
