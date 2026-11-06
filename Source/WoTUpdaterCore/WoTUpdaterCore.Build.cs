using UnrealBuildTool;

public class WoTUpdaterCore : ModuleRules
{
	public WoTUpdaterCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This dependency floor is the updater's architecture boundary. Keep all
		// engine, object, launch, HTTP, UI, and service policy above this module.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"WoTBootstrapCore"
		});
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
		// Specs share WoTBootstrapCore's native test factory (e.g.
		// ChangeMetadataOnly for the activation metadata-churn specs), as
		// WoTRetailReader's do; the factory compiles only with automation tests.
		PrivateIncludePaths.Add(
			System.IO.Path.Combine(ModuleDirectory, "../WoTBootstrapCore/Private/Tests"));

		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.AddRange(new string[]
			{
				"AppKit",
				"CoreServices",
				"Security"
			});
		}

		// The Private/Tests fixtures sign ed25519 test material with OpenSSL
		// (EVP_DigestSign). The module free-rode on WoTBootstrapCore's PRIVATE
		// OpenSSL link, which resolves inside a monolithic test exe but not
		// across DLL boundaries in a MODULAR editor/game build -- so the symbols
		// went unresolved the moment WoT linked this module
		// (on both platforms). Declare what the test code actually uses; gated to
		// test configurations so the shipping dependency floor is unchanged.
		if (Target.Configuration != UnrealTargetConfiguration.Shipping
			&& Target.Configuration != UnrealTargetConfiguration.Test)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}
	}
}
