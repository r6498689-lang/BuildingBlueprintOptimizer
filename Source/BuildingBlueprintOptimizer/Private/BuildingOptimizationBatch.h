#pragma once
#include "CoreMinimal.h"
#include "BuildingBlueprintOptimizerLibrary.h"
#include "AssetRegistry/AssetData.h"
namespace BBCOptimizationBatch
{
bool Start(const TArray<FAssetData>& Assets,const FString& Folder,const FBuildingOptimizationOptions& Options,TFunction<void(const FBuildingOptimizationBatchResult&)> Finished);
bool IsRunning();
FString Status();
void Cancel();
void Stop();
const FBuildingOptimizationBatchResult& LastResult();
}
