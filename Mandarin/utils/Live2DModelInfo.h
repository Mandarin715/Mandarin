#ifndef LIVE2DMODELINFO_H
#define LIVE2DMODELINFO_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

/*Live2D 模型资源清单（纯解析：不依赖 Cubism SDK，也不读文件——便于单元测试）
  设计前提：模型目录/文件名可能含中文或空格，一律由调用方用 QFile 读成字节再交进来；
  绝不把路径交给 Cubism SDK（其内部走 csmString/fopen，Windows 下打不开 UTF-8 中文路径）。*/
class Live2DModelInfo
{
  public:
    /*解析 model3.json；cdi3Json 可为空。失败时写 error 并返回 false。*/
    bool parse(const QByteArray &model3Json, const QByteArray &cdi3Json,
               QString *error = nullptr);

    QString mocPath() const { return m_mocPath; }
    QString physicsPath() const { return m_physicsPath; }
    QString displayInfoPath() const { return m_displayInfoPath; }
    QStringList texturePaths() const { return m_texturePaths; }

    /*model3.json 的 Groups（如 LipSync / EyeBlink）；未声明则返回空表*/
    QStringList groupIds(const QString &groupName) const;

    /*cdi3.json 里的参数 ID → 显示名*/
    bool hasParameter(const QString &id) const { return m_paramNames.contains(id); }
    QString parameterDisplayName(const QString &id) const { return m_paramNames.value(id); }
    int parameterCount() const { return m_paramNames.size(); }

    /*取 .exp3.json 驱动的参数 ID（水印.exp3.json → Param137）。
      用途：水印必须"显式置 0"关闭，而不是靠"不加载该表情"。*/
    static QString expressionParamId(const QByteArray &exp3Json);

    /*对 .moc3 字节算 sha256，供模型指纹校验*/
    static QString fingerprint(const QByteArray &moc3Bytes);

  private:
    QString m_mocPath;
    QString m_physicsPath;
    QString m_displayInfoPath;
    QStringList m_texturePaths;
    QHash<QString, QStringList> m_groups;
    QHash<QString, QString> m_paramNames;
};

#endif // LIVE2DMODELINFO_H
