#include "BuildingOutlinerVisibility.h"

#include "ActorTreeItem.h"
#include "Components/ChildActorComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "ILevelEditor.h"
#include "ISceneOutliner.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "SceneOutlinerFilters.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace BBCOutliner
{
static constexpr const TCHAR* BuildingKey = TEXT("BuildingBlueprintCreator.GeneratedBuilding");

void MarkBuilding(UBlueprint* Blueprint)
{
    Blueprint->GetOutermost()->GetMetaData().SetValue(Blueprint, BuildingKey, TEXT("1"));
}

bool IsGeneratedBuilding(const AActor* Actor)
{
    return Actor && IsGeneratedBlueprint(Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy));
}

bool IsGeneratedBlueprint(const UBlueprint* Blueprint)
{
    if (!Blueprint) return false;
    if (Blueprint->GetOutermost()->GetMetaData().GetValue(Blueprint, BuildingKey) == TEXT("1")) return true;

    // Read-only recognition of buildings created by versions 1.0.0--1.2.0.
    // Do not identify buildings by their asset name or inspect unrelated assets.
    if (Blueprint->ParentClass != AActor::StaticClass() || !Blueprint->SimpleConstructionScript) return false;
    const auto& Roots = Blueprint->SimpleConstructionScript->GetRootNodes();
    if (Roots.Num() != 1 || !Roots[0] || Roots[0]->GetVariableName() != TEXT("BuildingRoot")
        || Roots[0]->ComponentClass != USceneComponent::StaticClass()) return false;
    int32 Parts = 0;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node == Roots[0]) continue;
        if (!Node || Node->ComponentClass != UChildActorComponent::StaticClass()) return false;
        const FString Name = Node->GetVariableName().ToString();
        if (!Name.StartsWith(TEXT("Part_"))) return false;
        int32 End = 5;
        while (End < Name.Len() && FChar::IsDigit(Name[End])) ++End;
        if (End < 8 || End >= Name.Len() - 1 || Name[End] != TCHAR('_')) return false;
        ++Parts;
    }
    return Parts > 0;
}

bool ShouldShowActor(const AActor* Actor)
{
    if (!Actor || !Actor->IsChildActor()) return true;
    TSet<const AActor*> Visited;
    for (const AActor* Parent = Actor->GetParentActor(); Parent && !Visited.Contains(Parent); Parent = Parent->GetParentActor())
    {
        if (IsGeneratedBuilding(Parent)) return false;
        Visited.Add(Parent);
    }
    // Manually attached level actors are not ChildActors and remain visible,
    // as do the internal children of unrelated Blueprint actors.
    return true;
}

class FBuildingPartsFilter final : public TSceneOutlinerFilter<FActorTreeItem>
{
public:
    FBuildingPartsFilter() : TSceneOutlinerFilter<FActorTreeItem>(EDefaultBehaviour::Pass) {}
    virtual bool PassesFilterImpl(const FActorTreeItem& Item) const override { return ShouldShowActor(Item.Actor.Get()); }
};

struct FInstalledFilter
{
    TWeakPtr<ISceneOutliner> Outliner;
    TSharedPtr<FSceneOutlinerFilter> Filter;
};
static TArray<FInstalledFilter> Installed;
static FTSTicker::FDelegateHandle TickerHandle;

void InstallOnOutliner(const TSharedRef<ISceneOutliner>& Outliner)
{
    Installed.RemoveAll([](const FInstalledFilter& Entry) { return !Entry.Outliner.IsValid(); });
    if (Installed.ContainsByPredicate([&](const FInstalledFilter& Entry) { return Entry.Outliner.Pin() == Outliner; })) return;
    TSharedRef<FBuildingPartsFilter> Filter = MakeShared<FBuildingPartsFilter>();
    Outliner->AddFilter(Filter);
    Outliner->FullRefresh();
    Installed.Add({Outliner, Filter});
}

static bool RefreshFilters(float)
{
    if (!FModuleManager::Get().IsModuleLoaded("LevelEditor")) return true;
    TSharedPtr<ILevelEditor> Editor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor").GetFirstLevelEditor();
    if (!Editor) return true;
    for (const TWeakPtr<ISceneOutliner>& Weak : Editor->GetAllSceneOutliners())
        if (TSharedPtr<ISceneOutliner> Outliner = Weak.Pin()) InstallOnOutliner(Outliner.ToSharedRef());
    return true;
}

void Start()
{
    if (TickerHandle.IsValid()) return;
    RefreshFilters(0);
    // Detect newly opened Outliner tabs without refreshing existing trees or
    // walking actors every frame. Filtering itself occurs during normal UI work.
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&RefreshFilters), 1.0f);
}

void Stop()
{
    if (TickerHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
    TickerHandle.Reset();
    for (const FInstalledFilter& Entry : Installed)
        if (TSharedPtr<ISceneOutliner> Outliner = Entry.Outliner.Pin()) Outliner->RemoveFilter(Entry.Filter.ToSharedRef());
    Installed.Empty();
}
}
