#include "BuildingBackupManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif
namespace BBCBackups
{
static const TCHAR* Metadata = TEXT("BackupRecord.txt");
FString Root() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintOptimizer/AssetBackups")); }
static bool SafeDirectory(const FString& Directory)
{
    const FString Full = FPaths::ConvertRelativePathToFull(Directory);
    const FString Parent = FPaths::GetPath(Full);
    FGuid Id;
    if (!FPaths::IsSamePath(Parent, Root()) || FPaths::GetCleanFilename(Full).Len() != 32
        || !FGuid::ParseExact(FPaths::GetCleanFilename(Full), EGuidFormats::Digits, Id)) return false;
#if PLATFORM_WINDOWS
    // Reject junctions/symlinks in every ancestor. Deletion must stay within the
    // actual project's backup directory, not just a matching textual prefix.
    for (FString Path = Full; !Path.IsEmpty();)
    {
        const DWORD Attributes = ::GetFileAttributesW(*Path);
        if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const FString Next = FPaths::GetPath(Path);
        if (Next == Path || Next.Len() < 3) break;
        Path = Next;
    }
#endif
    return IFileManager::Get().DirectoryExists(*Full);
}
static bool Read(const FString& Directory, FRecord& Out)
{
    if (!SafeDirectory(Directory)) return false;
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *(Directory / Metadata)) || Lines.Num() < 5
        || Lines[0] != TEXT("BuildingBlueprintOptimizer.Backup.v1") || !FPaths::IsSamePath(Lines[3], Directory)
        || !Lines[1].StartsWith(TEXT("/Game/"))) return false;
    Out.Directory = FPaths::ConvertRelativePathToFull(Directory); Out.AssetPath = Lines[1]; Out.Created = Lines[2];
    const FString Base = FPackageName::GetLongPackageAssetName(FPackageName::ObjectPathToPackageName(Out.AssetPath));
    for (int32 I = 4; I < Lines.Num(); ++I)
    {
        const FString& Name = Lines[I];
        if (Name != Base + TEXT(".uasset") && Name != Base + TEXT(".uexp") && Name != Base + TEXT(".ubulk")) return false;
        if (Out.Files.Contains(Name)) return false;
        Out.Files.Add(Name);
        const int64 Size = IFileManager::Get().FileSize(*(Directory / Name));
        if (Size < 0) return false;
        Out.Bytes += Size;
    }
    return Out.Files.Contains(Base + TEXT(".uasset"));
}
static bool Journal(const TCHAR* Action, const FRecord& R)
{
    const FString Line = FString(Action) + TEXT("\t") + FDateTime::UtcNow().ToIso8601() + TEXT("\t")
        + R.AssetPath + TEXT("\t") + R.Directory + TEXT("\t") + FString::Printf(TEXT("%lld\n"), R.Bytes);
    return FFileHelper::SaveStringToFile(Line, *(Root() / TEXT("BackupIndex.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
        &IFileManager::Get(), FILEWRITE_Append);
}
bool Record(const FString& Directory, const FString& AssetPath, FString& Error)
{
    if (!SafeDirectory(Directory)) { Error = TEXT("备份路径校验失败，未替换原蓝图。"); return false; }
    TArray<FString> Files; IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*")), true, false);
    TArray<FString> Lines{TEXT("BuildingBlueprintOptimizer.Backup.v1"), AssetPath, FDateTime::UtcNow().ToIso8601(), FPaths::ConvertRelativePathToFull(Directory)};
    Lines.Append(Files);
    if (!FFileHelper::SaveStringArrayToFile(Lines, *(Directory / Metadata), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { Error = TEXT("备份记录保存失败，未替换原蓝图。"); return false; }
    FRecord R;
    if (!Read(Directory, R) || !Journal(TEXT("Created"), R))
    { Error = TEXT("备份索引记录失败，未替换原蓝图。"); return false; }
    return true;
}
TArray<FRecord> List()
{
    TArray<FString> Directories; IFileManager::Get().FindFiles(Directories, *(Root() / TEXT("*")), false, true);
    TArray<FRecord> Result;
    for (const auto& Name : Directories) { FRecord R; if (Read(Root() / Name, R)) Result.Add(MoveTemp(R)); }
    Result.Sort([](const FRecord& A, const FRecord& B) { return A.Created > B.Created; });
    return Result;
}
bool Remove(const FString& Directory, FString& Error)
{
    FRecord R;
    if (!Read(Directory, R)) { Error = TEXT("不是当前工程的有效插件备份，未删除：") + Directory; return false; }
    TArray<FString> Files, Dirs;
    IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*")), true, false);
    IFileManager::Get().FindFiles(Dirs, *(Directory / TEXT("*")), false, true);
    if (!Dirs.IsEmpty() || Files.Num() != R.Files.Num() + 1)
    { Error = TEXT("目录中包含未记录内容，未删除：") + Directory; return false; }
    for (const auto& File : Files) if (File != Metadata && !R.Files.Contains(File))
    { Error = TEXT("目录中包含未知文件，未删除：") + Directory; return false; }
    // Check all files before deleting any; never recursively delete a directory.
    for (const auto& File : Files) if (IFileManager::Get().IsReadOnly(*(Directory / File)))
    { Error = TEXT("备份文件只读，未删除：") + Directory; return false; }
    for (const auto& File : R.Files) if (!IFileManager::Get().Delete(*(Directory / File), false, false, true))
    { Error = TEXT("备份删除未完成：") + Directory; return false; }
    if (!IFileManager::Get().Delete(*(Directory / Metadata), false, false, true) || !IFileManager::Get().DeleteDirectory(*Directory))
    { Error = TEXT("备份目录清理未完成：") + Directory; return false; }
    if (!Journal(TEXT("Deleted"), R)) Error = TEXT("备份已删除，但索引日志写入失败。");
    return true;
}
}
