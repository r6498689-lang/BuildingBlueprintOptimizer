// Included at file scope. Keep replacement, disk backup and validation separate
// from candidate generation. Never replace until the candidate has been checked.
namespace BBCAssetReplacement
{
#if WITH_DEV_AUTOMATION_TESTS
static bool bFailNextSave = false;
void FailNextSaveForTest() { bFailNextSave = true; }
#endif
struct FGeometry
{
    UStaticMesh* Mesh; TArray<UMaterialInterface*> Materials; FTransform Transform;
    bool bVisible, bHidden; ECollisionEnabled::Type Collision;
};
static void Gather(AActor* Actor, TArray<FGeometry>& Out)
{
    TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
    for (auto* M : Meshes) if (M->GetStaticMesh() && !M->IsVisualizationComponent())
    {
        FGeometry G{M->GetStaticMesh(), {}, M->GetComponentTransform(), M->IsVisible(), M->bHiddenInGame, M->GetCollisionEnabled()};
        for (int32 I = 0; I < M->GetNumMaterials(); ++I) G.Materials.Add(M->GetMaterial(I));
        if (auto* ISM = Cast<UInstancedStaticMeshComponent>(M))
            for (int32 I = 0; I < ISM->GetInstanceCount(); ++I) { ISM->GetInstanceTransform(I, G.Transform, true); Out.Add(G); }
        else Out.Add(G);
    }
    TInlineComponentArray<UChildActorComponent*> Children(Actor);
    for (auto* C : Children) if (C->GetChildActor()) Gather(C->GetChildActor(), Out);
}
static bool Same(const TArray<FGeometry>& Before, const TArray<FGeometry>& After)
{
    if (Before.Num() != After.Num()) return false;
    TSet<int32> Matched;
    for (const auto& G : Before)
    {
        int32 Index = INDEX_NONE;
        for (int32 I = 0; I < After.Num(); ++I) if (!Matched.Contains(I))
        {
            const auto& A = After[I];
            if (G.Mesh == A.Mesh && G.Materials == A.Materials && G.Transform.Equals(A.Transform, .001)
                && G.bVisible == A.bVisible && G.bHidden == A.bHidden && G.Collision == A.Collision) { Index = I; break; }
        }
        if (Index == INDEX_NONE) return false;
        Matched.Add(Index);
    }
    return true;
}
static bool Save(UBlueprint* BP, FString& Error)
{
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    BBC::FSaveErrors Errors; Args.Error = &Errors;
    const FString File = FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    if (UPackage::SavePackage(BP->GetOutermost(), BP, *File, Args)) return true;
    Error += TEXT("\n") + FString::Join(Errors.Lines, TEXT("\n")); return false;
}
static void RepairChildReferences(const TArray<FSoftObjectPath>& Paths, UBlueprint* Updated)
{
    // UE's full-asset replacement remaps most references, but ChildActor class
    // and archetype pairs can still point to a retired transient class. Rebuild
    // those pairs through the native setter, templates before world instances.
    for (bool bTemplate : {true, false}) for (const auto& Path : Paths)
        if (auto* C = Cast<UChildActorComponent>(Path.ResolveObject()); IsValid(C) && C->IsTemplate() == bTemplate)
        {
            if (C->GetChildActorClass() != Updated->GeneratedClass
                || (C->GetChildActorTemplate() && C->GetChildActorTemplate()->GetClass() != Updated->GeneratedClass)
                || (!bTemplate && C->IsRegistered() && (!C->GetChildActor() || C->GetChildActor()->GetClass() != Updated->GeneratedClass)))
            {
                C->Modify(); C->SetChildActorClass(Updated->GeneratedClass.Get());
                if (auto* Class = C->GetTypedOuter<UBlueprintGeneratedClass>())
                    if (auto* Owner = Cast<UBlueprint>(Class->ClassGeneratedBy); Owner && Owner != Updated)
                        FBlueprintEditorUtils::MarkBlueprintAsModified(Owner);
            }
        }
}
static void PrepareComponentRollback(UBlueprint* Current, UBlueprint* Snapshot)
{
    // Converted source anchors are SceneComponents; the original generated
    // variables are ChildActorComponents. Do not feed incompatible anchor
    // pointers through the engine's unrelated-object property copier during
    // rollback. Original construction will assign the restored components.
    for (USCS_Node* Node : Snapshot->SimpleConstructionScript->GetAllNodes())
        if (auto* Property = FindFProperty<FObjectPropertyBase>(Current->GeneratedClass, Node->GetVariableName()))
            for (TObjectIterator<AActor> It; It; ++It) if (IsValid(*It) && It->IsA(Current->GeneratedClass))
            {
                UObject* Value = Property->GetObjectPropertyValue_InContainer(*It);
                if (Value && !Value->IsA(Node->ComponentClass)) Property->SetObjectPropertyValue_InContainer(*It, nullptr);
            }
}
static bool Apply(UBlueprint* Source, UBlueprint* Candidate, UWorld* ValidationWorld, FBuildingOptimizationResult& Result)
{
    const FString Path = Source->GetPathName();
    const FString File = FPackageName::LongPackageNameToFilename(Source->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    TArray<FSoftObjectPath> ChildReferences;
    for (TObjectIterator<UChildActorComponent> It; It; ++It)
        if (IsValid(*It) && It->GetChildActorClass() == Source->GeneratedClass && It->GetWorld() != ValidationWorld)
            ChildReferences.Add(FSoftObjectPath(*It));
    struct FPlaced { FSoftObjectPath Path; FSoftObjectPath ParentComponent; FTransform Transform; TArray<FGeometry> Geometry; bool bSelected; };
    TArray<FPlaced> Placed;
    for (TObjectIterator<AActor> It; It; ++It)
        if (IsValid(*It) && !It->IsTemplate() && It->GetWorld() && It->GetWorld() != ValidationWorld
            && It->GetWorld()->WorldType == EWorldType::Editor && It->IsA(Source->GeneratedClass))
        {
            FPlaced State{FSoftObjectPath(*It), FSoftObjectPath(It->GetParentComponent()), It->GetActorTransform(), {}, It->IsSelected()};
            Gather(*It, State.Geometry); Placed.Add(MoveTemp(State));
        }
    for (const auto& State : Placed)
    {
        FActorSpawnParameters Spawn; Spawn.ObjectFlags = RF_Transient;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Prospective = ValidationWorld->SpawnActor<AActor>(Candidate->GeneratedClass, State.Transform, Spawn);
        TArray<FGeometry> Geometry; if (Prospective) Gather(Prospective, Geometry);
        const bool bMatches = Prospective && Same(State.Geometry, Geometry);
        if (Prospective) Prospective->Destroy();
        if (!bMatches)
        {
            Result.Message = TEXT("已加载建筑含单独模型、材质、显示、碰撞或位置覆盖，不能安全替换；原蓝图未改动。请先统一实例设置，或另存副本：\n") + State.Path.ToString();
            return false;
        }
    }
    // A strong temporary blueprint snapshot is needed because ReplaceBlueprint
    // invalidates Source and may trigger GC/reinstancing. It is never registered.
    UPackage* SnapshotPackage = CreatePackage(*(TEXT("/Temp/BBCOriginal_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    TStrongObjectPtr<UBlueprint> Snapshot(DuplicateObject<UBlueprint>(Source, SnapshotPackage, Source->GetFName()));
    UBlueprint* Updated = FKismetEditorUtilities::ReplaceBlueprint(Source, Candidate);
    FString Error;
    bool bValid = IsValid(Updated) && Updated->GetPathName() == Path && Updated->GeneratedClass;
    if (bValid)
    {
        BBCOutliner::MarkBuilding(Updated);
        FKismetEditorUtilities::CompileBlueprint(Updated);
        RepairChildReferences(ChildReferences, Updated);
        bValid = Updated->Status != BS_Error;
        for (const auto& State : Placed)
        {
            auto* Parent = Cast<UChildActorComponent>(State.ParentComponent.ResolveObject());
            auto* Actor = Parent ? Parent->GetChildActor() : Cast<AActor>(State.Path.ResolveObject()); TArray<FGeometry> Now;
            if (IsValid(Actor)) Gather(Actor, Now);
            if (!IsValid(Actor) || !Actor->IsA(Updated->GeneratedClass) || !Actor->GetActorTransform().Equals(State.Transform, .001)
                || !Same(State.Geometry, Now)) { bValid = false; Error += TEXT("\n建筑实例未保持：") + State.Path.ToString(); }
            if (Actor && State.bSelected) GEditor->SelectActor(Actor, true, false);
        }
    }
    if (bValid)
    {
        BBCThumbnails::CacheForSave(Updated);
#if WITH_DEV_AUTOMATION_TESTS
        if (bFailNextSave) { bFailNextSave = false; Error = TEXT("隔离测试模拟保存失败"); bValid = false; }
        else
#endif
        bValid = Save(Updated, Error);
    }
    if (!bValid)
    {
        UBlueprint* Current = LoadObject<UBlueprint>(nullptr, *Path);
        if (Current) PrepareComponentRollback(Current, Snapshot.Get());
        UBlueprint* Restored = Current ? FKismetEditorUtilities::ReplaceBlueprint(Current, Snapshot.Get()) : nullptr;
        if (Restored) BBCOutliner::MarkBuilding(Restored);
        if (Restored) RepairChildReferences(ChildReferences, Restored);
        FString RestoreError;
        bool bRestored = Restored && Save(Restored, RestoreError);
        // Restore the exact original on-disk bytes as well as the in-memory BP.
        const FString Backup = Result.BackupDirectory / FPaths::GetCleanFilename(File);
        if (bRestored)
            for (const FString& Extension : {FString(TEXT(".uasset")), FString(TEXT(".uexp")), FString(TEXT(".ubulk"))})
            {
                const FString Original = FPaths::ChangeExtension(File, Extension);
                const FString Saved = Result.BackupDirectory / FPaths::GetCleanFilename(Original);
                if (IFileManager::Get().FileExists(*Saved)) bRestored &= IFileManager::Get().Copy(*Original, *Saved, true, true) == COPY_OK;
            }
        if (Restored && bRestored) Restored->GetOutermost()->SetDirtyFlag(false);
        Result.Message = TEXT("替换或保存校验失败。") + Error + (bRestored ? TEXT("\n已恢复原蓝图，请检查关卡后保存。")
            : TEXT("\n自动恢复未完成；请停止编辑并从备份恢复原蓝图。") + RestoreError);
        BBC::DiscardPackage(SnapshotPackage); return false;
    }
    Result.Message = FString::Printf(TEXT("已替换并保存原蓝图：%s\nISM：%d 个实例；已加载建筑的模型、材质与位置校验通过。\n原路径和引用保留；请检查后保存受影响关卡与引用蓝图。替换请通过备份恢复，不依赖 Ctrl+Z。"), *Path, Result.ISMInstanceCount);
    BBC::DiscardPackage(SnapshotPackage); return true;
}
}
