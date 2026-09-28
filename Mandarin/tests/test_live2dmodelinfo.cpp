#include <QtTest>

#include "../utils/Live2DModelInfo.h"

class TestLive2DModelInfo : public QObject
{
    Q_OBJECT

  private slots:
    void parseKeepsChinesePathsAndGroups();
    void parseRejectsMissingMoc();
    void expressionParamIdReadsWatermarkParam();
    void fingerprintChangesWithBytes();
};

/*模型路径/贴图清单原样保留中文，Groups 正确读出*/
void TestLive2DModelInfo::parseKeepsChinesePathsAndGroups()
{
    const QByteArray model3 = R"({
        "Version": 3,
        "FileReferences": {
            "Moc": "樱花miku.moc3",
            "Textures": ["樱花miku.4096/texture_00.png", "樱花miku.4096/texture_01.png"],
            "Physics": "樱花miku.physics3.json",
            "DisplayInfo": "樱花miku.cdi3.json"
        },
        "Groups": [
            { "Target": "Parameter", "Name": "LipSync", "Ids": [] },
            { "Target": "Parameter", "Name": "EyeBlink", "Ids": ["ParamEyeROpen", "ParamEyeLOpen"] }
        ]
    })";
    const QByteArray cdi3 = R"({
        "Parameters": [
            { "Id": "ParamMouthOpenY", "Name": "嘴　张开和闭合", "GroupId": "" }
        ]
    })";

    Live2DModelInfo info;
    QString error;
    QVERIFY2(info.parse(model3, cdi3, &error), qPrintable(error));

    QCOMPARE(info.mocPath(), QStringLiteral("樱花miku.moc3"));
    QCOMPARE(info.physicsPath(), QStringLiteral("樱花miku.physics3.json"));
    QCOMPARE(info.displayInfoPath(), QStringLiteral("樱花miku.cdi3.json"));
    QCOMPARE(info.texturePaths().size(), 2);
    QCOMPARE(info.texturePaths().first(), QStringLiteral("樱花miku.4096/texture_00.png"));

    // miku 的 LipSync 组是空的（方案里必须显式指定口型参数）
    QVERIFY(info.groupIds(QStringLiteral("LipSync")).isEmpty());
    QCOMPARE(info.groupIds(QStringLiteral("EyeBlink")),
             QStringList({QStringLiteral("ParamEyeROpen"), QStringLiteral("ParamEyeLOpen")}));

    QVERIFY(info.hasParameter(QStringLiteral("ParamMouthOpenY")));
    QCOMPARE(info.parameterDisplayName(QStringLiteral("ParamMouthOpenY")),
             QStringLiteral("嘴　张开和闭合"));
    QCOMPARE(info.parameterCount(), 1);
}

/*缺少 Moc 必须明确失败，不能静默返回一个空模型*/
void TestLive2DModelInfo::parseRejectsMissingMoc()
{
    Live2DModelInfo info;
    QString error;
    QVERIFY(!info.parse(QByteArrayLiteral("{\"Version\":3,\"FileReferences\":{}}"),
                        QByteArray(), &error));
    QVERIFY(!error.isEmpty());

    // 连 JSON 都不是
    Live2DModelInfo broken;
    QString brokenError;
    QVERIFY(!broken.parse(QByteArrayLiteral("not json"), QByteArray(), &brokenError));
    QVERIFY(!brokenError.isEmpty());
}

/*水印参数从表情文件里读出来，不硬编码 Param137*/
void TestLive2DModelInfo::expressionParamIdReadsWatermarkParam()
{
    const QByteArray watermark = R"({
        "Type": "Live2D Expression",
        "Parameters": [ { "Id": "Param137", "Value": 1.0, "Blend": "Add" } ]
    })";
    QCOMPARE(Live2DModelInfo::expressionParamId(watermark), QStringLiteral("Param137"));
    QVERIFY(Live2DModelInfo::expressionParamId(QByteArrayLiteral("{}")).isEmpty());
    QVERIFY(Live2DModelInfo::expressionParamId(QByteArrayLiteral("not json")).isEmpty());
}

/*指纹随字节变化——换模型后参数映射不匹配要能查出来*/
void TestLive2DModelInfo::fingerprintChangesWithBytes()
{
    const QString a = Live2DModelInfo::fingerprint(QByteArrayLiteral("moc3-A"));
    const QString b = Live2DModelInfo::fingerprint(QByteArrayLiteral("moc3-A"));
    const QString c = Live2DModelInfo::fingerprint(QByteArrayLiteral("moc3-B"));
    QCOMPARE(a, b);
    QVERIFY(a != c);
    QVERIFY(a.startsWith(QStringLiteral("sha256:")));
    QCOMPARE(a.size(), 7 + 64);
}

QTEST_MAIN(TestLive2DModelInfo)
#include "test_live2dmodelinfo.moc"
