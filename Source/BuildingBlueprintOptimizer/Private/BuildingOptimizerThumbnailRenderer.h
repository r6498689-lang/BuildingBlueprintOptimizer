#pragma once

#include "ThumbnailRendering/BlueprintThumbnailRenderer.h"
#include "BuildingOptimizerThumbnailRenderer.generated.h"

// Ordinary blueprints continue to use the engine renderer unchanged.
UCLASS()
class UBuildingOptimizerThumbnailRenderer : public UBlueprintThumbnailRenderer
{
    GENERATED_BODY()
public:
    virtual bool CanVisualizeAsset(UObject* Object) override;
    virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
        FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
    virtual void BeginDestroy() override;
private:
    TUniquePtr<FBlueprintThumbnailScene> BuildingScene;
};

namespace BBCThumbnails
{
    void Start();
    void Stop();
    void CacheForSave(UBlueprint* Blueprint);
}
