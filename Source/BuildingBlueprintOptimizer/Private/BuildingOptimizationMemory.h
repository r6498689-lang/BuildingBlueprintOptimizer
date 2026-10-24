#pragma once
#include "CoreMinimal.h"
namespace BBCMemory
{
FString Begin(const FString& Asset);
void Sample(const FString& Id, const FString& Asset, const TCHAR* Stage);
void End(const FString& Id, const FString& Asset);
FString File();
void Stop();
}
