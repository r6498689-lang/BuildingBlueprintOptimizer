#include "BuildingBatchResultPanel.h"
#include "BuildingBlueprintOptimizerLibrary.h"
#include "Styling/AppStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#define LOCTEXT_NAMESPACE "BuildingBatchResults"
TSharedRef<SWidget> MakeBatchResultPanel(const FBuildingOptimizationBatchResult& Result)
{
    const FLinearColor Success(.20f, .85f, .35f), Failure(1.f, .20f, .18f);
    TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
    // Stable partition for presentation only; processing order and the complete
    // plain-text report remain available through Copy Report.
    for (bool bSuccess : {false, true}) for (const auto& Item : Result.Items)
        if (Item.Result.bSuccess == bSuccess)
        {
            const FLinearColor Color = bSuccess ? Success : Failure;
            Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 12)
            [ SNew(SBorder).Padding(12).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                [ SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
                        [ SNew(STextBlock).Text(bSuccess ? LOCTEXT("BatchSuccess", "成功") : LOCTEXT("BatchFailure", "失败"))
                            .ColorAndOpacity(Color).Font(FAppStyle::GetFontStyle("BoldFont")) ]
                        + SHorizontalBox::Slot().FillWidth(1)
                        [ SNew(STextBlock).Text(FText::FromString(Item.SourceLabel)).AutoWrapText(true) ] ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                    [ SNew(STextBlock).Text(FText::FromString(Item.Result.Message)).AutoWrapText(true) ] ] ];
        }
    if (!Result.IncludedDescendantLabels.IsEmpty())
        Rows->AddSlot().AutoHeight()[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("已包含在祖先中，未重复创建：") + FString::Join(Result.IncludedDescendantLabels, TEXT("、")))) ];
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 24, 0)
            [ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("失败 %d 组"), Result.FailedCount)))
                .ColorAndOpacity(Result.FailedCount ? FSlateColor(Failure) : FSlateColor::UseForeground()).Font(FAppStyle::GetFontStyle("BoldFont")) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("成功 %d 组"), Result.CreatedCount)))
                .ColorAndOpacity(Success).Font(FAppStyle::GetFontStyle("BoldFont")) ] ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
        [ SNew(STextBlock).Text(Result.FailedCount ? LOCTEXT("FailuresFirst", "失败项已置顶，请先查看原因。完整结果可复制报告。") : LOCTEXT("AllSucceeded", "全部处理成功。完整结果可复制报告。")) ]
        + SVerticalBox::Slot().FillHeight(1)[ SNew(SScrollBox) + SScrollBox::Slot()[Rows] ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
        [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Result.ReportFilePath.IsEmpty() ? TEXT("报告未写入文件，请复制完整报告留存。") : TEXT("报告：") + Result.ReportFilePath)) ];
}


#undef LOCTEXT_NAMESPACE
