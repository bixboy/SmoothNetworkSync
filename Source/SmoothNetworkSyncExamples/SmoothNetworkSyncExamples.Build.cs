// Copyright (c) Bixboy, 2026. All Rights Reserved.

using UnrealBuildTool;

/** Demo actors used by the example map. Not needed to use Smooth Sync in your own project. */
public class SmoothNetworkSyncExamples : ModuleRules
{
	public SmoothNetworkSyncExamples(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Same engine header workaround as the runtime module (see SmoothNetworkSync.h).
#pragma warning disable CS0618
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion == 4)
		{
			bEnableUndefinedIdentifierWarnings = false;
			PCHUsage = ModuleRules.PCHUsageMode.NoSharedPCHs;
			PrivatePCHHeaderFile = "Public/SmoothNetworkSyncExamples.h";
		}
		else if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
		{
			bEnableUndefinedIdentifierWarnings = false;
		}
#pragma warning restore CS0618

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"SmoothNetworkSync",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"EnhancedInput",
			}
			);
	}
}
