#ifndef FACEDETECTOR_H
#define FACEDETECTOR_H

#include <QImage>

// 摄像头视觉感知：Windows.Media.FaceAnalysis 人脸检测封装。
// v1 仅支持 Windows(Q_OS_WIN)：平时摄像头关闭，仅"怀疑"时短开 2-3 秒抓帧做"是否有人脸"判定。
// 其他平台 isSupported()==false，detectFace() 返回 false（调用方回退键鼠推断）。
class FaceDetector
{
public:
    FaceDetector();
    ~FaceDetector();

    bool isSupported() const;
    // 检测图像中是否有人脸。返回 true 表示检测过程正常执行（人脸数可能为 0）；
    // 返回 false 表示不可用/初始化失败。成功时 *outFaceCount 写入人脸数（可为 0）。
    bool detectFace(const QImage &image, int *outFaceCount = nullptr);

private:
    struct Impl;
    Impl *d = nullptr;
};

#endif // FACEDETECTOR_H
