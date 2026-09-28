/*Cubism Framework（OpenGL 后端）的编译与链接验证。
  这一条测试同时钉住三件最容易出错的事：
  1) Framework 只编了 OpenGL 一个后端 —— 若有人图省事把 Framework/src 下的 .cpp 全部 glob 进来，
     会连带 D3D9/D3D11/Vulkan 一起编，Vulkan 还需要 Vulkan SDK，构建当场就炸；
  2) GLEW 的 include 路径与 GLEW_STATIC 定义正确；
  3) CSM_TARGET_WIN_GL 定义正确 —— 缺它这个头会走去 GLES2 分支，Windows 上直接编不过。*/
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

#include <Math/CubismMath.hpp>

#include <cstdio>

int main()
{
    using Live2D::Cubism::Framework::CubismMath;

    // 非 inline 的静态成员函数，定义在 CubismMath.cpp 里：
    // 能调到它就证明 Framework 静态库真的被链接进来了，而不只是头文件能找到。
    if (CubismMath::Clamp(5, 0, 1) != 1)
    {
        std::printf("FAIL: CubismMath::Clamp(5, 0, 1) != 1\n");
        return 1;
    }
    if (CubismMath::Clamp(-3, 0, 10) != 0)
    {
        std::printf("FAIL: CubismMath::Clamp(-3, 0, 10) != 0\n");
        return 1;
    }

    std::printf("OK: Cubism Framework (OpenGL backend) linked; CubismMath::Clamp works\n");
    return 0;
}
