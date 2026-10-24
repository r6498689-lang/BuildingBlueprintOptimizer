#include "BuildingBlueprintOptimizerLibrary.h"
#include "BuildingBackupManager.h"
#include "BuildingOptimizationMemory.h"
#include "BuildingOutlinerVisibility.h"
#include "BuildingOptimizerThumbnailRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshCompiler.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "DatasmithAssetUserData.h"
#include "DatasmithSceneActor.h"
#include "Editor.h"
#include "Editor/GroupActor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/Selection.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "LevelUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/OutputDevice.h"
#include "Misc/FeedbackContext.h"
#include "Materials/MaterialInterface.h"
#include "ObjectTools.h"
#include "PreviewScene.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "BuildingBlueprintOptimizer"

namespace BBC
{
// These two fields belong to Datasmith's importer, not to the reusable building.
// Never exempt another property merely because it has the same display name.
static bool IsDatasmithRelatedActors(UObject* Object, const FProperty* Property)
{
    return Object->IsA<ADatasmithSceneActor>()
        && Property == FindFProperty<FProperty>(ADatasmithSceneActor::StaticClass(), TEXT("RelatedActors"));
}

static bool MakeDatasmithDataIndependent(AActor* Template, FString& Error)
{
    Template->GroupActor = nullptr;
    // Work on a newly created template only. A clean metadata object also avoids
    // keeping a map component as an archetype through an instanced importer object.
    TArray<UObject*> Objects;
    GetObjectsWithOuter(Template, Objects, true);
    for (UObject* Object : Objects)
    {
        if (AActor* Member = Cast<AActor>(Object)) Member->GroupActor = nullptr;
        UActorComponent* Component = Cast<UActorComponent>(Object);
        if (!Component) continue;
        UDatasmithAssetUserData* Old = Cast<UDatasmithAssetUserData>(
            Component->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass()));
        if (!Old) continue;
        if (!Old->IsIn(Template) || Old->GetClass() != UDatasmithAssetUserData::StaticClass())
        {
            Error = TEXT("Datasmith 元数据不是独立副本或使用了自定义类型，已停止以避免修改原场景或丢失自定义数据。");
            return false;
        }
        UDatasmithAssetUserData* Clean = NewObject<UDatasmithAssetUserData>(Component, NAME_None, RF_Public);
        Clean->MetaData = Old->MetaData;
        Clean->MetaData.Remove(UDatasmithAssetUserData::UniqueIdMetaDataKey);
        Component->RemoveUserDataOfClass(UDatasmithAssetUserData::StaticClass());
        Component->AddAssetUserData(Clean);
        // Only retire objects owned by the copied metadata. Template values may
        // reference original map objects, which must never be modified here.
        TArray<UObject*> Retired;
        GetObjectsWithOuter(Old, Retired, true);
        Retired.Add(Old);
        for (UObject* Item : Retired)
        {
            Item->ClearFlags(RF_Public | RF_Standalone);
            Item->SetFlags(RF_Transient);
        }
        Old->ObjectTemplates.Reset();
    }
    return true;
}

class FSaveErrors final : public FOutputDevice
{
public:
    TArray<FString> Lines;
    virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (Verbosity <= ELogVerbosity::Warning && Lines.Num() < 8) Lines.Add(FString(Text).Left(1600));
        if (GWarn) GWarn->Serialize(Text, Verbosity, Category);
    }
};

static bool IsBlueprintActor(const AActor* Actor)
{
    return Actor && Cast<UBlueprintGeneratedClass>(Actor->GetClass()) != nullptr;
}

static bool IsHierarchyActor(const AActor* Actor)
{
    // Names such as *_grp are not evidence of a harmless container. Restrict
    // this path to exact native classes with transform-only components.
    if (!Actor || (Actor->GetClass() != AActor::StaticClass() && Actor->GetClass() != AGroupActor::StaticClass())
        || !Actor->GetRootComponent()) return false;
    TInlineComponentArray<UActorComponent*> Components(Actor);
    for (const UActorComponent* Component : Components)
        if (Component && !Component->IsVisualizationComponent())
        {
            if (Component->GetClass() != USceneComponent::StaticClass()) return false;
            // A custom editor group is not necessarily a transform-only wrapper.
            if (Actor->IsA<AGroupActor>() && (Component != Actor->GetRootComponent()
                || (Component->GetAssetUserDataArray() && !Component->GetAssetUserDataArray()->IsEmpty()))) return false;
        }
    return true;
}

static bool IsEmptyMeshNode(const AActor* Actor)
{
    if (!Actor || Actor->GetClass() != AStaticMeshActor::StaticClass()) return false;
    auto* Mesh = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
    if (!Mesh || Mesh->GetStaticMesh()) return false;
    TInlineComponentArray<UActorComponent*> Components(Actor);
    for (const UActorComponent* Component : Components)
        if (Component && Component != Mesh && !Component->IsVisualizationComponent()) return false;
    return true;
}

static TArray<AActor*> GatherActors(const TArray<AActor*>& Input, bool bIncludeAttached)
{
    TArray<AActor*> Result, Queue = Input;
    TSet<AActor*> Visited;
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        AActor* Actor = Queue[Index];
        if (!IsValid(Actor) || Visited.Contains(Actor)) continue;
        Visited.Add(Actor);
        if (!Actor->IsChildActor() || Input.Contains(Actor)) Result.Add(Actor);
        // Selecting a native editor group always means selecting its members.
        // Group membership is independent of the attachment hierarchy.
        if (AGroupActor* Group = Cast<AGroupActor>(Actor))
        {
            TArray<AActor*> Members;
            Group->GetAllChildren(Members, false);
            Queue.Append(Members);
        }
        if (bIncludeAttached)
        {
            TArray<AActor*> Attached;
            Actor->GetAttachedActors(Attached);
            Queue.Append(Attached);
        }
    }
    return Result;
}

static AActor* GroupAsRuntimeTemplate(AActor* Source)
{
    // GroupActor is editor-only. Represent it with an ordinary Actor containing
    // a clean scene root, rather than serializing editor membership references.
    AActor* Template = NewObject<AActor>(GetTransientPackage(), AActor::StaticClass(), NAME_None, RF_ArchetypeObject | RF_Public);
    USceneComponent* Scene = NewObject<USceneComponent>(Template, TEXT("GroupRoot"), RF_Public | RF_Transactional);
    Template->AddInstanceComponent(Scene);
    Template->SetRootComponent(Scene);
    // Editor groups have movable roots even when all members are static.
    // Their runtime wrapper must permit static members to attach beneath it.
    Scene->SetMobility(EComponentMobility::Static);
    Scene->SetRelativeTransform_Direct(Source->GetActorTransform());
    Template->Tags = Source->Tags;
    Template->SetActorHiddenInGame(Source->IsHidden());
    Template->SetActorEnableCollision(Source->GetActorEnableCollision());
    return Template;
}

static FString Label(const AActor* Actor)
{
    return Actor ? Actor->GetActorLabel() : TEXT("<已失效 Actor>");
}

// Deliberately inspect serialized, user-facing properties, not infrastructure
// references such as Owner/AttachParent/WorldPrivate. Owner is checked separately.
static bool IsSavedUserProperty(const FProperty* Property)
{
    return !Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated)
        && (Property->HasAnyPropertyFlags(CPF_Edit) || Cast<UBlueprintGeneratedClass>(Property->GetOwnerClass()));
}

static void VisitValueReferences(const FProperty* Property, const void* Value,
    const FString& Path, TFunctionRef<void(UObject*, const FString&, const FString&)> Visit, int32 Depth = 0)
{
    if (!Value || Depth > 32) return;
    if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
    {
        const FSoftObjectPtr& Ref = *static_cast<const FSoftObjectPtr*>(Value);
        Visit(Ref.Get(), Path, Ref.ToSoftObjectPath().ToString());
    }
    else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
    {
        Visit(Object->GetObjectPropertyValue(Value), Path, FString());
    }
    else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
    {
        // A soft path stored as a struct is also a level dependency.
        if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
        {
            const FSoftObjectPath& Ref = *static_cast<const FSoftObjectPath*>(Value);
            Visit(Ref.ResolveObject(), Path, Ref.ToString());
        }
        else for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
        {
            for (int32 I = 0; I < It->ArrayDim; ++I)
                VisitValueReferences(*It, It->ContainerPtrToValuePtr<void>(Value, I), Path + TEXT(".") + It->GetName(), Visit, Depth + 1);
        }
    }
    else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(Array, Value);
        for (int32 I = 0; I < Helper.Num(); ++I)
            VisitValueReferences(Array->Inner, Helper.GetRawPtr(I), Path + FString::Printf(TEXT("[%d]"), I), Visit, Depth + 1);
    }
    else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Helper(Set, Value);
        for (int32 I = 0; I < Helper.GetMaxIndex(); ++I) if (Helper.IsValidIndex(I))
            VisitValueReferences(Set->ElementProp, Helper.GetElementPtr(I), Path, Visit, Depth + 1);
    }
    else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Helper(Map, Value);
        for (int32 I = 0; I < Helper.GetMaxIndex(); ++I) if (Helper.IsValidIndex(I))
        {
            VisitValueReferences(Map->KeyProp, Helper.GetKeyPtr(I), Path + TEXT(".Key"), Visit, Depth + 1);
            VisitValueReferences(Map->ValueProp, Helper.GetValuePtr(I), Path + TEXT(".Value"), Visit, Depth + 1);
        }
    }
}

static void VisitReferences(UObject* Object, TFunctionRef<void(UObject*, const FString&, const FString&)> Visit,
    bool bSkipDatasmithManagement = false)
{
    for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It) if (IsSavedUserProperty(*It))
    {
        if (bSkipDatasmithManagement && IsDatasmithRelatedActors(Object, *It)) continue;
        for (int32 I = 0; I < It->ArrayDim; ++I)
            VisitValueReferences(*It, It->ContainerPtrToValuePtr<void>(Object, I), Object->GetName() + TEXT(".") + It->GetName(), Visit);
    }
}

static AActor* OwningActor(UObject* Object)
{
    if (AActor* Actor = Cast<AActor>(Object)) return Actor;
    if (UActorComponent* Component = Cast<UActorComponent>(Object)) return Component->GetOwner();
    return Object ? Object->GetTypedOuter<AActor>() : nullptr;
}

static void ActorFamily(AActor* Actor, TArray<AActor*>& Out)
{
    if (!Actor || Out.Contains(Actor)) return;
    Out.Add(Actor);
    TInlineComponentArray<UChildActorComponent*> Components(Actor);
    for (UChildActorComponent* Component : Components) ActorFamily(Component->GetChildActor(), Out);
}

static bool IsLevelReference(UObject* Object, const FString& SoftPath)
{
    return (Object && (OwningActor(Object) || Object->IsA<UWorld>() || Object->IsA<ULevel>()))
        || SoftPath.Contains(TEXT(":PersistentLevel."));
}

static void CheckSavedAsset(UObject* Object, const FString& Context, TSet<UPackage*>& Checked, TArray<FString>& Errors)
{
    if (!Object) return;
    if (UClass* Class = Cast<UClass>(Object)) Object = Class->ClassGeneratedBy;
    if (!Object) return;
    if (Object->IsA<UMaterialInterface>() && !Object->IsAsset())
    {
        Errors.Add(Context + TEXT(": 使用了动态/临时材质，无法作为可复用资产保存：") + Object->GetName()); return;
    }
    if (!Object->IsAsset()) return;
    UPackage* Package = Object->GetOutermost();
    if (Package->HasAnyPackageFlags(PKG_CompiledIn) || Checked.Contains(Package)) return;
    Checked.Add(Package);
    if (Package->IsDirty() || Object->HasAnyFlags(RF_Transient) || !FPackageName::DoesPackageExist(Package->GetName()))
        Errors.Add(Context + TEXT(": 请先保存引用的资源，再创建建筑蓝图：") + Package->GetName());
}

static void CheckReferences(AActor* Actor, TSet<UPackage*>& CheckedPackages, TArray<FString>& Errors)
{
    TArray<AActor*> Family;
    ActorFamily(Actor, Family);
    for (AActor* Member : Family)
    {
        CheckSavedAsset(Member->GetClass(), Label(Actor), CheckedPackages, Errors);
        if (Member->GetOwner() && !Family.Contains(Member->GetOwner()))
            Errors.Add(Label(Actor) + TEXT(": Owner 引用了关卡中的其他 Actor，无法可靠保存。"));
        auto Check = [&](UObject* Target, const FString& Path, const FString& SoftPath)
        {
            CheckSavedAsset(Target, Label(Actor) + TEXT(": ") + Path, CheckedPackages, Errors);
            if (!IsLevelReference(Target, SoftPath)) return;
            AActor* TargetActor = OwningActor(Target);
            if (TargetActor && Family.Contains(TargetActor) && SoftPath.IsEmpty()) return;
            Errors.Add(Label(Actor) + TEXT(": ") + Path + TEXT(" 引用了关卡对象 ")
                + (TargetActor ? Label(TargetActor) : SoftPath) + TEXT("。暂不支持跨 Actor 引用重绑定；请先解除此引用。"));
        };
        VisitReferences(Member, Check);
        TInlineComponentArray<UActorComponent*> Components(Member);
        for (UActorComponent* Component : Components) if (Component && !Component->IsVisualizationComponent())
            VisitReferences(Component, Check);
    }
}

struct FMeshState
{
    UClass* Class = nullptr;
    UStaticMesh* Mesh = nullptr;
    TArray<UMaterialInterface*> Materials;
    FTransform Transform;
    TArray<FTransform> Instances;
    FCollisionResponseContainer Responses;
    ECollisionEnabled::Type Collision = ECollisionEnabled::NoCollision;
    EComponentMobility::Type Mobility = EComponentMobility::Static;
    bool bVisible = false, bHidden = false, bShadow = false;
};

struct FActorState
{
    UClass* Class = nullptr;
    TMap<FString, FString> Values;
    TMap<FString, FMeshState> Meshes;
    TMap<FString, TSharedPtr<FActorState>> Children;
    TArray<FName> Tags;
    bool bHidden = false, bCollision = false;
};

static FActorState Capture(AActor* Actor)
{
    FActorState Result;
    Result.Class = Actor->GetClass();
    Result.Tags = Actor->Tags;
    Result.bHidden = Actor->IsHidden();
    Result.bCollision = Actor->GetActorEnableCollision();
    // All serialized BP variables, including inherited BP instance variables.
    for (TFieldIterator<FProperty> It(Actor->GetClass()); It; ++It)
    {
        if (!Cast<UBlueprintGeneratedClass>(It->GetOwnerClass()) || !IsSavedUserProperty(*It)) continue;
        FString Value;
        It->ExportText_InContainer(0, Value, Actor, nullptr, Actor, PPF_None);
        Value.ReplaceInline(*Actor->GetPathName(), TEXT("#SELF"));
        Result.Values.Add(It->GetName(), MoveTemp(Value));
    }
    TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
    for (UStaticMeshComponent* Mesh : Meshes)
    {
        if (!Mesh || Mesh->IsVisualizationComponent() || Mesh->IsEditorOnly()) continue;
        FMeshState State;
        State.Class = Mesh->GetClass(); State.Mesh = Mesh->GetStaticMesh(); State.Transform = Mesh->GetComponentTransform();
        State.Collision = Mesh->GetCollisionEnabled(); State.Responses = Mesh->GetCollisionResponseToChannels();
        State.Mobility = Mesh->Mobility; State.bVisible = Mesh->IsVisible(); State.bHidden = Mesh->bHiddenInGame; State.bShadow = Mesh->CastShadow;
        for (int32 I = 0; I < Mesh->GetNumMaterials(); ++I) State.Materials.Add(Mesh->GetMaterial(I));
        if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Mesh))
            for (int32 I = 0; I < ISM->GetInstanceCount(); ++I)
            {
                FTransform Transform; ISM->GetInstanceTransform(I, Transform, true); State.Instances.Add(Transform);
            }
        Result.Meshes.Add(Mesh->GetName(), MoveTemp(State));
    }
    TInlineComponentArray<UChildActorComponent*> Children(Actor);
    for (UChildActorComponent* Component : Children) if (AActor* Child = Component->GetChildActor())
        Result.Children.Add(Component->GetName(), MakeShared<FActorState>(Capture(Child)));
    return Result;
}

static bool SameTransform(const FTransform& A, const FTransform& B)
{
    return A.GetLocation().Equals(B.GetLocation(), 0.05) && A.GetScale3D().Equals(B.GetScale3D(), 0.0001)
        && A.GetRotation().Equals(B.GetRotation(), 0.0001);
}

static bool Compare(const FActorState& Before, const FActorState& After, const FString& Path, FString& Error)
{
    auto Fail = [&](const FString& Detail) { Error = Path + TEXT(": ") + Detail; return false; };
    if (Before.Class != After.Class || Before.Tags != After.Tags || Before.bHidden != After.bHidden || Before.bCollision != After.bCollision)
        return Fail(TEXT("Actor 类型、标签、可见性或碰撞状态与原对象不一致。"));
    if (Before.Values.Num() != After.Values.Num()) return Fail(TEXT("蓝图实例参数数量发生变化。"));
    for (const auto& Pair : Before.Values)
    {
        const FString* Other = After.Values.Find(Pair.Key);
        if (!Other || Pair.Value != *Other) return Fail(TEXT("蓝图实例参数未能保留：") + Pair.Key);
    }
    if (Before.Meshes.Num() != After.Meshes.Num() || Before.Children.Num() != After.Children.Num())
        return Fail(TEXT("生成后的模型或嵌套蓝图数量与原对象不一致；请检查构造脚本或实例添加的组件。"));
    for (const auto& Pair : Before.Meshes)
    {
        const FMeshState* Other = After.Meshes.Find(Pair.Key);
        const FMeshState& Original = Pair.Value;
        if (!Other || Original.Class != Other->Class || Original.Mesh != Other->Mesh || Original.Materials != Other->Materials)
            return Fail(TEXT("模型或材质未能完整保留：") + Pair.Key);
        if (!SameTransform(Original.Transform, Other->Transform)) return Fail(TEXT("模型的世界位置、旋转或缩放发生变化：") + Pair.Key
            + TEXT("\n原始：") + Original.Transform.ToString() + TEXT("\n生成：") + Other->Transform.ToString());
        if (Original.Mobility != Other->Mobility || Original.Collision != Other->Collision || Original.Responses != Other->Responses
            || Original.bVisible != Other->bVisible || Original.bHidden != Other->bHidden || Original.bShadow != Other->bShadow)
            return Fail(TEXT("模型的移动性、碰撞、可见性或投影设置发生变化：") + Pair.Key);
        if (Original.Instances.Num() != Other->Instances.Num()) return Fail(TEXT("实例化网格体数量发生变化：") + Pair.Key);
        for (int32 I = 0; I < Original.Instances.Num(); ++I)
            if (!SameTransform(Original.Instances[I], Other->Instances[I])) return Fail(TEXT("实例化网格体变换发生变化：") + Pair.Key);
    }
    for (const auto& Pair : Before.Children)
    {
        const TSharedPtr<FActorState>* Other = After.Children.Find(Pair.Key);
        if (!Other) return Fail(TEXT("嵌套蓝图丢失：") + Pair.Key);
        if (!Compare(*Pair.Value, **Other, Path + TEXT("/") + Pair.Key, Error)) return false;
    }
    return true;
}

static void DiscardPackage(UPackage* Package)
{
    ForEachObjectWithPackage(Package, [](UObject* Object)
    {
        Object->ClearFlags(RF_Public | RF_Standalone);
        Object->SetFlags(RF_Transient);
        return true;
    }, true);
    Package->SetDirtyFlag(false);
    Package->Rename(*FString::Printf(TEXT("/Temp/RejectedBuilding_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)), nullptr,
        REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
}

#include "BuildingInstancing.inl"
#include "BuildingNanite.inl"
}

FString UBuildingBlueprintOptimizerLibrary::FindAvailableAssetName(const FString& FolderPath, const FString& DesiredName)
{
    for (int32 Suffix = 0; Suffix < 100000; ++Suffix)
    {
        const FString Name = DesiredName + (Suffix ? FString::Printf(TEXT("_%d"), Suffix) : FString());
        const FString PackageName = FolderPath / Name;
        if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)) return Name;
    }
    return FString();
}

#include "BuildingAssetReplacement.inl"
#include "BuildingAssetOptimizer.inl"

#undef LOCTEXT_NAMESPACE
