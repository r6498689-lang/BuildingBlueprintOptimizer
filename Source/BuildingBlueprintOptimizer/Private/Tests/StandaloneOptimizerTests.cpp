#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintOptimizerLibrary.h"
#include "BuildingOutlinerVisibility.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PreviewScene.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBOStandaloneTest, "BuildingBlueprintOptimizer.Standalone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBBOStandaloneTest::RunTest(const FString&)
{
    // Explicit, isolated validation only; never write fixture assets during Run All.
    if (!FParse::Param(FCommandLine::Get(), TEXT("BBCSplitValidation"))) return true;
    const bool bExpectCreator = FParse::Param(FCommandLine::Get(), TEXT("BBCExpectCreator"));
    TestEqual(TEXT("Creator module is optional and can coexist"),
        FModuleManager::Get().IsModuleLoaded(TEXT("BuildingBlueprintCreator")), bExpectCreator);
    TestTrue(TEXT("Null input is rejected"), !UBuildingBlueprintOptimizerLibrary::GetBuildingOptimizationReason(nullptr).IsEmpty());
    const FString Folder = TEXT("/Game/BBOValidation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Package = CreatePackage(*(Folder / TEXT("Source")));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("Source"),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Standalone fixture created with native engine APIs"), BP)) return false;
    TestTrue(TEXT("Ordinary blueprint is rejected"), !UBuildingBlueprintOptimizerLibrary::GetBuildingOptimizationReason(BP).IsEmpty());
    BBCOutliner::MarkBuilding(BP);
    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    auto* Root = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("BuildingRoot"));
    SCS->AddNode(Root);
    CastChecked<USceneComponent>(Root->ComponentTemplate)->SetMobility(EComponentMobility::Static);
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Engine test cube exists"), Cube)) return false;
    for (int32 I = 0; I < 2; ++I)
    {
        auto* Node = SCS->CreateNode(UChildActorComponent::StaticClass(), FName(*FString::Printf(TEXT("Part_%03d_Cube"), I + 1)));
        auto* Child = CastChecked<UChildActorComponent>(Node->ComponentTemplate);
        Child->SetMobility(EComponentMobility::Static);
        Child->SetChildActorClass(AStaticMeshActor::StaticClass());
        auto* Template = Cast<AStaticMeshActor>(Child->GetChildActorTemplate());
        if (!TestNotNull(TEXT("Native child actor template"), Template)) return false;
        Template->GetStaticMeshComponent()->SetStaticMesh(Cube);
        Template->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
        Child->SetRelativeLocation_Direct(FVector(I * 250.f, 0.f, 0.f));
        Root->AddChildNode(Node);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
    if (!TestTrue(TEXT("Native fixture saves"), UPackage::SavePackage(Package, BP, *File, Args))) return false;
    TestTrue(TEXT("Saved compatible building accepted without creator"), UBuildingBlueprintOptimizerLibrary::GetBuildingOptimizationReason(BP).IsEmpty());
    auto Result = UBuildingBlueprintOptimizerLibrary::OptimizeBuildingBlueprint(BP, Folder, TEXT("Optimized"));
    AddInfo(Result.Message);
    if (!TestTrue(TEXT("Standalone ISM optimization succeeds"), Result.bSuccess)) return false;
    TestEqual(TEXT("Two repeated parts become two instances"), Result.ISMInstanceCount, 2);
    TestEqual(TEXT("One ISM group"), Result.ISMGroupCount, 1);
    TestEqual(TEXT("Source component hierarchy remains unchanged"), BP->SimpleConstructionScript->GetAllNodes().Num(), 3);
    auto* Output = LoadObject<UBlueprint>(nullptr, *Result.AssetObjectPath);
    if (!TestNotNull(TEXT("Optimized asset can be loaded"), Output)) return false;
    FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
    AActor* Actor = Preview.GetWorld()->SpawnActor<AActor>(Output->GeneratedClass);
    if (!TestNotNull(TEXT("Optimized asset can be instantiated"), Actor)) return false;
    TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(Actor);
    if (!TestEqual(TEXT("Output contains one ISM"), ISMs.Num(), 1)) return false;
    TestEqual(TEXT("Mesh reference retained"), ISMs[0]->GetStaticMesh().Get(), Cube);
    TestEqual(TEXT("Output retains both geometry instances"), ISMs[0]->GetInstanceCount(), 2);
    return !HasAnyErrors();
}
#endif
