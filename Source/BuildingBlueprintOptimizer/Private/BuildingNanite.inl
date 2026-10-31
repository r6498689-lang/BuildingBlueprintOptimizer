// Included inside BBC. Mesh assets are shared: this explicitly changes and saves
// project assets only when requested. Never rebuild engine/plugin content.
static bool EnableBuildingNanite(const TArray<AActor*>& Actors, FBuildingOptimizationResult& Result)
{
    TSet<UStaticMesh*> Seen;
    TArray<UStaticMesh*> Pending;
    TArray<FString> Errors;
    for (AActor* Actor : Actors)
    {
        TArray<AActor*> Family; ActorFamily(Actor, Family);
        for (AActor* Member : Family)
        {
            TInlineComponentArray<UStaticMeshComponent*> Components(Member);
            for (UStaticMeshComponent* Component : Components)
            {
                UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
                if (!Mesh || Component->IsVisualizationComponent() || Component->IsEditorOnly()) continue;
                if (Component->IsDisallowNanite()) Result.Warnings.Add(Label(Actor) + TEXT("：组件禁用了 Nanite；开启模型资产不会覆盖该组件设置。"));
                if (Seen.Contains(Mesh)) continue;
                Seen.Add(Mesh);
                if (Mesh->GetNaniteSettings().bEnabled) { ++Result.NaniteAlreadyEnabledCount; continue; }
                const FString Package = Mesh->GetOutermost()->GetName();
                const FString File = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
                if (!Package.StartsWith(TEXT("/Game/")) || !IFileManager::Get().FileExists(*File)
                    || IFileManager::Get().IsReadOnly(*File) || Mesh->GetOutermost()->IsDirty() || !Mesh->IsMeshDescriptionValid(0))
                    Errors.Add(Mesh->GetPathName() + TEXT("：开启 Nanite 需要已保存、可写、可重建的 /Game 模型；引擎/插件模型请先复制到项目并替换引用，或取消此勾选。"));
                else Pending.Add(Mesh);
            }
        }
    }
    if (!Errors.IsEmpty()) { Result.Message = FString::Join(Errors, TEXT("\n")); return false; }
    Result.bNaniteProcessed = true;
    if (Pending.IsEmpty()) return true;
    auto* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
    if (!Subsystem) { Result.Message = TEXT("静态网格编辑子系统不可用，未开启 Nanite。"); return false; }
    FScopedTransaction Transaction(NSLOCTEXT("BuildingBlueprintOptimizer", "EnableBuildingNanite", "为建筑模型开启 Nanite"));
    TArray<FMeshNaniteSettings> Old;
    TArray<UStaticMesh*> Changed;
    FScopedSlowTask Progress(Pending.Num(), NSLOCTEXT("BuildingBlueprintOptimizer", "BuildNanite", "正在逐个构建并保存 Nanite 模型…"));
    Progress.MakeDialog();
    auto Apply = [&](UStaticMesh* Mesh, const FMeshNaniteSettings& Settings)
    {
        TArray<UStaticMesh*> One {Mesh};
        FStaticMeshCompilingManager::FFinishCompilationOptions Options; Options.bIncludeDependentMeshes = true;
        FStaticMeshCompilingManager::Get().FinishCompilation(One, Options);
        Subsystem->SetNaniteSettings(Mesh, Settings, true);
        FStaticMeshCompilingManager::Get().FinishCompilation(One);
    };
    auto Save = [&](UStaticMesh* Mesh, FString& Error)
    {
        const FString File = FPackageName::LongPackageNameToFilename(Mesh->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        FSaveErrors SaveErrors; Args.Error = &SaveErrors;
        const bool bSaved = UPackage::SavePackage(Mesh->GetOutermost(), Mesh, *File, Args);
        if (!bSaved) Error += Mesh->GetPathName() + TEXT("：保存失败。\n") + FString::Join(SaveErrors.Lines, TEXT("\n"));
        if (bSaved)
        {
            auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            // Drop only reconstructible CPU source-description caches. Keep bulk,
            // render/collision data, asset references and transaction history.
            // Multi-LOD/HiRes and open editors are outside the verified scope.
            const bool Safe = !Mesh->IsCompiling() && !Mesh->GetOutermost()->IsDirty()
                && Mesh->GetNumSourceModels() == 1 && !Mesh->IsHiResMeshDescriptionValid()
                && Editors && !Editors->FindEditorForAsset(Mesh, false);
            const FString Id=FGuid::NewGuid().ToString(EGuidFormats::Digits);
            if (Safe)
            {
                BBCMemory::Sample(Id, Mesh->GetPathName(), TEXT("BeforeMeshDescriptionClear"));
                Mesh->ClearMeshDescriptions();
                BBCMemory::Sample(Id, Mesh->GetPathName(), TEXT("AfterMeshDescriptionClear"));
            }
            else BBCMemory::Sample(Id, Mesh->GetPathName(), TEXT("MeshDescriptionClearSkipped"));
        }
        return bSaved;
    };
    for (UStaticMesh* Mesh : Pending)
    {
        Progress.EnterProgressFrame(1, FText::FromString(Mesh->GetName()));
        Old.Add(Mesh->GetNaniteSettings()); Changed.Add(Mesh);
        FMeshNaniteSettings Settings = Mesh->GetNaniteSettings(); Settings.bEnabled = true;
        Apply(Mesh, Settings);
        FString Error;
        const bool bBuilt = Mesh->HasValidNaniteData();
        if (!bBuilt) Error = Mesh->GetPathName() + TEXT("：未能生成有效 Nanite 数据。");
        if (!bBuilt || !Save(Mesh, Error))
        {
            bool bRestored = true; FString RestoreError;
            for (int32 I = Changed.Num() - 1; I >= 0; --I) { Apply(Changed[I], Old[I]); bRestored &= Save(Changed[I], RestoreError); }
            Transaction.Cancel(); Result.NaniteEnabledCount = 0;
            Result.Message = Error + (bRestored ? TEXT("\n已恢复本次修改的模型 Nanite 设置，未创建建筑蓝图。")
                : TEXT("\n部分模型恢复保存失败，请检查以下资源并保存：\n") + RestoreError);
            return false;
        }
        ++Result.NaniteEnabledCount;
    }
    return true;
}
