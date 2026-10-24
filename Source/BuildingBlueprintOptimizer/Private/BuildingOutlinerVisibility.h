#pragma once

#include "CoreMinimal.h"

class AActor;
class UBlueprint;
class ISceneOutliner;

namespace BBCOutliner
{
    void MarkBuilding(UBlueprint* Blueprint);
    bool IsGeneratedBlueprint(const UBlueprint* Blueprint);
    bool IsGeneratedBuilding(const AActor* Actor);
    bool ShouldShowActor(const AActor* Actor);
    void InstallOnOutliner(const TSharedRef<ISceneOutliner>& Outliner);
    void Start();
    void Stop();
}
