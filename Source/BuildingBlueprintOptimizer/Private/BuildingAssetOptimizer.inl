// Asset-only optimization using the standalone module's shared validation helpers.
FString UBuildingBlueprintOptimizerLibrary::GetBuildingOptimizationReason(const UBlueprint* Blueprint)
{
    if (!IsValid(Blueprint) || !BBCOutliner::IsGeneratedBlueprint(Blueprint)) return TEXT("请选择建筑蓝图创建器生成或结构兼容的建筑蓝图资产。");
    if (Blueprint->ParentClass != AActor::StaticClass() || !Blueprint->GeneratedClass || !Blueprint->SimpleConstructionScript)
        return TEXT("暂不优化继承建筑或缺少完整组件结构的蓝图。");
    if (Blueprint->GetOutermost()->IsDirty() || Blueprint->Status == BS_Dirty || Blueprint->Status == BS_Error)
        return TEXT("请先编译成功并保存原建筑蓝图，再执行优化。");
    if (!FPackageName::DoesPackageExist(Blueprint->GetOutermost()->GetName())) return TEXT("请先将原建筑蓝图保存到磁盘。");
    if (!Blueprint->NewVariables.IsEmpty() || !Blueprint->Timelines.IsEmpty() || !Blueprint->ImplementedInterfaces.IsEmpty())
        return TEXT("该建筑含自定义变量、时间轴或接口，暂不自动改变其组件类型；原资产保持。");
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (const UEdGraph* Graph : Graphs)
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            if (!Node->IsA<UK2Node_FunctionEntry>() && !Node->IsA<UK2Node_Event>())
                return TEXT("该建筑含自定义蓝图逻辑，暂不自动改变其组件类型；原资产保持。");
            for (const UEdGraphPin* Pin : Node->Pins) if (Pin && !Pin->LinkedTo.IsEmpty())
                return TEXT("该建筑含已连接的蓝图逻辑，暂不自动改变其组件类型；原资产保持。");
        }
    const auto& Roots = Blueprint->SimpleConstructionScript->GetRootNodes();
    if (Roots.Num() != 1 || Roots[0]->GetVariableName() != TEXT("BuildingRoot")) return TEXT("建筑根层级已改变，无法可靠优化。");
    for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        if (!Node || !Node->ComponentTemplate || (Node->ComponentClass != USceneComponent::StaticClass()
            && Node->ComponentClass != UChildActorComponent::StaticClass()
            && Node->ComponentClass != UInstancedStaticMeshComponent::StaticClass()
            && Node->ComponentClass != UHierarchicalInstancedStaticMeshComponent::StaticClass()))
            return TEXT("建筑含额外或自定义组件类型，暂不自动优化；原资产保持。");
    return FString();
}

static FBuildingOptimizationResult OptimizeBuildingAssetInternal(UBlueprint* Source,
    const FString& FolderPath, const FString& DesiredName, const FBuildingOptimizationOptions& Options, const FString& OperationId)
{
    using namespace BBC;
    FBuildingOptimizationResult Result;
    if (!GEditor || GEditor->PlayWorld) { Result.Message = TEXT("请退出运行/模拟模式后优化建筑蓝图。"); return Result; }
    Result.Message = UBuildingBlueprintOptimizerLibrary::GetBuildingOptimizationReason(Source);
    if (!Result.Message.IsEmpty()) return Result;
    if (!Options.bUseISM && !Options.bEnableNanite)
    { Result.Message = TEXT("请至少选择 ISM 或 Nanite 一项优化。"); return Result; }
    const FString SourcePath = Source->GetPathName();
    const FString SourcePackageName = Source->GetOutermost()->GetName();
    const FString SourceFile = FPackageName::LongPackageNameToFilename(SourcePackageName, FPackageName::GetAssetPackageExtension());
    if (Options.bReplaceOriginal && (!SourcePackageName.StartsWith(TEXT("/Game/")) || IFileManager::Get().IsReadOnly(*SourceFile)))
    { Result.Message = TEXT("替换需要已保存且可写的 /Game 原蓝图，未执行优化。"); return Result; }
    FString Folder = Options.bReplaceOriginal ? FPackageName::GetLongPackagePath(SourcePackageName) : FolderPath;
    Folder.RemoveFromEnd(TEXT("/"));
    const FString TargetName = Options.bReplaceOriginal ? Source->GetName() : DesiredName;
    if (!(Folder == TEXT("/Game") || Folder.StartsWith(TEXT("/Game/"))) || TargetName.IsEmpty()
        || !FPackageName::IsValidLongPackageName(Folder / TargetName)
        || !FPackageName::IsValidObjectPath((Folder / TargetName) + TEXT(".") + TargetName))
    { Result.Message = TEXT("请使用 /Game 下的有效目录和蓝图资源名称。"); return Result; }
    FScopedSlowTask Progress(4, NSLOCTEXT("BuildingBlueprintOptimizer", "OptimizeAsset", "正在优化建筑蓝图副本…"));
    Progress.MakeDialog(); Progress.EnterProgressFrame(1);
    // Neither preview instance is placed in the user's level or participates in
    // editor transactions. The original asset and any placed instances stay intact.
    FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false).SetTransactional(false));
    FActorSpawnParameters Spawn; Spawn.ObjectFlags = RF_Transient;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Original = Preview.GetWorld()->SpawnActor<AActor>(Source->GeneratedClass, FTransform::Identity, Spawn);
    if (!Original) { Result.Message = TEXT("无法创建原建筑的临时验证实例。"); return Result; }
    struct FAssetGroup : FISMGroup { USCS_Node* ParentNode = nullptr; };
    TArray<AActor*> Actors;
    TArray<USCS_Node*> SourceNodes;
    TArray<FAssetGroup> Groups;
    USimpleConstructionScript* SourceSCS = Source->SimpleConstructionScript;
    for (USCS_Node* Node : SourceSCS->GetAllNodes())
    {
        if (!Options.bUseISM) continue;
        if (Node->ComponentClass != UChildActorComponent::StaticClass()) continue;
        auto* CAC = FindObject<UChildActorComponent>(Original, *Node->GetVariableName().ToString());
        if (!CAC || !CAC->GetChildActor()) { Result.Message = TEXT("原建筑中有无法实例化的子 Actor：") + Node->GetVariableName().ToString(); return Result; }
        AActor* Actor = CAC->GetChildActor();
        const int32 Index = Actors.Add(Actor); SourceNodes.Add(Node);
        auto* MeshActor = Cast<AStaticMeshActor>(Actor);
        if (!MeshActor || Actor->GetClass() != AStaticMeshActor::StaticClass()) continue;
        if (!MeshActor->GetStaticMeshComponent()->GetStaticMesh())
        { Result.Warnings.Add(Node->GetVariableName().ToString() + TEXT("：静态网格体为空，继续保留节点和子级。")); continue; }
        FString Reason = ISMExclusion(Actor);
        if (!Node->GetChildNodes().IsEmpty()) Reason = TEXT("仍控制挂接子级");
        if (CAC->GetRelativeTransform().GetDeterminant() <= 0) Reason = TEXT("局部镜像或零缩放");
        if (!CAC->IsVisible() || CAC->bHiddenInGame || CAC->IsUsingAbsoluteLocation() || CAC->IsUsingAbsoluteRotation()
            || CAC->IsUsingAbsoluteScale() || !Node->AttachToName.IsNone()
            || (CAC->GetAssetUserDataArray() && !CAC->GetAssetUserDataArray()->IsEmpty())) Reason = TEXT("子 Actor 组件含独立显示、挂接或用户数据设置");
        if (!Reason.IsEmpty()) { Result.Warnings.Add(Node->GetVariableName().ToString() + TEXT("：保留原零件（") + Reason + TEXT("）。")); continue; }
        USCS_Node* Parent = SourceSCS->FindParentNode(Node);
        int32 Match = INDEX_NONE;
        for (int32 I = 0; I < Groups.Num(); ++I)
        {
            auto* First = CastChecked<AStaticMeshActor>(Actors[Groups[I].Members[0]]);
            if (Groups[I].ParentNode == Parent && First->IsHidden() == Actor->IsHidden()
                && First->GetActorEnableCollision() == Actor->GetActorEnableCollision()
                && SameMeshSettings(First->GetStaticMeshComponent(), MeshActor->GetStaticMeshComponent())) { Match = I; break; }
        }
        if (Match == INDEX_NONE) { FAssetGroup Group; Group.ParentNode = Parent; Match = Groups.Add(MoveTemp(Group)); }
        Groups[Match].Members.Add(Index);
    }
    Groups.RemoveAll([](const FAssetGroup& Group) { return Group.Members.Num() < 2; });
    if (Groups.IsEmpty() && !Options.bEnableNanite)
    {
        Result.Message = TEXT("没有至少两个同父级且设置兼容的重复零件，未生成副本；已有 ISM 和嵌套蓝图保持原样。");
        if (!Result.Warnings.IsEmpty()) Result.Message += TEXT("\n\n") + FString::Join(Result.Warnings, TEXT("\n"));
        return Result;
    }
    if (Options.bReplaceOriginal)
    {
        Result.BackupDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintOptimizer/AssetBackups") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
        if (!IFileManager::Get().MakeDirectory(*Result.BackupDirectory, true))
        { Result.Message = TEXT("无法创建原蓝图备份目录，未执行优化。"); return Result; }
        for (const FString& Extension : {FString(TEXT(".uasset")), FString(TEXT(".uexp")), FString(TEXT(".ubulk"))})
        {
            const FString File = FPaths::ChangeExtension(SourceFile, Extension);
            if (IFileManager::Get().FileExists(*File) && IFileManager::Get().Copy(*(Result.BackupDirectory / FPaths::GetCleanFilename(File)), *File) != COPY_OK)
            { Result.Message = TEXT("原蓝图备份失败，未执行优化。"); return Result; }
            if (IFileManager::Get().FileExists(*File))
            {
                TArray<uint8> A, B;
                if (!FFileHelper::LoadFileToArray(A, *File) || !FFileHelper::LoadFileToArray(B, *(Result.BackupDirectory / FPaths::GetCleanFilename(File))) || A != B)
                { Result.Message = TEXT("原蓝图备份字节校验失败，未执行优化。"); return Result; }
            }
        }
        if (!BBCBackups::Record(Result.BackupDirectory, SourcePath, Result.Message)) return Result;
    }
    if (Options.bEnableNanite && !EnableBuildingNanite({Original}, Result)) return Result;
    BBCMemory::Sample(OperationId,SourcePath,TEXT("AfterNaniteStage"));
    if (Options.bReplaceOriginal && Groups.IsEmpty())
    {
        Result.bSuccess = true; Result.AssetObjectPath = SourcePath;
        Result.Message = TEXT("Nanite 处理完成；没有需要转换的 ISM 零件，原蓝图路径、结构和引用保持。");
        return Result;
    }
    Progress.EnterProgressFrame(1);
    const FString Name = Options.bReplaceOriginal ? Source->GetName() : UBuildingBlueprintOptimizerLibrary::FindAvailableAssetName(Folder, TargetName);
    if (Name.IsEmpty()) { Result.Message = TEXT("无法取得可用副本名称。"); return Result; }
    const FString PackageName = Options.bReplaceOriginal ? TEXT("/Temp/BBCOptimization_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) / Name : Folder / Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    UPackage* Package = CreatePackage(*PackageName);
    UBlueprint* Copy = DuplicateObject<UBlueprint>(Source, Package, FName(*Name));
    if (!Copy || !Copy->GeneratedClass || Copy->GeneratedClass == Source->GeneratedClass
        || Copy->SimpleConstructionScript == SourceSCS)
    { DiscardPackage(Package); Result.Message = TEXT("无法创建独立的建筑蓝图副本。"); return Result; }
    for (const USCS_Node* Node : Copy->SimpleConstructionScript->GetAllNodes())
        if (!Node || !Node->ComponentTemplate || Node->ComponentTemplate->GetOutermost() != Package)
        { DiscardPackage(Package); Result.Message = TEXT("副本组件模板未独立，已停止；原资产保持。"); return Result; }
    Copy->SetFlags(RF_Public | RF_Standalone); BBCOutliner::MarkBuilding(Copy);
    USimpleConstructionScript* SCS = Copy->SimpleConstructionScript;
    FActorState Expected = Capture(Original);
    TArray<FISMGroup> ValidationGroups;
    TSet<FName> ConvertedNames;
    TInlineComponentArray<UInstancedStaticMeshComponent*> ExistingISMs(Original);
    for (FAssetGroup& Group : Groups)
    {
        USCS_Node* Parent = Group.ParentNode ? SCS->FindSCSNode(Group.ParentNode->GetVariableName()) : SCS->GetRootNodes()[0];
        USCS_Node* HNode = SCS->CreateNode(UInstancedStaticMeshComponent::StaticClass(), TEXT("ISM_Optimized"));
        auto* ISM = CastChecked<UInstancedStaticMeshComponent>(HNode->ComponentTemplate);
        FString Error;
        if (!Parent || !InitializeISM(ISM, CastChecked<AStaticMeshActor>(Actors[Group.Members[0]]), Error))
        { DiscardPackage(Package); Result.Message = TEXT("创建 ISM 副本组件失败：") + Error; return Result; }
        Parent->AddChildNode(HNode); Group.NodeName = HNode->GetVariableName();
        for (int32 J = 0; J < Group.Members.Num(); ++J)
        {
            const int32 Index = Group.Members[J]; const FName Variable = SourceNodes[Index]->GetVariableName();
            USCS_Node* Node = SCS->FindSCSNode(Variable);
            auto* SourceComponent = FindObject<UChildActorComponent>(Original, *Variable.ToString());
            ISM->AddInstance(SourceComponent->GetRelativeTransform());
            auto* Old = CastChecked<UChildActorComponent>(Node->ComponentTemplate);
            const FString TemplateName = Old->GetName(); UObject* Outer = Old->GetOuter();
            Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
            TArray<UObject*> Retired; GetObjectsWithOuter(Old, Retired, true); Retired.Add(Old);
            for (UObject* Object : Retired) { Object->ClearFlags(RF_Public | RF_Standalone); Object->SetFlags(RF_Transient); }
            auto* Anchor = NewObject<USceneComponent>(Outer, FName(*TemplateName), RF_ArchetypeObject | RF_Public | RF_Transactional);
            Anchor->SetMobility(Old->Mobility); Anchor->SetRelativeTransform_Direct(Old->GetRelativeTransform());
            CopyISMSourceMetadata(CastChecked<AStaticMeshActor>(Actors[Index]), Anchor);
            for (FName Tag : Old->ComponentTags) Anchor->ComponentTags.AddUnique(Tag);
            Anchor->ComponentTags.Add(FName(*FString::Printf(TEXT("BBC.ISM=%s;Instance=%d"), *Group.NodeName.ToString(), J)));
            Node->ComponentClass = USceneComponent::StaticClass(); Node->ComponentTemplate = Anchor;
            ConvertedNames.Add(Variable);
            Expected.Children.Remove(Variable.ToString()); Expected.Values.Remove(Variable.ToString());
            ++Result.ISMInstanceCount;
        }
        ValidationGroups.Add(Group);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Copy);
    FKismetEditorUtilities::CompileBlueprint(Copy);
    BBCMemory::Sample(OperationId,SourcePath,TEXT("AfterCandidateCompile"));
    if (Copy->Status == BS_Error || !Copy->GeneratedClass)
    { DiscardPackage(Package); Result.Message = TEXT("优化副本编译失败，未保存；原蓝图保持。"); return Result; }
    Progress.EnterProgressFrame(1);
    AActor* Optimized = Preview.GetWorld()->SpawnActor<AActor>(Copy->GeneratedClass, FTransform::Identity, Spawn);
    FString Error;
    bool bValid = Optimized != nullptr;
    if (bValid)
    {
        FActorState Actual = Capture(Optimized); Expected.Class = Actual.Class;
        for (FName Variable : ConvertedNames) Actual.Values.Remove(Variable.ToString());
        for (const auto& Group : ValidationGroups) { Actual.Meshes.Remove(Group.NodeName.ToString()); Actual.Values.Remove(Group.NodeName.ToString()); }
        bValid = Compare(Expected, Actual, Source->GetName(), Error)
            && ValidateISM(Optimized, Actors, ValidationGroups, Error, ExistingISMs.Num());
        for (USCS_Node* Node : SourceSCS->GetAllNodes())
        {
            auto* Before = FindObject<USceneComponent>(Original, *Node->GetVariableName().ToString());
            auto* After = FindObject<USceneComponent>(Optimized, *Node->GetVariableName().ToString());
            const FName BeforeParent = Before && Before->GetAttachParent() ? Before->GetAttachParent()->GetFName() : NAME_None;
            const FName AfterParent = After && After->GetAttachParent() ? After->GetAttachParent()->GetFName() : NAME_None;
            if (!Before || !After || BeforeParent != AfterParent || !SameTransform(Before->GetComponentTransform(), After->GetComponentTransform()))
            { bValid = false; Error += TEXT("\n组件层级或变换未保留：") + Node->GetVariableName().ToString(); }
        }
    }
    if (!bValid) { DiscardPackage(Package); Result.Message = TEXT("优化副本验证失败，未保存；原蓝图保持。\n") + Error; return Result; }
    BBCMemory::Sample(OperationId,SourcePath,TEXT("AfterValidation"));
    Progress.EnterProgressFrame(1);
    if (Options.bReplaceOriginal)
    {
        // Reinstancing deliberately uses the engine API; it remaps blueprint,
        // generated-class and placed-actor references instead of renaming assets.
        const bool bApplied = BBCAssetReplacement::Apply(Source, Copy, Preview.GetWorld(), Result);
        BBCMemory::Sample(OperationId,SourcePath,TEXT("AfterReplacementAndSave"));
        DiscardPackage(Package);
        Result.bSuccess = bApplied;
        if (bApplied) Result.AssetObjectPath = SourcePath;
        Result.ActorCount = Actors.Num(); Result.ISMGroupCount = Groups.Num();
        Result.HISMGroupCount = Result.ISMGroupCount; Result.HISMInstanceCount = Result.ISMInstanceCount;
        if (!Result.Warnings.IsEmpty()) Result.Message += TEXT("\n\n提示：\n") + FString::Join(Result.Warnings, TEXT("\n"));
        return Result;
    }
    if (IFileManager::Get().FileExists(*Filename))
    { DiscardPackage(Package); Result.Message = TEXT("目标文件已存在，请重试；不会覆盖原资源。"); return Result; }
    BBCThumbnails::CacheForSave(Copy);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    FSaveErrors SaveErrors; Args.Error = &SaveErrors;
    if (!UPackage::SavePackage(Package, Copy, *Filename, Args))
    {
        if (IFileManager::Get().FileExists(*Filename)) IFileManager::Get().Delete(*Filename);
        DiscardPackage(Package); Result.Message = TEXT("优化副本保存失败，原蓝图保持。\n") + FString::Join(SaveErrors.Lines, TEXT("\n")); return Result;
    }
    FAssetRegistryModule::AssetCreated(Copy);
    BBCMemory::Sample(OperationId,SourcePath,TEXT("AfterCopySave"));
    Result.bSuccess = true; Result.AssetObjectPath = Copy->GetPathName(); Result.ActorCount = Actors.Num(); Result.ISMGroupCount = Groups.Num();
    Result.HISMGroupCount = Result.ISMGroupCount; Result.HISMInstanceCount = Result.ISMInstanceCount;
    Result.Message = FString::Printf(TEXT("已保存优化副本：%s\n本次新增 ISM：%d 个组件，%d 个实例。\n原蓝图和关卡中已有建筑保持；副本保留原点和父级层级。"),
        *Result.AssetObjectPath, Result.ISMGroupCount, Result.ISMInstanceCount);
    if (!Result.Warnings.IsEmpty()) Result.Message += TEXT("\n\n提示：\n") + FString::Join(Result.Warnings, TEXT("\n"));
    return Result;
}

FBuildingOptimizationResult UBuildingBlueprintOptimizerLibrary::OptimizeBuildingBlueprintWithOptions(UBlueprint* Source,
    const FString& FolderPath, const FString& AssetName, const FBuildingOptimizationOptions& Options)
{
    const FString SourcePath=GetPathNameSafe(Source);
    const FString OperationId=BBCMemory::Begin(SourcePath);
    FBuildingOptimizationResult Result = OptimizeBuildingAssetInternal(Source, FolderPath, AssetName, Options, OperationId);
    BBCMemory::End(OperationId,SourcePath);
    if (Options.bEnableNanite && Result.bNaniteProcessed)
    {
        Result.Message += FString::Printf(TEXT("\nNanite 原模型：本次开启并保存 %d 个，原本开启 %d 个。"), Result.NaniteEnabledCount, Result.NaniteAlreadyEnabledCount);
        if (Result.NaniteEnabledCount) Result.Message += TEXT("\n模型设置影响所有引用，有独立撤销步骤；蓝图操作失败不会自动撤销已保存的 Nanite 修改。");
        if (!Options.bReplaceOriginal && IsValid(Source) && Source->GetOutermost()->IsDirty())
            Result.Message += TEXT("\nUE 重建模型后将原蓝图标记为待保存；本次未写入原蓝图文件，请检查后按需保存。");
    }
    if (!Result.BackupDirectory.IsEmpty()) Result.Message += TEXT("\n原蓝图备份：") + Result.BackupDirectory;
    Result.Message += TEXT("\n内存记录：") + BBCMemory::File();
    return Result;
}

FBuildingOptimizationResult UBuildingBlueprintOptimizerLibrary::OptimizeBuildingBlueprint(UBlueprint* Source,
    const FString& FolderPath, const FString& AssetName)
{
    return OptimizeBuildingBlueprintWithOptions(Source, FolderPath, AssetName, FBuildingOptimizationOptions());
}
