#include "Live2DCubismRuntime.h"

#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

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
}

bool isRunning()
{
    return g_refCount > 0;
}
} // namespace Live2DCubismRuntime
