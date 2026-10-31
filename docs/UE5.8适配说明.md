# UE5.8 适配说明

本分支是本仓库公开版本的 UE5.8 适配，保持原有独立插件的模块名和功能范围。它不等同于任何私用合并版；创建器和优化器仍可分别安装。

## 版本选择

- UE5.6：使用 main 分支。
- UE5.8：使用 ue5.8 分支。本次实际构建环境为 UE5.8.4、Win64 Development Editor、Visual Studio14.44、Windows SDK10.0.26100。

## 本次修改与验证

插件描述指定 EngineVersion=5.8.0，保留原功能版本并追加UE5.8标识。
Nanite 设置通过 GetNaniteSettings 访问器读取；补充InputCore、MeshDescription及StaticMeshDescription的直接模块依赖。

三个公开插件（BuildingBlueprintCreator、BuildingBlueprintOptimizer、DatasmithBatchTools）在同一隔离工程中全部编译和链接通过。拟上传源码与实际编译副本逐文件SHA256一致。UE5.6分支的插件源码不作替换。

当前机器正在使用另一个UE工程且可用内存较低，本次未再启动独立UE执行这些公开拆分版本的场景操作测试。因此，本记录仅确认本分支的编译结果及上述Python测试，不引用私用合并插件此前的原生测试作为本分支验证结果。完整工作流、GPU画面及项目打包仍需在工程副本中测试。

## 安装

下载 ue5.8 分支，将对应插件源码放到UE5.8工程的Plugins目录，使用UE5.8工具链编译Editor目标。不要混用UE5.6 DLL。仓库继续只提供源码与说明，不提交引擎文件、测试工程、生成缓存或项目素材。
