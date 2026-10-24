#pragma once
#include "CoreMinimal.h"
class SWidget;
struct FBuildingOptimizationBatchResult;
TSharedRef<SWidget> MakeBatchResultPanel(const FBuildingOptimizationBatchResult& Result);
