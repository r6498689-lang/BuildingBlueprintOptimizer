#include "BuildingBlueprintAssetMenu.h"
#include "BuildingBlueprintOptimizerLibrary.h"
#include "BuildingBackupManager.h"
#include "BuildingOptimizationBatch.h"
#include "BuildingOptimizationMemory.h"
#include "BuildingOptimizationSave.h"
#include "BuildingBatchResultPanel.h"
#include "ContentBrowserMenuContexts.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "Misc/PackageName.h"
#include "Misc/MessageDialog.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/CoreDelegates.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"
#endif

#define LOCTEXT_NAMESPACE "BuildingBlueprintAssetOptimizer"
namespace BBCAssetMenu
{
static int Owner;
static FDelegateHandle StartupHandle;
static FDelegateHandle ExitHandle;
static void ShowResult(const FString& Text);

static TSharedRef<SWindow> MakeBackupsWindow()
{
    const TArray<BBCBackups::FRecord> Records = BBCBackups::List();
    TSharedRef<TSet<FString>> Selected = MakeShared<TSet<FString>>();
    TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
    int64 Bytes = 0;
    for (const auto& R : Records)
    {
        Bytes += R.Bytes;
        Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 14)
        [ SNew(SCheckBox).IsChecked_Lambda([Selected, Directory = R.Directory]
            { return Selected->Contains(Directory) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([Selected, Directory = R.Directory](ECheckBoxState S)
            { if (S == ECheckBoxState::Checked) Selected->Add(Directory); else Selected->Remove(Directory); })
            [ SNew(STextBlock).WrapTextAt(730).ToolTipText(FText::FromString(R.Directory)).Text(FText::FromString(R.AssetPath + TEXT("\n") + R.Created
                + FString::Printf(TEXT("  %.2f MB\n"), R.Bytes / 1048576.0) + R.Directory)) ] ];
    }
    TSharedRef<SWindow> Window = SNew(SWindow).Title(LOCTEXT("Backups", "清除旧的备份"))
        .ClientSize(FVector2D(820, 480)).SupportsMinimize(false).SupportsMaximize(false);
    const TWeakPtr<SWindow> Weak = Window;
    Window->SetContent(SNew(SBorder).Padding(16)
        [ SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(FString::Printf(TEXT("当前工程的插件备份：%d 项，共 %.2f MB。\n勾选要删除的备份；默认不选。删除后无法再用这些备份恢复蓝图。"), Records.Num(), Bytes / 1048576.0))) ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [ SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
                [ SNew(SButton).Text(LOCTEXT("SelectAllBackups", "全选"))
                    .IsEnabled(Records.Num() > 0)
                    .OnClicked_Lambda([Selected, Records] { for (const auto& R : Records) Selected->Add(R.Directory); return FReply::Handled(); }) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 14, 0)
                [ SNew(SButton).Text(LOCTEXT("ClearBackupSelection", "取消全选"))
                    .IsEnabled_Lambda([Selected] { return !Selected->IsEmpty(); })
                    .OnClicked_Lambda([Selected] { Selected->Reset(); return FReply::Handled(); }) ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [ SNew(STextBlock).Text_Lambda([Selected, Count = Records.Num()]
                    { return FText::FromString(FString::Printf(TEXT("已选 %d / %d 项"), Selected->Num(), Count)); }) ] ]
            + SVerticalBox::Slot().FillHeight(1)
            [ SNew(SScrollBox) + SScrollBox::Slot()[Rows] ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 0).HAlign(HAlign_Right)
            [ SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
                [ SNew(SButton).Text(LOCTEXT("BackupsClose", "关闭")).OnClicked_Lambda([Weak] { if (auto W = Weak.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); }) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ SNew(SButton).Text(LOCTEXT("DeleteBackups", "删除选中备份"))
                    .IsEnabled_Lambda([Selected] { return !Selected->IsEmpty(); })
                    .OnClicked_Lambda([Selected, Weak]
                    {
                        const FString Question = FString::Printf(TEXT("确定永久删除选中的 %d 项备份？\n不会删除当前蓝图或模型资产。删除后无法使用这些备份恢复。"), Selected->Num());
                        if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Question)) != EAppReturnType::Yes) return FReply::Handled();
                        int32 Removed = 0; TArray<FString> Errors;
                        for (const auto& Directory : *Selected)
                        {
                            FString Error; if (BBCBackups::Remove(Directory, Error)) ++Removed;
                            if (!Error.IsEmpty()) Errors.Add(Error);
                        }
                        if (auto W = Weak.Pin()) W->RequestDestroyWindow();
                        ShowResult(FString::Printf(TEXT("已删除 %d 项备份。"), Removed) + (Errors.IsEmpty() ? FString() : TEXT("\n") + FString::Join(Errors, TEXT("\n"))));
                        return FReply::Handled();
                    }) ] ]
        ]);
    return Window;
}
static void ShowBackups()
{
    FSlateApplication::Get().AddModalWindow(MakeBackupsWindow(), FGlobalTabmanager::Get()->GetRootWindow());
}

class SOptimizationDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOptimizationDialog) : _Count(1) {} SLATE_ARGUMENT(int32,Count) SLATE_END_ARGS()
    FBuildingOptimizationOptions Options;
    bool bAccepted = false;
    TWeakPtr<SWindow> Window;
    TSharedPtr<SCheckBox> ISMCheck, NaniteCheck, ReplaceCheck, CopyCheck;
    TSharedPtr<SButton> Confirm;
    void SavePreferences()
    {
        GConfig->SetBool(TEXT("BuildingBlueprintOptimizer"),TEXT("bUseISM"),Options.bUseISM,GEditorPerProjectIni);
        GConfig->SetBool(TEXT("BuildingBlueprintOptimizer"),TEXT("bEnableNanite"),Options.bEnableNanite,GEditorPerProjectIni);
        GConfig->SetBool(TEXT("BuildingBlueprintOptimizer"),TEXT("bReplaceOriginal"),Options.bReplaceOriginal,GEditorPerProjectIni);
        GConfig->Flush(false,GEditorPerProjectIni);
    }
    void Construct(const FArguments& Args)
    {
        GConfig->GetBool(TEXT("BuildingBlueprintOptimizer"),TEXT("bUseISM"),Options.bUseISM,GEditorPerProjectIni);
        GConfig->GetBool(TEXT("BuildingBlueprintOptimizer"),TEXT("bEnableNanite"),Options.bEnableNanite,GEditorPerProjectIni);
        GConfig->GetBool(TEXT("BuildingBlueprintOptimizer"),TEXT("bReplaceOriginal"),Options.bReplaceOriginal,GEditorPerProjectIni);
        ChildSlot [ SNew(SBorder).Padding(18)
            [ SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)
                [ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("已选择 %d 个蓝图；整批共用设置，逐项校验，失败后继续。"),Args._Count))).AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(STextBlock).Text(LOCTEXT("Choose", "选择优化内容（至少一项）")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
                [ SAssignNew(ISMCheck, SCheckBox).IsChecked(Options.bUseISM?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Options.bUseISM = S == ECheckBoxState::Checked; SavePreferences(); })
                    [ SNew(STextBlock).Text(LOCTEXT("ISM", "重复模型转为 ISM")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [ SAssignNew(NaniteCheck, SCheckBox).IsChecked(Options.bEnableNanite?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Options.bEnableNanite = S == ECheckBoxState::Checked; SavePreferences(); })
                    [ SNew(STextBlock).Text(LOCTEXT("Nanite", "开启原模型的 Nanite")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 16)
                [ SNew(STextBlock).Text(LOCTEXT("NaniteHint", "Nanite 会保存共享的原模型，影响所有引用；另存蓝图副本也一样。"))
                    .AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(STextBlock).Text(LOCTEXT("SaveMode", "保存方式")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
                [ SAssignNew(CopyCheck, SCheckBox).Style(FAppStyle::Get(), "RadioButton")
                    .IsChecked_Lambda([this] { return Options.bReplaceOriginal ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState S) { if (S == ECheckBoxState::Checked) {Options.bReplaceOriginal = false;SavePreferences();} })
                    [ SNew(STextBlock).Text(LOCTEXT("Copy", "另存副本（原蓝图保留）")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [ SAssignNew(ReplaceCheck, SCheckBox).Style(FAppStyle::Get(), "RadioButton")
                    .IsChecked_Lambda([this] { return Options.bReplaceOriginal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState S) { if (S == ECheckBoxState::Checked) {Options.bReplaceOriginal = true;SavePreferences();} })
                    [ SNew(STextBlock).Text(LOCTEXT("Replace", "替换原蓝图（更新所有引用）")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [ SNew(STextBlock).Text(LOCTEXT("BackupHint", "替换前备份原蓝图，校验失败会尝试恢复。成功后请检查并保存关卡；替换通过备份恢复，不依赖 Ctrl+Z。"))
                    .AutoWrapText(true).Visibility_Lambda([this] { return Options.bReplaceOriginal ? EVisibility::Visible : EVisibility::Collapsed; }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 18, 0, 0).HAlign(HAlign_Right)
                [ SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
                    [ SNew(SButton).Text(LOCTEXT("Cleanup", "清除旧的备份"))
                        .OnClicked_Lambda([] { ShowBackups(); return FReply::Handled(); }) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
                    [ SNew(SButton).Text(LOCTEXT("Cancel", "取消")).OnClicked_Lambda([this] { if (auto W = Window.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); }) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SAssignNew(Confirm, SButton).Text_Lambda([this] { return Options.bReplaceOriginal ? LOCTEXT("Apply", "备份并替换原蓝图") : LOCTEXT("Next", "下一步：选择保存位置"); })
                        .IsEnabled_Lambda([this] { return Options.bUseISM || Options.bEnableNanite; })
                        .OnClicked_Lambda([this] { bAccepted = true; if (auto W = Window.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); }) ] ]
            ] ];
    }
};

static void ShowResult(const FString& Text)
{
    TSharedRef<SWindow> Window = SNew(SWindow).Title(LOCTEXT("Result", "优化建筑蓝图"))
        .ClientSize(FVector2D(720, 520)).SupportsMinimize(false);
    Window->SetContent(SNew(SBorder).Padding(14)
        [ SNew(SVerticalBox)
            + SVerticalBox::Slot().FillHeight(1)
            [ SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(FText::FromString(Text)) ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 10, 0, 0)
            [ SNew(SButton).Text(LOCTEXT("Close", "关闭")).OnClicked_Lambda([Weak = TWeakPtr<SWindow>(Window)]
                { if (auto Pinned = Weak.Pin()) Pinned->RequestDestroyWindow(); return FReply::Handled(); }) ]
        ]);
    FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());
}

static FString ChooseBatchFolder()
{
    TSharedRef<FString> Path=MakeShared<FString>(TEXT("/Game/Buildings"));TSharedRef<bool> Accepted=MakeShared<bool>(false);
    GConfig->GetString(TEXT("BuildingBlueprintOptimizer"),TEXT("LastOutputFolder"),*Path,GEditorPerProjectIni);
    auto W=SNew(SWindow).Title(LOCTEXT("BatchFolder","统一选择优化副本输出目录")).ClientSize(FVector2D(520,460)).SupportsMinimize(false).SupportsMaximize(false);
    FPathPickerConfig Picker;Picker.DefaultPath=*Path;Picker.bAddDefaultPath=true;Picker.bAllowReadOnlyFolders=false;
    Picker.OnPathSelected=FOnPathSelected::CreateLambda([Path](const FString& P){*Path=P;});
    W->SetContent(SNew(SVerticalBox)
        +SVerticalBox::Slot().FillHeight(1)[FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().CreatePathPicker(Picker)]
        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("FolderCancel","取消")).OnClicked_Lambda([Weak=TWeakPtr<SWindow>(W)]{if(auto P=Weak.Pin())P->RequestDestroyWindow();return FReply::Handled();})]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("FolderConfirm","使用此目录并开始"))
                .IsEnabled_Lambda([Path]{return (*Path==TEXT("/Game")||Path->StartsWith(TEXT("/Game/")))&&FPackageName::IsValidLongPackageName(*Path/TEXT("Probe"));})
                .OnClicked_Lambda([Path,Accepted,Weak=TWeakPtr<SWindow>(W)]{*Accepted=true;if(auto P=Weak.Pin())P->RequestDestroyWindow();return FReply::Handled();})]]);
    FSlateApplication::Get().AddModalWindow(W,FGlobalTabmanager::Get()->GetRootWindow());
    if(!*Accepted)return FString();
    GConfig->SetString(TEXT("BuildingBlueprintOptimizer"),TEXT("LastOutputFolder"),**Path,GEditorPerProjectIni);GConfig->Flush(false,GEditorPerProjectIni);return *Path;
}
static void ShowBatchResult(const FBuildingOptimizationBatchResult& R)
{
    auto W=SNew(SWindow).Title(LOCTEXT("BatchComplete","批量优化蓝图结果")).ClientSize(FVector2D(760,560));
    W->SetContent(SNew(SBorder).Padding(14)[SNew(SVerticalBox)
        +SVerticalBox::Slot().FillHeight(1)[MakeBatchResultPanel(R)]
        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("BatchCopy","复制完整报告")).OnClicked_Lambda([Text=R.Message]{FPlatformApplicationMisc::ClipboardCopy(*Text);return FReply::Handled();})]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("BatchClose","关闭")).OnClicked_Lambda([Weak=TWeakPtr<SWindow>(W)]{if(auto P=Weak.Pin())P->RequestDestroyWindow();return FReply::Handled();})]]]);
    FSlateApplication::Get().AddModalWindow(W,FGlobalTabmanager::Get()->GetRootWindow());
}
static void Execute(TArray<FAssetData> Assets)
{
    if (!GEditor || GEditor->PlayWorld || BBCOptimizationBatch::IsRunning() || Assets.IsEmpty()) return;
    // Snapshot paths only. Do not preload every selected building before its turn.
    TSharedRef<SWindow> Window = SNew(SWindow).Title(LOCTEXT("Options", "优化建筑蓝图"))
        .ClientSize(FVector2D(600, 440)).SupportsMinimize(false).SupportsMaximize(false);
    TSharedRef<SOptimizationDialog> Dialog = SNew(SOptimizationDialog).Count(Assets.Num()); Dialog->Window = Window; Window->SetContent(Dialog);
    FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());
    if (!Dialog->bAccepted) return;
    if(Assets.Num()>1)
    {
        FString Folder;if(!Dialog->Options.bReplaceOriginal){Folder=ChooseBatchFolder();if(Folder.IsEmpty())return;}
        auto Progress=SNew(SWindow).Title(LOCTEXT("BatchProgress","正在批量优化建筑蓝图")).ClientSize(FVector2D(660,170)).SupportsMinimize(false).SupportsMaximize(false);
        Progress->SetContent(SNew(SBorder).Padding(18)[SNew(SVerticalBox)
            +SVerticalBox::Slot().FillHeight(1)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([]{return FText::FromString(BBCOptimizationBatch::Status());})]
            +SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(LOCTEXT("CancelBatch","取消剩余项（已完成结果保留）")).OnClicked_Lambda([]{BBCOptimizationBatch::Cancel();return FReply::Handled();})]]);
        Progress->SetOnWindowClosed(FOnWindowClosed::CreateLambda([](const TSharedRef<SWindow>&){BBCOptimizationBatch::Cancel();}));
        const bool Started=BBCOptimizationBatch::Start(Assets,Folder,Dialog->Options,[Weak=TWeakPtr<SWindow>(Progress)](const FBuildingOptimizationBatchResult& R)
        {if(auto P=Weak.Pin())P->RequestDestroyWindow();if(!IsEngineExitRequested())ShowBatchResult(R);});
        if(Started)FSlateApplication::Get().AddWindow(Progress);
        return;
    }
    const FString LoadId=FGuid::NewGuid().ToString(EGuidFormats::Digits);
    BBCMemory::Sample(LoadId,Assets[0].GetSoftObjectPath().ToString(),TEXT("BeforeAssetLoad"));
    auto* Source=Cast<UBlueprint>(Assets[0].GetAsset());BBCMemory::Sample(LoadId,Assets[0].GetSoftObjectPath().ToString(),TEXT("AfterAssetLoad"));
    const FString Reason=UBuildingBlueprintOptimizerLibrary::GetBuildingOptimizationReason(Source);
    if(!Reason.IsEmpty()){ShowResult(Reason);return;}
    auto& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
    FString Folder = FPackageName::GetLongPackagePath(Source->GetOutermost()->GetName());
    FString Name = Source->GetName();
    if (!Dialog->Options.bReplaceOriginal)
    {
    FSaveAssetDialogConfig Config;
    Config.DialogTitleOverride = LOCTEXT("SaveCopy", "保存建筑优化副本（原蓝图保留）");
    Config.DefaultPath = FPackageName::GetLongPackagePath(Source->GetOutermost()->GetName());
    if (!(Config.DefaultPath == TEXT("/Game") || Config.DefaultPath.StartsWith(TEXT("/Game/")))) Config.DefaultPath = TEXT("/Game/Buildings");
    const FString Suffix = Dialog->Options.bUseISM ? (Dialog->Options.bEnableNanite ? TEXT("_ISM_Nanite") : TEXT("_ISM")) : TEXT("_Nanite");
    Config.DefaultAssetName = UBuildingBlueprintOptimizerLibrary::FindAvailableAssetName(Config.DefaultPath, Source->GetName() + Suffix);
    Config.AssetClassNames.Add(UBlueprint::StaticClass()->GetClassPathName());
    Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;
    const FString Path = Browser.CreateModalSaveAssetDialog(Config);
    if (Path.IsEmpty()) return;
    const FString Package = FPackageName::ObjectPathToPackageName(Path);
    Folder = FPackageName::GetLongPackagePath(Package); Name = FPackageName::GetLongPackageAssetName(Package);
    }
    auto Result = UBuildingBlueprintOptimizerLibrary::OptimizeBuildingBlueprintWithOptions(Source, Folder, Name, Dialog->Options);
    SaveAllAfterBuildingOptimization(Result, LoadId, Assets[0].GetSoftObjectPath().ToString());
    if (Result.bSuccess)
        if (auto* Output = LoadObject<UBlueprint>(nullptr, *Result.AssetObjectPath)) Browser.SyncBrowserToAssets(TArray<UObject*>({Output}));
    ShowResult(Result.Message);
}

static void Register()
{
    FToolMenuOwnerScoped Scope(&Owner);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.Blueprint"));
    Menu->FindOrAddSection(TEXT("BuildingBlueprintOptimization"), LOCTEXT("Section", "建筑蓝图"))
        .AddDynamicEntry(TEXT("BuildingISMOptimizer"), FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section)
        {
            const auto* Context = Section.FindContext<UContentBrowserAssetContextMenuContext>();
            if (!Context || Context->SelectedAssets.IsEmpty()) return;
            const TArray<FAssetData> Assets = Context->SelectedAssets;
            // Read registry data only. Opening a context menu must not spawn a
            // building or synchronously load a large blueprint.
            const bool bSingleBlueprint = !Assets.IsEmpty() && !Assets.ContainsByPredicate([](const FAssetData& A){return A.AssetClassPath!=UBlueprint::StaticClass()->GetClassPathName();});
            Section.AddMenuEntry(TEXT("OptimizeBuildingISM"), LOCTEXT("Optimize", "优化建筑蓝图…"),
                bSingleBlueprint ? LOCTEXT("Hint", "选择 ISM / Nanite 优化，以及替换原蓝图或另存副本。")
                    : LOCTEXT("SelectOne", "请选择建筑蓝图资产，可多选。"),
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.Blueprint"),
                FUIAction(FExecuteAction::CreateLambda([Assets] { Execute(Assets); }),
                    FCanExecuteAction::CreateLambda([bSingleBlueprint] { return bSingleBlueprint && GEditor && !GEditor->PlayWorld && !BBCOptimizationBatch::IsRunning(); })));
        }));
}

void Start()
{
    ExitHandle=FCoreDelegates::OnEnginePreExit.AddLambda([]{BBCOptimizationBatch::Stop();BBCMemory::Stop();});
    StartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&Register));
}
void Stop()
{
    BBCOptimizationBatch::Stop();BBCMemory::Stop();FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);ExitHandle.Reset();
    if (StartupHandle.IsValid()) UToolMenus::UnRegisterStartupCallback(StartupHandle);
    UToolMenus::UnregisterOwner(&Owner); StartupHandle.Reset();
}
}
#undef LOCTEXT_NAMESPACE


#undef LOCTEXT_NAMESPACE
