#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BuildingBlueprintOptimizerLibrary.generated.h"

class UBlueprint;

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTOPTIMIZER_API FBuildingOptimizationOptions
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building") bool bUseISM = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building") bool bEnableNanite = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building") bool bReplaceOriginal = false;
};

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTOPTIMIZER_API FBuildingOptimizationResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Building") bool bSuccess = false;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString AssetObjectPath;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString Message;
    UPROPERTY(BlueprintReadOnly, Category="Building") TObjectPtr<AActor> ReplacementActor = nullptr;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 ActorCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") FVector Pivot = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 ISMGroupCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 ISMInstanceCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building", meta=(DeprecatedProperty, DeprecationMessage="Use ISMGroupCount.")) int32 HISMGroupCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building", meta=(DeprecatedProperty, DeprecationMessage="Use ISMInstanceCount.")) int32 HISMInstanceCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") TArray<FString> Warnings;
    UPROPERTY(BlueprintReadOnly, Category="Building") bool bNaniteProcessed = false;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 NaniteEnabledCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 NaniteAlreadyEnabledCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString BackupDirectory;
};

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTOPTIMIZER_API FBuildingOptimizationBatchItem
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="Building") FString SourceLabel;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString SourceObjectPath;
    UPROPERTY(BlueprintReadOnly, Category="Building") FBuildingOptimizationResult Result;
};

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTOPTIMIZER_API FBuildingOptimizationBatchResult
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="Building") bool bAccepted = false;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 CreatedCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 FailedCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") TArray<FString> IncludedDescendantLabels;
    UPROPERTY(BlueprintReadOnly, Category="Building") TArray<FBuildingOptimizationBatchItem> Items;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString Message;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString ReportFilePath;
};

UCLASS()
class BUILDINGBLUEPRINTOPTIMIZER_API UBuildingBlueprintOptimizerLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    static FString GetBuildingOptimizationReason(const UBlueprint* Blueprint);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static FBuildingOptimizationResult OptimizeBuildingBlueprint(UBlueprint* Blueprint, const FString& FolderPath, const FString& AssetName);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static FBuildingOptimizationResult OptimizeBuildingBlueprintWithOptions(UBlueprint* Blueprint, const FString& FolderPath, const FString& AssetName, const FBuildingOptimizationOptions& Options);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static FString FindAvailableAssetName(const FString& FolderPath, const FString& DesiredName);

};
