#ifndef LIVE2DCUBISMRUNTIME_H
#define LIVE2DCUBISMRUNTIME_H

#include <QString>

class QOpenGLContext;

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

/***本进程**所有离屏 GL 上下文共享的根上下文**（进程内单例，随 acquire/release 的
   引用计数同生共死）。

   为什么必须有它 —— 这是"同进程第二个渲染器渲染为空白"那个既存缺陷的根因所在：
   Cubism 框架里有**进程级单例**持有 GL 对象（`CubismShader_OpenGLES2` 缓存着色器程序名与
   uniform 位置、`CubismOffscreenManager_OpenGLES2` 缓存离屏渲染目标）。GL 的对象名
   （程序名/贴图名/FBO 名）是**按上下文**解析的：在上下文甲里创建的名字，到了上下文乙
   里什么都不是。而 `glUseProgram` 对不存在的名字**不报错、只是静默失败**，
   于是第二个上下文里一条三角形都发不出去 —— 数据全对、画面全空，从现象几乎无法反推。

   让所有上下文**共享 GL 对象**是唯一能同时满足"框架按进程级单例缓存"与"每个渲染器
   各有自己的 FBO/贴图"两件事的做法：名字在共享组里全局唯一、人人都认得，
   而每组渲染目标仍然各归各的实例（互不打架）。这也是官方样例（一个进程一个上下文）
   之外的用法唯一正确的接法。

   调用方必须**在本进程任何离屏上下文 create() 之前**拿到它，并用它作为 setShareContext。
   生命周期：引用计数归零时随框架一起销毁 —— 渲染器析构时先释放自己的上下文，
   `release()` 才可能删掉这个根上下文，顺序天然安全。*/
QOpenGLContext *shareContext();
} // namespace Live2DCubismRuntime

#endif // LIVE2DCUBISMRUNTIME_H
