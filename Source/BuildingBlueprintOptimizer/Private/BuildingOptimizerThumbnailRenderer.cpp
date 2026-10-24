#include "BuildingOptimizerThumbnailRenderer.h"
#include "BuildingOutlinerVisibility.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "ObjectTools.h"
#include "RHI.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"

bool UBuildingOptimizerThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
    const UBlueprint* Blueprint = Cast<UBlueprint>(Object);
    if (BBCOutliner::IsGeneratedBlueprint(Blueprint))
        return IsValid(Blueprint->GeneratedClass) && Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass())
            && !Blueprint->GeneratedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated);
    return Super::CanVisualizeAsset(Object);
}

void UBuildingOptimizerThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
    FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
    UBlueprint* Blueprint = Cast<UBlueprint>(Object);
    if (!BBCOutliner::IsGeneratedBlueprint(Blueprint))
    {
        Super::Draw(Object, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
        return;
    }
    if (!IsValid(Blueprint) || !IsValid(Blueprint->GeneratedClass) || !Blueprint->bHasBeenRegenerated
        || Blueprint->bBeingCompiled || Blueprint->HasAnyFlags(RF_Transient)) return;

    // Keep just one preview world. Never retain 100 complete building instances
    // as the ordinary engine blueprint thumbnail cache does for classes.
    if (!BuildingScene) BuildingScene = MakeUnique<FBlueprintThumbnailScene>();
    BuildingScene->SetBlueprint(Blueprint);
    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        RenderTarget, BuildingScene->GetScene(), FEngineShowFlags(ESFIM_Game))
        .SetTime(UThumbnailRenderer::GetTime()).SetAdditionalViewFamily(bAdditionalViewFamily));
    ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
    ViewFamily.EngineShowFlags.MotionBlur = 0;
    RenderViewFamily(Canvas, &ViewFamily, BuildingScene->CreateView(&ViewFamily, X, Y, Width, Height));
    // The render commands already precede removal of the actor's render state.
    // Keep no preview building alive between draws; meshes/materials stay shared.
    BuildingScene->SetBlueprint(nullptr);
}

void UBuildingOptimizerThumbnailRenderer::BeginDestroy()
{
    BuildingScene.Reset();
    Super::BeginDestroy();
}

namespace BBCThumbnails
{
static bool Installed = false;
static TStrongObjectPtr<UThumbnailRenderer> PreviousRenderer;
static TWeakObjectPtr<UBuildingOptimizerThumbnailRenderer> InstalledRenderer;
static TWeakObjectPtr<UThumbnailManager> InstalledManager;

static FThumbnailRenderingInfo* BlueprintRegistration(UThumbnailManager& Manager)
{
    // The manager exposes registration methods, but no getter for an entry
    // rejected by CanVisualizeAsset. Read its reflected, typed configuration.
    const FArrayProperty* Property = FindFProperty<FArrayProperty>(Manager.GetClass(), TEXT("RenderableThumbnailTypes"));
    if (!Property) return nullptr;
    auto* Entries = Property->ContainerPtrToValuePtr<TArray<FThumbnailRenderingInfo>>(&Manager);
    return Entries->FindByPredicate([](const FThumbnailRenderingInfo& Entry)
        { return Entry.ClassNeedingThumbnail == UBlueprint::StaticClass(); });
}

void Start()
{
    if (Installed || !FApp::CanEverRender() || GUsingNullRHI) return;
    UThumbnailManager& Manager = UThumbnailManager::Get();
    FThumbnailRenderingInfo* Existing = BlueprintRegistration(Manager);
    if (!Existing || !Existing->Renderer || Existing->Renderer->GetClass() != UBlueprintThumbnailRenderer::StaticClass())
    {
        UE_LOG(LogTemp, Warning, TEXT("Building Blueprint Creator: another Blueprint thumbnail renderer is installed; leaving it unchanged."));
        return;
    }
    // Preserve the original entry's position, class and configuration string.
    // Re-registering UBlueprint at the end would take precedence over specialized
    // renderers for derived assets such as AnimBlueprint and WidgetBlueprint.
    // Existing lookup-cache entries point at this same struct; no invalidation
    // or config-file mutation is needed when replacing only its renderer object.
    PreviousRenderer.Reset(Existing->Renderer.Get());
    UBuildingOptimizerThumbnailRenderer* Renderer = NewObject<UBuildingOptimizerThumbnailRenderer>(GetTransientPackage());
    InstalledRenderer = Renderer;
    Existing->Renderer = Renderer;
    InstalledManager = &Manager;
    Installed = true;
}

void Stop()
{
    if (!Installed) return;
    // TryGet returns a raw singleton pointer even after UObject destruction.
    // Resolve our weak reference instead, and never recreate the manager here.
    if (UThumbnailManager* Manager = InstalledManager.Get())
    {
        FThumbnailRenderingInfo* Existing = BlueprintRegistration(*Manager);
        if (Existing && Existing->Renderer == InstalledRenderer.Get())
            Existing->Renderer = PreviousRenderer.Get();
    }
    InstalledRenderer.Reset();
    InstalledManager.Reset();
    PreviousRenderer.Reset();
    Installed = false;
}

void CacheForSave(UBlueprint* Blueprint)
{
    if (FApp::CanEverRender() && !GUsingNullRHI)
        ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Blueprint);
}
}
