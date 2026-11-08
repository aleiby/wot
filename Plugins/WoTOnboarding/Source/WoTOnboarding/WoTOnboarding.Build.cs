using UnrealBuildTool;

public class WoTOnboarding : ModuleRules
{
	public WoTOnboarding(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately minimal. The plugin consumes the game module's
		// onboarding seams — plain delegates and TFunctions — and links no
		// Source/WoT class, so disabling or omitting the plugin costs the
		// game nothing and the parity lanes can run without it.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		// The editor-only import/export seam speaks JSON so the content
		// importer can drive it through reflection alone.
		PrivateDependencyModuleNames.Add("Json");
	}
}
