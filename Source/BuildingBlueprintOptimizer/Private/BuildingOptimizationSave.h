#pragma once
#include "BuildingBlueprintOptimizerLibrary.h"
#include "BuildingOptimizationMemory.h"
#include "FileHelpers.h"
#include "Misc/App.h"

inline void SaveAllAfterBuildingOptimization(FBuildingOptimizationResult& Result, const FString& Id, const FString& Asset)
{
    if (!Result.bSuccess) return;
    BBCMemory::Sample(Id, Asset, TEXT("BeforeSaveAll"));
    // Save maps, external actors and content, using native save/checkout handling.
    TArray<UPackage*> Packages, Failed;
    FEditorFileUtils::GetDirtyPackages(Packages);
    const auto Code = Packages.IsEmpty() ? FEditorFileUtils::PR_Success :
        FEditorFileUtils::PromptForCheckoutAndSave(Packages, true, false, &Failed, FApp::IsUnattended(), false);
    TArray<UPackage*> Remaining;
    FEditorFileUtils::GetDirtyPackages(Remaining);
    const bool Saved = Code == FEditorFileUtils::PR_Success && Failed.IsEmpty() && Remaining.IsEmpty();
    BBCMemory::Sample(Id, Asset, Saved ? TEXT("AfterSaveAll") : TEXT("SaveAllFailed"));
    Result.Message += Saved ? TEXT("\n全部保存完成（包括关卡及其他已修改资源）。")
        : TEXT("\n全部保存失败或取消：优化结果已保留，请手动全部保存；本项按失败记录。未回滚已经保存的内容。");
    if (!Saved) Result.bSuccess = false;
}
