#include "BuildingOptimizationMemory.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "StaticMeshCompiler.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
DEFINE_LOG_CATEGORY_STATIC(LogBBCMemory, Log, All);
namespace BBCMemory
{
static FString LogFile;
static uint64 Generation=0;
static int32 ActiveDepth=0;
struct FPending { FString Id,Asset; double Due; uint64 Generation; int32 Delay; };
static TArray<FPending> Pending;
static FTSTicker::FDelegateHandle Handle;
FString File()
{
    if(LogFile.IsEmpty())
    {
        const FString Dir=FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintOptimizer/MemoryReports");
        IFileManager::Get().MakeDirectory(*Dir,true);
        LogFile=FPaths::ConvertRelativePathToFull(Dir/FString::Printf(TEXT("Memory_%s_%u.csv"),*FGuid::NewGuid().ToString(EGuidFormats::Digits),FPlatformProcess::GetCurrentProcessId()));
        FFileHelper::SaveStringToFile(TEXT("UTC,OperationId,Stage,Asset,ProcessCommitBytes,ProcessPeakCommitBytes,WorkingSetBytes,AvailablePhysicalBytes,UEAvailableCommitBytes,UObjectSlots,StaticMeshJobs,AsyncLoading,Frame\n"),*LogFile,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    return LogFile;
}
void Sample(const FString& Id,const FString& Asset,const TCHAR* Stage)
{
    const auto M=FPlatformMemory::GetStats();
    FString Quoted=Asset.Replace(TEXT("\""),TEXT("\"\""));
    const FString Line=FString::Printf(TEXT("%s,%s,%s,\"%s\",%llu,%llu,%llu,%llu,%llu,%d,%d,%d,%llu\n"),
        *FDateTime::UtcNow().ToIso8601(),*Id,Stage,*Quoted,uint64(M.UsedVirtual),uint64(M.PeakUsedVirtual),uint64(M.UsedPhysical),uint64(M.AvailablePhysical),uint64(M.AvailableVirtual),
        GUObjectArray.GetObjectArrayNum(),FStaticMeshCompilingManager::Get().GetNumRemainingMeshes(),IsAsyncLoading()?1:0,uint64(GFrameCounter));
    if(!FFileHelper::SaveStringToFile(Line,*File(),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_Append))
        UE_LOG(LogBBCMemory,Warning,TEXT("Cannot append memory report: %s"),*LogFile);
    UE_LOG(LogBBCMemory,Log,TEXT("%s %s %s Commit=%.2f MiB WorkingSet=%.2f MiB MeshJobs=%d"),*Id,Stage,*Asset,M.UsedVirtual/1048576.0,M.UsedPhysical/1048576.0,FStaticMeshCompilingManager::Get().GetNumRemainingMeshes());
}
FString Begin(const FString& Asset)
{
    ++Generation;++ActiveDepth;const FString Id=FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Sample(Id,Asset,TEXT("BeforeOptimization"));return Id;
}
void End(const FString& Id,const FString& Asset)
{
    Sample(Id,Asset,TEXT("AfterPreviewCleanup"));
    ActiveDepth=FMath::Max(0,ActiveDepth-1);
    for(int32 Delay:{5,15})Pending.Add({Id,Asset,FPlatformTime::Seconds()+Delay,Generation,Delay});
    if(!Handle.IsValid())Handle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        if(IsEngineExitRequested()){Pending.Reset();Handle.Reset();return false;}
        const double Now=FPlatformTime::Seconds();
        for(int32 I=Pending.Num()-1;I>=0;--I)if(Now>=Pending[I].Due)
        {
            const auto P=Pending[I];
            const bool Quiet=ActiveDepth==0&&P.Generation==Generation&&!IsAsyncLoading()&&FStaticMeshCompilingManager::Get().GetNumRemainingMeshes()==0;
            const FString Stage=FString::Printf(TEXT("DeferredMin%dSeconds_%s"),P.Delay,Quiet?TEXT("QuietObserved"):TEXT("BusyOrLaterOperation"));
            Sample(P.Id,P.Asset,*Stage);Pending.RemoveAt(I);
        }
        if(Pending.IsEmpty()){Handle.Reset();return false;}return true;
    }),.25f);
}
void Stop(){if(Handle.IsValid())FTSTicker::GetCoreTicker().RemoveTicker(Handle);Handle.Reset();Pending.Reset();}
}
