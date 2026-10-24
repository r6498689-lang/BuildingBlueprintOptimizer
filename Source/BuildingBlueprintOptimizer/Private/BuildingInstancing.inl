// Included inside namespace BBC, after the single-actor validation helpers.
// Runtime assets contain engine components only; no plugin runtime class.
struct FISMGroup
{
    TArray<int32> Members;
    AActor* Parent = nullptr;
    FName NodeName;
};

static bool IsSharedMeshProperty(const FProperty* Property)
{
    if (!Property->HasAnyPropertyFlags(CPF_Edit)
        || Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)) return false;
    const FName Name = Property->GetFName();
    return Name != TEXT("RelativeLocation") && Name != TEXT("RelativeRotation") && Name != TEXT("RelativeScale3D")
        && Name != TEXT("AssetUserData") && Name != TEXT("AssetUserDataEditorOnly");
}

static bool SameMeshSettings(const UStaticMeshComponent* A, const UStaticMeshComponent* B, FString* Difference = nullptr)
{
    if (A->GetStaticMesh() != B->GetStaticMesh())
    { if (Difference) *Difference = TEXT("StaticMesh"); return false; }
    // Compare every editable base-component setting, including materials, body,
    // shadow, navigation, custom primitive data and culling. Never key by mesh alone.
    for (TFieldIterator<FProperty> It(UStaticMeshComponent::StaticClass()); It; ++It)
        if (IsSharedMeshProperty(*It) && !It->Identical_InContainer(A, B))
        {
            if (Difference) *Difference = It->GetName();
            return false;
        }
    return true;
}

static FString ISMExclusion(AActor* Actor)
{
    if (Actor->GetClass() != AStaticMeshActor::StaticClass()) return FString();
    auto* Mesh = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
    if (!Mesh || !Mesh->GetStaticMesh()) return FString();
    TInlineComponentArray<UActorComponent*> Components(Actor);
    for (UActorComponent* Component : Components)
        if (Component && Component != Mesh && !Component->IsVisualizationComponent()) return TEXT("含额外组件");
    if (Mesh->GetClass() != UStaticMeshComponent::StaticClass()) return TEXT("自定义模型组件");
    for (const UObject* Object : {static_cast<const UObject*>(Actor), static_cast<const UObject*>(Mesh)})
        for (TFieldIterator<FMulticastDelegateProperty> It(Object->GetClass()); It; ++It)
            if (const FMulticastScriptDelegate* Delegate = It->GetMulticastDelegate(It->ContainerPtrToValuePtr<void>(Object)); Delegate && Delegate->IsBound())
            {
                // ChildActorComponent binds its own destruction bookkeeping at
                // spawn. Ignore exactly that engine callback on a copy of the
                // delegate, never clear a user's event or mutate the source.
                FMulticastScriptDelegate Other = *Delegate;
                if (Object == Actor && It->GetFName() == TEXT("OnDestroyed") && Actor->GetParentComponent())
                    Other.Remove(Actor->GetParentComponent(), TEXT("OnChildActorDestroyed"));
                if (Other.IsBound()) return TEXT("已绑定独立事件");
            }
    if (Mesh->Mobility != EComponentMobility::Static || Mesh->BodyInstance.bSimulatePhysics) return TEXT("移动或模拟物理的模型");
    if (Mesh->GetRelativeTransform().GetDeterminant() <= 0) return TEXT("镜像或零缩放实例，保留独立剔除方向");
    if (!Actor->Tags.IsEmpty() || Actor->GetIsReplicated() || Actor->IsReplicatingMovement() || Actor->CustomTimeDilation != 1.f
        || CastChecked<AStaticMeshActor>(Actor)->bStaticMeshReplicateMovement || Actor->IsEditorOnly()) return TEXT("独立标签、复制或编辑器专用状态");
    TArray<AActor*> Children; Actor->GetAttachedActors(Children);
    if (!Children.IsEmpty()) return TEXT("该模型仍控制挂接子级");
    if (Mesh->GetMeshPaintTexture()) return TEXT("实例专属网格绘制贴图");
    for (const FStaticMeshComponentLODInfo& LOD : Mesh->LODData)
        if (LOD.OverrideVertexColors || !LOD.PaintedVertices.IsEmpty()) return TEXT("实例专属顶点绘制数据");
    if (const auto* Data = Mesh->GetAssetUserDataArray())
        for (UAssetUserData* Item : *Data)
            if (Item && Item->GetClass() != UDatasmithAssetUserData::StaticClass()) return TEXT("自定义资产用户数据");
    return FString();
}

static TArray<FISMGroup> PlanISM(const TArray<AActor*>& Actors, const TMap<AActor*, AActor*>& Parents,
    TArray<FString>& Warnings)
{
    TArray<FISMGroup> Groups;
    for (int32 I = 0; I < Actors.Num(); ++I)
    {
        auto* Actor = Cast<AStaticMeshActor>(Actors[I]);
        if (!Actor || Actor->GetClass() != AStaticMeshActor::StaticClass() || !Actor->GetStaticMeshComponent()->GetStaticMesh()) continue;
        FString Reason = ISMExclusion(Actor);
        if (!Reason.IsEmpty()) { Warnings.Add(Label(Actor) + TEXT("：保留原零件，未转 ISM（") + Reason + TEXT("）。")); continue; }
        int32 GroupIndex = Groups.IndexOfByPredicate([&](const FISMGroup& Group)
        {
            AActor* First = Actors[Group.Members[0]];
            return Group.Parent == Parents.FindRef(Actor) && First->GetAttachParentActor() == Actor->GetAttachParentActor()
                && First->IsHidden() == Actor->IsHidden() && First->GetActorEnableCollision() == Actor->GetActorEnableCollision()
                && SameMeshSettings(CastChecked<AStaticMeshActor>(First)->GetStaticMeshComponent(), Actor->GetStaticMeshComponent());
        });
        if (GroupIndex == INDEX_NONE) { FISMGroup Group; Group.Parent = Parents.FindRef(Actor); GroupIndex = Groups.Add(MoveTemp(Group)); }
        Groups[GroupIndex].Members.Add(I);
    }
    Groups.RemoveAll([](const FISMGroup& Group) { return Group.Members.Num() < 2; });
    return Groups;
}

static bool InitializeISM(UInstancedStaticMeshComponent* ISM, AStaticMeshActor* Source, FString& Error)
{
    // Deep-copy through a transient actor, cleaning importer-owned data only on
    // the copy. Copy shared mesh settings into the engine ISM SCS template.
    auto* Staging = NewObject<UChildActorComponent>(GetTransientPackage(), NAME_None, RF_ArchetypeObject);
    Staging->SetChildActorClass(Source->GetClass(), Source);
    AActor* Template = Staging->GetChildActorTemplate();
    if (!Template || !MakeDatasmithDataIndependent(Template, Error)) return false;
    auto* Mesh = CastChecked<AStaticMeshActor>(Template)->GetStaticMeshComponent();
    UEngine::FCopyPropertiesForUnrelatedObjectsParams Params; Params.bDoDelta = false;
    UEngine::CopyPropertiesForUnrelatedObjects(Mesh, ISM, Params);
    ISM->SetupAttachment(nullptr);
    if (FArrayProperty* Children = FindFProperty<FArrayProperty>(USceneComponent::StaticClass(), TEXT("AttachChildren")))
        FScriptArrayHelper(Children, Children->ContainerPtrToValuePtr<void>(ISM)).EmptyValues();
    ISM->RemoveUserDataOfClass(UDatasmithAssetUserData::StaticClass());
    ISM->CreationMethod = EComponentCreationMethod::Native;
    ISM->SetRelativeTransform_Direct(FTransform::Identity);
    ISM->SetMobility(EComponentMobility::Static);
    ISM->SetHiddenInGame(Source->IsHidden() || Mesh->bHiddenInGame);
    if (!Source->GetActorEnableCollision()) ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    return true;
}

static void CopyISMSourceMetadata(AStaticMeshActor* Actor, USceneComponent* Anchor)
{
    auto* Source = Actor->GetStaticMeshComponent();
    Anchor->ComponentTags = Source->ComponentTags;
    if (const auto* Old = Cast<UDatasmithAssetUserData>(Source->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass())))
    {
        auto* Data = NewObject<UDatasmithAssetUserData>(Anchor, NAME_None, RF_Public);
        Data->MetaData = Old->MetaData;
        Data->MetaData.Remove(UDatasmithAssetUserData::UniqueIdMetaDataKey);
        Anchor->AddAssetUserData(Data);
    }
}

static bool ValidateISM(AActor* Building, const TArray<AActor*>& Actors, const TArray<FISMGroup>& Groups,
    FString& Error, int32 ExistingCount = 0)
{
    TInlineComponentArray<UInstancedStaticMeshComponent*> Components(Building);
    if (Components.Num() != Groups.Num() + ExistingCount) { Error = TEXT("生成的 ISM 组件数量不一致。"); return false; }
    for (const FISMGroup& Group : Groups)
    {
        auto* ISM = FindObject<UInstancedStaticMeshComponent>(Building, *Group.NodeName.ToString());
        if (!ISM || ISM->GetClass() != UInstancedStaticMeshComponent::StaticClass() || ISM->GetInstanceCount() != Group.Members.Num())
        { Error = TEXT("生成的 ISM 类型或实例数量不一致。"); return false; }
        auto* First = CastChecked<AStaticMeshActor>(Actors[Group.Members[0]]);
        auto* Expected = DuplicateObject<UStaticMeshComponent>(First->GetStaticMeshComponent(), GetTransientPackage());
        Expected->SetHiddenInGame(First->IsHidden() || Expected->bHiddenInGame);
        if (!First->GetActorEnableCollision()) Expected->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        FString Difference;
        if (!SameMeshSettings(Expected, ISM, &Difference))
        { Error = Label(First) + TEXT("：ISM 模型/材质/组件设置未保留：") + Difference; return false; }
        for (int32 J = 0; J < Group.Members.Num(); ++J)
        {
            auto* Original = CastChecked<AStaticMeshActor>(Actors[Group.Members[J]]);
            auto* Mesh = Original->GetStaticMeshComponent();
            FTransform Actual;
            if (!ISM->GetInstanceTransform(J, Actual, true) || !SameTransform(Mesh->GetComponentTransform(), Actual))
            { Error = Label(Original) + TEXT("：ISM 实例世界变换不一致，已保留原建筑。"); return false; }
        }
    }
    return true;
}
