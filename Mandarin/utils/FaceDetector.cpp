#include "FaceDetector.h"

#ifdef Q_OS_WIN

#include <coroutine>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.FaceAnalysis.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstring>
#include <vector>

using namespace winrt;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage::Streams;

// 避免与用  户类 FaceDetector 冲突
namespace wf = Windows::Media::FaceAnalysis;
using WinFaceDetector = wf::FaceDetector;

struct FaceDetector::Impl
{
    bool inited = false;
    bool supported = false;

    void init()
    {
        if (inited)
            return;
        inited = true;
        try
        {
            init_apartment();
        }
        catch (hresult_error const &)
        {
            // 线程可能已由 Qt/OLE 初始化 COM，忽略即可
        }
        try
        {
            supported = WinFaceDetector::IsSupported();
        }
        catch (...)
        {
            supported = false;
        }
    }
};

FaceDetector::FaceDetector() : d(new Impl)
{
    d->init();
}

FaceDetector::~FaceDetector()
{
    delete d;
}

bool FaceDetector::isSupported() const
{
    return d && d->supported;
}

bool FaceDetector::detectFace(const QImage &image, int *outFaceCount)
{
    if (outFaceCount)
        *outFaceCount = 0;
    if (!d || !d->supported || image.isNull())
        return false;

    try
    {
        // 统一到 ARGB32（小端内存即 BGRA），并手动按行复制成紧密包装的 BGRA 缓冲
        const QImage bgra =
            image.format() == QImage::Format_ARGB32
                ? image
                : image.convertToFormat(QImage::Format_ARGB32);
        const int w = bgra.width(), h = bgra.height();
        if (w <= 0 || h <= 0)
            return false;
        const int bpl = bgra.bytesPerLine();
        std::vector<uint8_t> packed(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
        {
            std::memcpy(packed.data() + static_cast<size_t>(y) * w * 4,
                        bgra.constBits() + static_cast<size_t>(y) * bpl,
                        static_cast<size_t>(w) * 4);
        }

        DataWriter writer;
        writer.WriteBytes(
            winrt::array_view<uint8_t const>(packed.data(),
                                             packed.data() + packed.size()));
        auto buffer = writer.DetachBuffer();
        auto bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            buffer, BitmapPixelFormat::Bgra8, w, h);

        auto detector = WinFaceDetector::CreateAsync().get();
        auto faces = detector.DetectFacesAsync(bitmap).get();
        const unsigned count = faces.Size();
        if (outFaceCount)
            *outFaceCount = static_cast<int>(count);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

#else // !Q_OS_WIN

struct FaceDetector::Impl {};

FaceDetector::FaceDetector() : d(new Impl) {}
FaceDetector::~FaceDetector() { delete d; }

bool FaceDetector::isSupported() const
{
    return false;
}

bool FaceDetector::detectFace(const QImage &, int *outFaceCount)
{
    if (outFaceCount)
        *outFaceCount = 0;
    return false;
}

#endif // Q_OS_WIN
