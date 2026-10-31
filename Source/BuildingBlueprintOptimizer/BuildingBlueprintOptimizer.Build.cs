using UnrealBuildTool;

public class BuildingBlueprintOptimizer : ModuleRules
{
    public BuildingBlueprintOptimizer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "InputCore", "MeshDescription", "StaticMeshDescription", "UnrealEd", "LevelEditor", "Slate", "SlateCore", "ContentBrowser", "RenderCore", "RHI", "ApplicationCore",
            "AssetRegistry", "AssetTools", "Kismet", "KismetCompiler", "BlueprintGraph", "Projects", "DatasmithContent", "SceneOutliner", "ToolMenus", "StaticMeshEditor"
        });
    }
}
