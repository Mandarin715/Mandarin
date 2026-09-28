/*Cubism Core 的最小冒烟测试：证明 include 路径与 .lib 选型都对。
  Core 是闭源预编译库，只有真链上并成功调用一次，才能排除"路径错了但没报错"这类假通过。*/
#include <Live2DCubismCore.h>

#include <cstdio>

int main()
{
    const csmVersion version = csmGetVersion();
    if (version == 0)
    {
        std::printf("FAIL: csmGetVersion() 返回 0\n");
        return 1;
    }

    // 版本号打包方式：major << 24 | minor << 16 | patch << 8 | build
    const unsigned major = (version >> 24) & 0xFFu;
    const unsigned minor = (version >> 16) & 0xFFu;
    const unsigned patch = (version >> 8) & 0xFFu;

    // moc3 支持的最新格式版本：确认拿到的不是个空壳库
    const csmMocVersion latestMocVersion = csmGetLatestMocVersion();
    if (latestMocVersion == 0)
    {
        std::printf("FAIL: csmGetLatestMocVersion() 返回 0\n");
        return 1;
    }

    std::printf("OK: Cubism Core %u.%u.%u (raw 0x%08X), latest moc version %u\n",
                major, minor, patch, static_cast<unsigned>(version),
                static_cast<unsigned>(latestMocVersion));
    return 0;
}
