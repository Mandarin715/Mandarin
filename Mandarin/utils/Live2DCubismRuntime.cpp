#include "Live2DCubismRuntime.h"

#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

#include <cstring>
#include <malloc.h>
#include <string>

using Live2D::Cubism::Framework::csmByte;
using Live2D::Cubism::Framework::csmSizeInt;
using Live2D::Cubism::Framework::csmSizeType;
using Live2D::Cubism::Framework::csmUint32;

namespace
{
/*Cubism 要求真正的对齐分配：moc 缓冲需 64 字节对齐并被原地 revive，
  模型缓冲需 16 字节对齐（见 Core 的 csmAlignofMoc / csmAlignofModel）。
  注意 AllocateAligned/DeallocateAligned 必须成对用 _aligned_*，不能与 malloc/free 混用。*/
class CubismAllocator : public Live2D::Cubism::Framework::ICubismAllocator
{
  public:
    void *Allocate(const csmSizeType size) override { return malloc(size); }

    void Deallocate(void *memory) override { free(memory); }

    void *AllocateAligned(const csmSizeType size, const csmUint32 alignment) override
    {
        return _aligned_malloc(size, alignment);
    }

    void DeallocateAligned(void *alignedMemory) override { _aligned_free(alignedMemory); }
};

CubismAllocator g_allocator;
int g_refCount = 0;

/*⚠️ 必须是**进程级生命周期**的对象。
  CubismFramework::StartUp 只保存 option 的**指针**（s_option = option），不会拷贝；
  若把 Option 放在 acquire() 的栈上，函数一返回框架读到的是悬空内存，
  症状是着色器阶段报 "File loader is not set."（因为 LoadFileFunction 读出来是空），
  而不会崩溃 —— 极难排查。样例把它做成 LAppDelegate 的成员也是同一个原因。*/
Live2D::Cubism::Framework::CubismFramework::Option g_option;

/*框架的日志出口。级别由 Option.LoggingLevel 控制，这里统一转到 qWarning。*/
void cubismLog(const char *message)
{
    if (message != nullptr)
        qWarning("[Cubism] %s", message);
}

/*框架内部的资源读取回调。它只用于**一件事**：加载 GL 着色器源码
  （CubismShader_OpenGLES2.cpp 里硬编码的相对路径 "FrameworkShaders/VertShaderSrc.vert" 等）。
  模型数据（moc3/model3/physics3/exp3/motion3/贴图）框架一概不碰磁盘，全部由我们以内存传入。

  两个必须处理的细节：
  1. 路径是**相对进程 CWD** 的，而 ctest 的 CWD 与可执行文件目录通常不是同一个，
     所以这里补一层"退回可执行文件目录"的解析；
  2. 用 QFile 而不是 fopen：虽然着色器路径是 ASCII，但统一走 Qt 可以同时覆盖中文路径。*/
QString resolveAssetPath(const QString &path)
{
    if (QFileInfo(path).isAbsolute() || QFile::exists(path))
        return path;
    const QString besideExecutable =
        QCoreApplication::applicationDirPath() + QLatin1Char('/') + path;
    if (QFile::exists(besideExecutable))
        return besideExecutable;
    return path;
}

csmByte *loadFile(const std::string filePath, csmSizeInt *outSize)
{
    if (outSize != nullptr)
        *outSize = 0;

    const QString rawPath =
        QString::fromUtf8(filePath.c_str(), static_cast<int>(filePath.size()));
    const QString path = resolveAssetPath(rawPath);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning("[Cubism] 资源打不开：%s", qPrintable(path));
        return nullptr;
    }

    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty())
    {
        qWarning("[Cubism] 资源为空：%s", qPrintable(path));
        return nullptr;
    }

    auto *buffer = new csmByte[static_cast<size_t>(bytes.size())];
    std::memcpy(buffer, bytes.constData(), static_cast<size_t>(bytes.size()));
    if (outSize != nullptr)
        *outSize = static_cast<csmSizeInt>(bytes.size());
    return buffer;
}

void releaseBytes(csmByte *byteData)
{
    delete[] byteData;
}

/*==================== 离屏 GL 上下文的共享根 ====================

  见 Live2DCubismRuntime.h 里 shareContext() 的长说明：Cubism 框架用**进程级单例**
  缓存 GL 对象（着色器程序/uniform 位置、离屏渲染目标），而 GL 名字是按上下文解析的，
  所以多个离屏上下文必须共享 GL 对象才可能都画得出来。

  这里的根上下文自己带一个 QOffscreenSurface、格式与渲染器请求的完全一致
  （2.0 / NoProfile / alpha 8 / 无多重采样）：不用 QOpenGLContext::globalShareContext()，
  是因为那个"全局共享上下文"的格式由 Qt 按默认表面格式决定，本进程里没有任何窗口，
  不能假定它一定建得起来。自己建一个，格式与用途都是明写的。

  它**不需要**长期 current 在任何线程上：它只是共享组的根，负责让 GL 名字全局唯一。
  销毁顺序由引用计数保证 —— 最后一个渲染器析构时先释放自己的上下文，才轮到 release()
  走到这里（见 shareContextRoot 的说明）。*/
struct SharedGlRoot
{
    QOpenGLContext *context = nullptr;
    QOffscreenSurface *surface = nullptr;

    bool ensure(QString *error)
    {
        if (context != nullptr)
            return true;

        /*同样的格式：Cubism 走 ES2 风格接口，2.0 上下文是实测唯一能正常出图的
          （3.0 会让绘制阶段报 GL_INVALID_OPERATION，见 Live2DOffscreenRenderer::initializeGl）。
          共享只要求"名字空间一致"，格式一致让根上下文与子上下文完全同源，少一类变量。*/
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setProfile(QSurfaceFormat::NoProfile);
        format.setVersion(2, 0);
        format.setAlphaBufferSize(8);
        format.setSamples(0);

        surface = new QOffscreenSurface();
        surface->setFormat(format);
        surface->create();
        if (!surface->isValid())
        {
            delete surface;
            surface = nullptr;
            if (error != nullptr)
                *error = QStringLiteral("共享根上下文：QOffscreenSurface 创建失败");
            return false;
        }

        context = new QOpenGLContext();
        context->setFormat(format);
        if (!context->create())
        {
            delete context;
            context = nullptr;
            delete surface;
            surface = nullptr;
            if (error != nullptr)
                *error = QStringLiteral("共享根上下文：QOpenGLContext 创建失败");
            return false;
        }
        return true;
    }

    /*只在最后一个引用释放时调用（此刻所有渲染器上下文都已析构）。*/
    void destroy()
    {
        delete context;
        context = nullptr;
        delete surface;
        surface = nullptr;
    }
};

SharedGlRoot g_sharedGlRoot;
} // namespace

namespace Live2DCubismRuntime
{
bool acquire(QString *error)
{
    if (g_refCount > 0)
    {
        ++g_refCount;
        return true;
    }

    Live2D::Cubism::Framework::CubismFramework::Option &option = g_option;
    option.LogFunction = cubismLog;
    option.LoggingLevel = Live2D::Cubism::Framework::CubismFramework::Option::LogLevel_Warning;
    option.LoadFileFunction = loadFile;
    option.ReleaseBytesFunction = releaseBytes;

    if (!Live2D::Cubism::Framework::CubismFramework::StartUp(&g_allocator, &option))
    {
        if (error != nullptr)
            *error = QStringLiteral("CubismFramework::StartUp 失败");
        return false;
    }

    Live2D::Cubism::Framework::CubismFramework::Initialize();
    g_refCount = 1;
    return true;
}

void release()
{
    if (g_refCount <= 0)
        return;
    if (--g_refCount > 0)
        return;

    Live2D::Cubism::Framework::CubismFramework::Dispose();
    Live2D::Cubism::Framework::CubismFramework::CleanUp();
    /*共享根上下文也随框架一起收尾（此时所有用到它的渲染器都已析构 —— 它们各自在
       析构里释放自己的上下文并调 release()，最后一个才会走到这里）。*/
    g_sharedGlRoot.destroy();
}

bool isRunning()
{
    return g_refCount > 0;
}

QOpenGLContext *shareContext()
{
    if (!g_sharedGlRoot.ensure(nullptr))
        return nullptr;
    return g_sharedGlRoot.context;
}
} // namespace Live2DCubismRuntime
