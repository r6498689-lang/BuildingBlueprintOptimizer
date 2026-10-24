#pragma once
#include "CoreMinimal.h"
namespace BBCBackups
{
struct FRecord
{
    FString Directory, AssetPath, Created;
    int64 Bytes = 0;
    TArray<FString> Files;
};
FString Root();
bool Record(const FString& Directory, const FString& AssetPath, FString& Error);
TArray<FRecord> List();
bool Remove(const FString& Directory, FString& Error);
}
