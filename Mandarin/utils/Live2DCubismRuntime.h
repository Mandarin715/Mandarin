#ifndef LIVE2DCUBISMRUNTIME_H
#define LIVE2DCUBISMRUNTIME_H

#include <QString>

/*Cubism Native 框架的进程级生命周期。
  CubismFramework::StartUp / Initialize 全局只能来一次，且必须成对收尾，
  所以这里用引用计数包住，供多个渲染器实例（以及测试）安全共用。

  另一个关键职责是**资源读取钩子**：框架自己加载 physics/motion/expression 时会回调
  LoadFileFunction（路径是 UTF-8 字节串）。Cubism 内部走 fopen，在 Windows 上打不开中文路径，
  因此这个回调用 QFile 实现，把中文路径问题从框架内部一并解决掉。*/
namespace Live2DCubismRuntime
{
/*首次调用完成 StartUp + Initialize；重复调用只增加引用计数。失败返回 false 并写 error。*/
bool acquire(QString *error = nullptr);

/*释放一次引用；计数归零时 Dispose + CleanUp。*/
void release();

/*当前框架是否已启动（测试用）*/
bool isRunning();
} // namespace Live2DCubismRuntime

#endif // LIVE2DCUBISMRUNTIME_H
