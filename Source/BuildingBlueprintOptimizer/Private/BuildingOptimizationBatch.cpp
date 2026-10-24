#include "BuildingOptimizationBatch.h"
#include "BuildingOptimizationMemory.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "StaticMeshCompiler.h"
#include "UObject/GarbageCollection.h"
#include "Editor.h"
#include "BuildingOptimizationSave.h"
namespace BBCOptimizationBatch
{
struct FTask : TSharedFromThis<FTask>
{
    TArray<FAssetData> Assets; FString Folder,Id,Status,LastAsset;
    FBuildingOptimizationOptions Options;
    FBuildingOptimizationBatchResult Result;
    TFunction<void(const FBuildingOptimizationBatchResult&)> Finished;
    int32 Index=0;bool bWaiting=false,bCancel=false;
    double LastTime=0;uint64 LastFrame=0;
    FTSTicker::FDelegateHandle Handle;
    bool Tick(float);
};
static TSharedPtr<FTask> Active;
static FBuildingOptimizationBatchResult Completed;
bool IsRunning(){return Active.IsValid();}
FString Status(){return Active?Active->Status:FString();}
const FBuildingOptimizationBatchResult& LastResult(){return Completed;}
void Cancel(){if(Active)Active->bCancel=true;}
void Stop(){if(Active&&Active->Handle.IsValid())FTSTicker::GetCoreTicker().RemoveTicker(Active->Handle);Active.Reset();}
bool FTask::Tick(float)
{
    auto KeepAlive=AsShared();
    if(IsEngineExitRequested()){Stop();return false;}
    if(bWaiting)
    {
        if(FPlatformTime::Seconds()-LastTime<1.0||GFrameCounter<=LastFrame+2)return true;
        // The optimization call and its preview/transaction scopes have returned.
        // Collect only unreachable objects; preserve loaded assets and undo history.
        if(!IsAsyncLoading() && FStaticMeshCompilingManager::Get().GetNumRemainingMeshes()==0
            && !IsGarbageCollecting() && (!GEditor || !GEditor->IsTransactionActive()))
        {
            BBCMemory::Sample(Id,LastAsset,TEXT("BeforeBetweenItemsGC"));
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
            BBCMemory::Sample(Id,LastAsset,TEXT("AfterBetweenItemsGC"));
        }
        BBCMemory::Sample(Id,LastAsset,(!IsAsyncLoading()&&FStaticMeshCompilingManager::Get().GetNumRemainingMeshes()==0)?TEXT("BetweenItems_QuietObserved"):TEXT("BetweenItems_BackgroundBusy"));
        bWaiting=false;
    }
    if(Index<Assets.Num())
    {
        const FAssetData Data=Assets[Index++];LastAsset=Data.GetSoftObjectPath().ToString();
        Status=FString::Printf(TEXT("正在处理 %d / %d：%s"),Index,Assets.Num(),*Data.AssetName.ToString());
        FBuildingOptimizationBatchItem Item;Item.SourceLabel=Data.AssetName.ToString()+TEXT(" [")+Data.PackagePath.ToString()+TEXT("]");Item.SourceObjectPath=LastAsset;
        BBCMemory::Sample(Id,LastAsset,TEXT("BeforeAssetLoad"));
        if(bCancel)Item.Result.Message=TEXT("用户取消，本项未执行。");
        else if(auto* BP=Cast<UBlueprint>(Data.GetAsset()))
        {
            const FString Suffix=Options.bUseISM?(Options.bEnableNanite?TEXT("_ISM_Nanite"):TEXT("_ISM")):TEXT("_Nanite");
            Item.Result=UBuildingBlueprintOptimizerLibrary::OptimizeBuildingBlueprintWithOptions(BP,Folder,Data.AssetName.ToString()+Suffix,Options);
            SaveAllAfterBuildingOptimization(Item.Result, Id, LastAsset);
        }
        else Item.Result.Message=TEXT("不是可加载的建筑蓝图，本项跳过。");
        if(Item.Result.bSuccess)++Result.CreatedCount;else ++Result.FailedCount;
        Result.Items.Add(MoveTemp(Item));
        LastTime=FPlatformTime::Seconds();LastFrame=GFrameCounter;bWaiting=true;
        Status=FString::Printf(TEXT("已处理 %d / %d；成功 %d，失败/未处理 %d。正在等待编辑器清理与内存采样…"),Index,Assets.Num(),Result.CreatedCount,Result.FailedCount);
        return true;
    }
    Result.Message=FString::Printf(TEXT("批量优化完成：成功 %d，失败/未处理 %d。\n%s\n内存记录：%s"),Result.CreatedCount,Result.FailedCount,Options.bReplaceOriginal?TEXT("模式：替换原蓝图，请检查并保存受影响关卡和引用蓝图。"):*(TEXT("输出目录：")+Folder),*BBCMemory::File());
    for(const auto& I:Result.Items)Result.Message+=FString::Printf(TEXT("\n\n[%s] %s\n%s"),I.Result.bSuccess?TEXT("成功"):TEXT("失败/未处理"),*I.SourceObjectPath,*I.Result.Message);
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintOptimizer/OptimizationReports");IFileManager::Get().MakeDirectory(*Dir,true);
    const FString Path=FPaths::ConvertRelativePathToFull(Dir/(Id+TEXT(".txt")));
    if(FFileHelper::SaveStringToFile(Result.Message,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))Result.ReportFilePath=Path;
    Completed=Result;Handle.Reset();Active.Reset();
    if(Finished)Finished(Completed);return false;
}
bool Start(const TArray<FAssetData>& Assets,const FString& Folder,const FBuildingOptimizationOptions& Options,TFunction<void(const FBuildingOptimizationBatchResult&)> Finished)
{
    if(Active||Assets.IsEmpty()||(!Options.bUseISM&&!Options.bEnableNanite))return false;
    if(!Options.bReplaceOriginal&&(!(Folder==TEXT("/Game")||Folder.StartsWith(TEXT("/Game/")))||!FPackageName::IsValidLongPackageName(Folder/TEXT("Probe"))))return false;
    Active=MakeShared<FTask>();TSet<FSoftObjectPath> Seen;
    for(const auto& A:Assets)if(!Seen.Contains(A.GetSoftObjectPath())){Seen.Add(A.GetSoftObjectPath());Active->Assets.Add(A);}
    Active->Folder=Folder;Active->Options=Options;Active->Finished=MoveTemp(Finished);
    Active->Id=FGuid::NewGuid().ToString(EGuidFormats::Digits);Active->Result.bAccepted=true;Active->Status=TEXT("准备开始批量优化…");
    Active->Handle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(Active.ToSharedRef(),&FTask::Tick),.1f);return true;
}
}
