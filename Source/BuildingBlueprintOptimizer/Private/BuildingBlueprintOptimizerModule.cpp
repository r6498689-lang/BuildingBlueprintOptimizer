#include "BuildingBlueprintAssetMenu.h"
#include "BuildingOutlinerVisibility.h"
#include "BuildingOptimizerThumbnailRenderer.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

class FBuildingBlueprintOptimizerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        BBCOutliner::Start();
        BBCThumbnails::Start();
        BBCAssetMenu::Start();
        ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FBuildingBlueprintOptimizerModule::Release);
    }
    virtual void ShutdownModule() override
    {
        FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
        Release();
    }
private:
    void Release()
    {
        if (bReleased) return;
        bReleased = true;
        BBCAssetMenu::Stop();
        BBCThumbnails::Stop();
        BBCOutliner::Stop();
    }
    FDelegateHandle ExitHandle;
    bool bReleased = false;
};
IMPLEMENT_MODULE(FBuildingBlueprintOptimizerModule, BuildingBlueprintOptimizer)
