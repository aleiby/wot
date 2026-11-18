using UnrealBuildTool;

public class WoTOnboarding : ModuleRules
{
	public WoTOnboarding(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately minimal. The plugin consumes the game module's
		// onboarding seams (WoTOnboardingSeams.h: plain delegates, TFunction
		// rows, the row registries) and links no other game surface, so
		// disabling or omitting the plugin costs the game nothing and the
		// parity lanes can run without it.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// The guide browser is a Slate overlay; InputCore carries the FKey
			// close-key plumbing it shares with the retail help window.
			"InputCore",
			"Slate",
			"SlateCore"
		});

		// The compiled guide rows are JSON (Private/Generated/
		// WoTAngrealGuideRows.inl, parsed at runtime). "WoT" is the seam
		// header's home and the one game-module dependency.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"WoT"
		});
	}
}
