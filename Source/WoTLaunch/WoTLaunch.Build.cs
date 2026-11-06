using UnrealBuildTool;

public class WoTLaunch : ModuleRules
{
	public WoTLaunch(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately UObject-free. Game code may depend on this module, never
		// the reverse; the bootstrap result vocabulary is also Core-only.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"WoTBootstrapCore",
		});
		// The native display projection contract stops at ApplicationCore. It
		// never reaches Engine, Slate, project, plugin, or game-owned types.
		PrivateDependencyModuleNames.Add("ApplicationCore");

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Packaging wrappers consume this receipt property and must pass the
			// exact UAT argument. It prevents Epic's bootstrap executable from
			// becoming a second candidate-root game image.
			AdditionalPropertiesForReceipt.Add(
				"WoTRequiredUATArgument", "-NoBootstrapExe");
		}
	}
}
