#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>

#include <array>

namespace {

QDoubleSpinBox* makeFloatBox(double value, double minimum, double maximum, int decimals = 4)
{
  auto* box = new QDoubleSpinBox;
  box->setDecimals(decimals);
  box->setRange(minimum, maximum);
  box->setSingleStep(decimals <= 2 ? 0.01 : 0.001);
  box->setValue(value);
  return box;
}

class ControlCenter final : public QWidget
{
public:
  ControlCenter()
  {
    setWindowTitle(tr("CurvedStereoGS · 曲面立体渲染控制台"));
    resize(940, 780);

    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Qt 管理模型与渲染参数；渲染方式可在曲面三段式与普通平面间切换，立体模式可在立体与非立体间切换。修改参数后点击“应用并重启”。"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* settingsPanel  = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->addWidget(makeRenderModeGroup());
    settingsLayout->addWidget(makeSceneGroup());
    settingsLayout->addWidget(makeDynamicGroup());
    settingsLayout->addWidget(makeOutputGroup());
    settingsLayout->addWidget(makeGeometryGroup());
    settingsLayout->addStretch();
    scroll->setWidget(settingsPanel);
    root->addWidget(scroll, 1);

    auto* actions   = new QHBoxLayout;
    m_startButton   = new QPushButton(tr("启动渲染"));
    m_restartButton = new QPushButton(tr("应用并重启"));
    m_stopButton    = new QPushButton(tr("停止渲染"));
    actions->addWidget(m_startButton);
    actions->addWidget(m_restartButton);
    actions->addWidget(m_stopButton);
    root->addLayout(actions);

    m_status = new QLabel(tr("未启动。请选择 .ply、.spz 或 .csgs 场景（仍支持旧 .vkgs 文件）。"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setPlaceholderText(tr("渲染器日志会显示在这里。"));
    m_log->setMinimumHeight(170);
    root->addWidget(m_log);

    m_renderer = new QProcess(this);
    m_renderer->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_renderer, &QProcess::readyReadStandardOutput, this,
            [this] { appendOutput(QString::fromLocal8Bit(m_renderer->readAllStandardOutput())); });
    connect(m_renderer, &QProcess::started, this, [this] {
      m_status->setText(rendererRunningStatusText());
      updateButtons();
    });
    connect(m_renderer, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
      appendOutput(tr("\n渲染器已退出，退出码：%1\n").arg(exitCode));
      m_status->setText(tr("渲染器已停止。"));
      updateButtons();
    });
    connect(m_renderer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
      m_status->setText(tr("无法启动渲染器：%1").arg(m_renderer->errorString()));
      updateButtons();
    });

    connect(m_startButton, &QPushButton::clicked, this, [this] { startRenderer(); });
    connect(m_restartButton, &QPushButton::clicked, this, [this] {
      stopRenderer();
      startRenderer();
    });
    connect(m_stopButton, &QPushButton::clicked, this, [this] { stopRenderer(); });

    restoreSettings();
    updateButtons();
  }

  ~ControlCenter() override
  {
    saveSettings();
    stopRenderer();
  }

private:
  QGroupBox* makeSceneGroup()
  {
    auto* group  = new QGroupBox(tr("场景"));
    auto* layout = new QHBoxLayout(group);
    m_scene      = new QComboBox;
    m_scene->setEditable(true);
    m_scene->setInsertPolicy(QComboBox::NoInsert);
    m_scene->setPlaceholderText(tr("留空时使用渲染器默认场景；下拉自动列出模型文件夹中的模型"));
    auto* refresh = new QPushButton(tr("刷新"));
    auto* browse  = new QPushButton(tr("浏览…"));
    layout->addWidget(m_scene, 1);
    layout->addWidget(refresh);
    layout->addWidget(browse);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshModelCombo(); });
    connect(m_scene, &QComboBox::currentTextChanged, this, [this] { updateDynamicSequenceForScene(m_scene->currentText()); });
    connect(browse, &QPushButton::clicked, this, [this] {
      const QString filename =
          QFileDialog::getOpenFileName(this, tr("选择 3DGS 场景"), m_scene->currentText(),
                                       tr("3DGS 场景 (*.ply *.spz *.csgs *.vkgs)"));
      if(!filename.isEmpty())
        m_scene->setCurrentText(filename);
    });
    return group;
  }

  QGroupBox* makeDynamicGroup()
  {
    auto* group  = new QGroupBox(tr("动态 4DGS 序列与训练相机"));
    auto* layout = new QVBoxLayout(group);
    auto* sequenceRow = new QWidget;
    auto* sequenceLayout = new QHBoxLayout(sequenceRow);
    sequenceLayout->setContentsMargins(0, 0, 0, 0);
    m_dynamicSequence = new QLineEdit;
    m_dynamicSequence->setPlaceholderText(tr("选择 .dgs 动态序列；留空时渲染静态场景"));
    auto* browse = new QPushButton(tr("浏览…"));
    sequenceLayout->addWidget(m_dynamicSequence, 1);
    sequenceLayout->addWidget(browse);
    layout->addWidget(sequenceRow);
    connect(browse, &QPushButton::clicked, this, [this] {
      const QString filename =
          QFileDialog::getOpenFileName(this, tr("选择动态 4DGS 序列"), m_dynamicSequence->text(),
                                       tr("动态序列 (*.dgs)"));
      if(!filename.isEmpty())
        m_dynamicSequence->setText(filename);
    });

    auto* cameraRow = new QWidget;
    auto* cameraLayout = new QHBoxLayout(cameraRow);
    cameraLayout->setContentsMargins(0, 0, 0, 0);
    m_followTrainingCamera = new QCheckBox(tr("启动时使用训练相机"));
    m_followTrainingCamera->setChecked(true);
    m_trainingCameraMetadata = new QLineEdit;
    m_trainingCameraMetadata->setPlaceholderText(tr("自动查找 train_meta.json，也可手动选择"));
    auto* cameraBrowse = new QPushButton(tr("相机文件…"));
    m_trainingCameraIndex = new QSpinBox;
    m_trainingCameraIndex->setRange(0, 26);
    m_trainingCameraIndex->setValue(8);
    m_trainingCameraIndex->setPrefix(tr("相机 "));
    cameraLayout->addWidget(m_followTrainingCamera);
    cameraLayout->addWidget(m_trainingCameraMetadata, 1);
    cameraLayout->addWidget(cameraBrowse);
    cameraLayout->addWidget(m_trainingCameraIndex);
    layout->addWidget(cameraRow);

    auto updateCameraControls = [this](bool enabled) {
      m_trainingCameraMetadata->setEnabled(enabled);
      m_trainingCameraIndex->setEnabled(enabled);
    };
    connect(m_followTrainingCamera, &QCheckBox::toggled, this, updateCameraControls);
    updateCameraControls(true);
    connect(cameraBrowse, &QPushButton::clicked, this, [this] {
      const QString filename = QFileDialog::getOpenFileName(
          this, tr("选择训练相机元数据"), m_trainingCameraMetadata->text(),
          tr("Dynamic3DGaussians 元数据 (train_meta.json);;JSON 文件 (*.json)"));
      if(!filename.isEmpty())
      {
        m_trainingCameraMetadata->setText(filename);
        m_followTrainingCamera->setEnabled(true);
        m_followTrainingCamera->setChecked(true);
      }
    });
    return group;
  }

  QGroupBox* makeRenderModeGroup()
  {
    auto* group = new QGroupBox(tr("渲染方式"));
    auto* form  = new QFormLayout(group);
    m_warpMode  = new QComboBox;
    m_warpMode->addItem(tr("曲面三段式渲染（Multiview + 三段式曲面 Warp）"), 1);
    m_warpMode->addItem(tr("普通平面渲染（无曲面校正，标准透视）"), 0);
    form->addRow(tr("渲染目标"), m_warpMode);
    connect(m_warpMode, &QComboBox::currentIndexChanged, this, [this] { updateControlEnablement(); });
    return group;
  }

  QGroupBox* makeOutputGroup()
  {
    auto* group = new QGroupBox(tr("输出"));
    auto* form  = new QFormLayout(group);
    m_autoSize  = new QCheckBox(tr("使用主显示器分辨率"));
    m_autoSize->setChecked(true);
    m_width  = new QSpinBox;
    m_height = new QSpinBox;
    for(QSpinBox* box : {m_width, m_height})
    {
      box->setRange(320, 16384);
      box->setSingleStep(64);
    }
    m_width->setValue(1920);
    m_height->setValue(1080);
    auto* dimensions       = new QWidget;
    auto* dimensionsLayout = new QHBoxLayout(dimensions);
    dimensionsLayout->setContentsMargins(0, 0, 0, 0);
    dimensionsLayout->addWidget(m_width);
    dimensionsLayout->addWidget(new QLabel(QStringLiteral("×")));
    dimensionsLayout->addWidget(m_height);
    dimensionsLayout->addStretch();
    form->addRow(tr("分辨率"), m_autoSize);
    form->addRow(tr("自定义宽 × 高"), dimensions);
    connect(m_autoSize, &QCheckBox::toggled, dimensions, [dimensions](bool checked) { dimensions->setEnabled(!checked); });
    dimensions->setEnabled(false);

    m_gpuInterop = new QCheckBox(tr("启用 Vulkan/OpenGL GPU 共享（不支持时自动回退）"));
    m_gpuInterop->setChecked(true);
    m_onlyStereoWindow = new QCheckBox(tr("立体窗口显示后隐藏 Vulkan 预览窗口"));
    m_onlyStereoWindow->setChecked(true);
    m_stereoMode = new QComboBox;
    m_stereoMode->addItem(tr("立体渲染"), 1);
    m_stereoMode->addItem(tr("非立体（单目）"), 0);
    m_outputMode = new QComboBox;
    m_outputMode->addItem(tr("自动：四缓冲优先，不支持时 Vulkan 并排"), 0);
    m_outputMode->addItem(tr("WGL 四缓冲主动立体"), 1);
    m_outputMode->addItem(tr("Vulkan Multiview 并排（Half-SBS）"), 2);
    form->addRow(tr("立体模式"), m_stereoMode);
    form->addRow(tr("输出模式"), m_outputMode);
    form->addRow(m_gpuInterop);
    form->addRow(m_onlyStereoWindow);
    connect(m_stereoMode, &QComboBox::currentIndexChanged, this, [this] { updateControlEnablement(); });
    connect(m_outputMode, &QComboBox::currentIndexChanged, this, [this] { updateControlEnablement(); });
    return group;
  }

  QGroupBox* makeGeometryGroup()
  {
    auto* group        = new QGroupBox(tr("三段式曲面标定 · 单位：米"));
    m_geometryGroup = group;
    // 标定参数很少改动：默认折叠，勾选展开；数值始终参与启动参数
    group->setCheckable(true);
    group->setChecked(false);
    auto* form         = new QFormLayout(group);
    m_screenWidth      = makeFloatBox(1.92, 0.1, 100.0);
    m_radius           = makeFloatBox(0.96, 0.01, 100.0);
    m_bottomHeight     = makeFloatBox(0.8, -100.0, 100.0);
    m_toScreenDistance = makeFloatBox(0.96, 0.01, 100.0);
    m_sections = {makeFloatBox(0.389, 0.0, 100.0), makeFloatBox(1.508, 0.0, 100.0), makeFloatBox(0.502, 0.0, 100.0)};
    m_eyeSeparationMm = makeFloatBox(65.0, 0.0, 150.0, 1);
    m_eyeSeparationMm->setSuffix(tr(" mm"));
    form->addRow(tr("屏幕宽度"), m_screenWidth);
    form->addRow(tr("曲面半径"), m_radius);
    form->addRow(tr("底部高度"), m_bottomHeight);
    form->addRow(tr("眼平面至虚拟屏距离"), m_toScreenDistance);
    form->addRow(tr("水平段长度"), m_sections[0]);
    form->addRow(tr("弧形段长度"), m_sections[1]);
    form->addRow(tr("竖直段长度"), m_sections[2]);
    form->addRow(tr("双眼间距"), m_eyeSeparationMm);
    return group;
  }

  // 渲染方式与立体模式相互独立，输出选项随之联动：
  // 平面渲染禁用三段式标定；非立体禁用输出模式与眼距；
  // Half-SBS 输出不经过 OpenGL，禁用 Vulkan/OpenGL 共享选项。
  void updateControlEnablement()
  {
    if(m_warpMode == nullptr || m_stereoMode == nullptr || m_outputMode == nullptr || m_gpuInterop == nullptr
       || m_onlyStereoWindow == nullptr || m_geometryGroup == nullptr || m_eyeSeparationMm == nullptr)
      return;  // 面板尚未构建完成（首次 addItem 触发信号时）
    const bool curved         = m_warpMode->currentData().toInt() != 0;
    const bool stereo         = m_stereoMode->currentData().toInt() != 0;
    const bool usesQuadBuffer = m_outputMode->currentData().toInt() != 2;
    m_geometryGroup->setEnabled(curved);
    m_eyeSeparationMm->setEnabled(stereo);
    m_outputMode->setEnabled(stereo);
    m_gpuInterop->setEnabled(stereo && usesQuadBuffer);
    m_onlyStereoWindow->setEnabled(stereo && usesQuadBuffer);
  }

  QString rendererRunningStatusText() const
  {
    const bool curved = m_warpMode != nullptr && m_warpMode->currentData().toInt() != 0;
    const bool stereo = m_stereoMode != nullptr && m_stereoMode->currentData().toInt() != 0;
    if(!stereo)
      return curved ? tr("渲染器正在运行：曲面 Warp 单目输出。") : tr("渲染器正在运行：普通平面渲染。");
    return tr("渲染器正在运行；等待立体输出能力检测。");
  }

  QString rendererPath() const
  {
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(
#ifdef Q_OS_WIN
            QStringLiteral("CurvedStereoGS.exe")
#else
            QStringLiteral("CurvedStereoGS")
#endif
        );
  }

  // 场景名解析：下拉列表显示文件名，启动时依次在 models/、assets/ 与程序目录
  // 下定位；绝对路径与空值原样返回。
  QString resolveScenePath(const QString& scene) const
  {
    if(scene.isEmpty() || QFileInfo(scene).isAbsolute())
      return scene;
    const QDir appDir = QDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.absoluteFilePath(QStringLiteral("models/dynamic/") + scene),
        appDir.absoluteFilePath(QStringLiteral("models/static/") + scene),
        appDir.absoluteFilePath(QStringLiteral("../../models/dynamic/") + scene),
        appDir.absoluteFilePath(QStringLiteral("../../models/static/") + scene),
        appDir.absoluteFilePath(QStringLiteral("models/") + scene),
        appDir.absoluteFilePath(QStringLiteral("../../models/") + scene),
        appDir.absoluteFilePath(scene),
    };
    for(const QString& candidate : candidates)
      if(QFileInfo(candidate).exists())
        return candidate;
    return scene;
  }

  // 自动检索模型文件夹（models/ 与 assets/），刷新场景下拉列表
  void refreshModelCombo()
  {
    const QString current = m_scene->currentText();
    m_scene->clear();
    const QDir     appDir  = QDir(QCoreApplication::applicationDirPath());
    const QStringList filters = {QStringLiteral("*.ply"), QStringLiteral("*.spz"),
                                 QStringLiteral("*.csgs"), QStringLiteral("*.vkgs")};
    // 分层模型布局：models/dynamic（canonical .ply + 同名 .dgs + training_cameras）
    // 与 models/static（普通静态场景）；扫描源码树与便携包两套根目录。
    const QStringList folders  = {appDir.absoluteFilePath(QStringLiteral("models/dynamic")),
                                  appDir.absoluteFilePath(QStringLiteral("models/static")),
                                  appDir.absoluteFilePath(QStringLiteral("../../models/dynamic")),
                                  appDir.absoluteFilePath(QStringLiteral("../../models/static")),
                                  appDir.absoluteFilePath(QStringLiteral("models")),
                                  appDir.absoluteFilePath(QStringLiteral("../../models"))};
    for(const QString& folder : folders)
    {
      const QDir dir(folder);
      if(!dir.exists())
        continue;
      const QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Name);
      for(const QFileInfo& entry : entries)
        m_scene->addItem(entry.fileName());
    }
    if(!current.isEmpty())
    {
      const int existing = m_scene->findText(current);
      if(existing >= 0)
        m_scene->setCurrentIndex(existing);
      else
        m_scene->setCurrentText(current);
    }
    updateDynamicSequenceForScene(m_scene->currentText());
  }

  // 场景 → 动态序列联动：静态场景（无同名 .dgs）自动停用动态播放，
  // 有同名 .dgs 的场景自动带上序列（用户手动改过的序列路径优先）。
  void updateDynamicSequenceForScene(const QString& scene)
  {
    const QString scenePath = resolveScenePath(scene);
    if(scenePath.isEmpty())
      return;
    const QFileInfo info(scenePath);
    QString base = info.completeBaseName();
    if(base.endsWith(QStringLiteral("_canonical")))
      base.chop(QStringLiteral("_canonical").length());
    QString stem = info.absolutePath() + QLatin1Char('/') + base;
    if(QFileInfo::exists(stem + QStringLiteral(".dgs")))
    {
      m_dynamicSequence->setEnabled(true);
      if(m_dynamicSequence->text().trimmed().isEmpty()
         || m_dynamicSequence->text().trimmed() == m_lastAutoSequence)
        m_dynamicSequence->setText(stem + QStringLiteral(".dgs"));
    }
    else
    {
      // 静态模型：默认关闭时间序列
      m_dynamicSequence->clear();
      m_dynamicSequence->setEnabled(false);
      m_dynamicSequence->setPlaceholderText(tr("当前场景没有同名动态序列（静态渲染）"));
    }
    m_lastAutoSequence = m_dynamicSequence->text().trimmed();

    QString cameraBase = info.completeBaseName();
    if(cameraBase.endsWith(QStringLiteral("_canonical")))
      cameraBase.chop(QStringLiteral("_canonical").length());
    const QString cameraPath = info.absolutePath() + QStringLiteral("/training_cameras/") + cameraBase
                               + QStringLiteral("/train_meta.json");
    const QFileInfo cameraInfo(cameraPath);
    const QString currentCameraPath = m_trainingCameraMetadata->text().trimmed();
    if(cameraInfo.exists())
    {
      if(currentCameraPath.isEmpty() || currentCameraPath == m_lastAutoTrainingCamera)
        m_trainingCameraMetadata->setText(cameraInfo.absoluteFilePath());
      m_followTrainingCamera->setEnabled(true);
    }
    else if(currentCameraPath.isEmpty() || currentCameraPath == m_lastAutoTrainingCamera)
    {
      m_trainingCameraMetadata->clear();
      m_followTrainingCamera->setEnabled(false);
      m_followTrainingCamera->setChecked(false);
    }
    else
    {
      m_followTrainingCamera->setEnabled(true);
    }
    m_lastAutoTrainingCamera = m_trainingCameraMetadata->text().trimmed();
    const bool cameraEnabled = m_followTrainingCamera->isChecked() && m_followTrainingCamera->isEnabled();
    m_trainingCameraMetadata->setEnabled(cameraEnabled);
    m_trainingCameraIndex->setEnabled(cameraEnabled);
  }

  QStringList rendererArguments() const
  {
    QStringList args;
    auto addNumber = [&args](const QString& name, double value) { args << name << QString::number(value, 'f', 5); };

    const QString scenePath = resolveScenePath(m_scene->currentText().trimmed());
    if(!scenePath.isEmpty())
      args << QStringLiteral("--scene") << scenePath;
    if(!m_autoSize->isChecked())
      args << QStringLiteral("--size") << QString::number(m_width->value()) << QString::number(m_height->value());
    // 渲染方式与立体模式相互独立：曲面开关只控制 Warp 与标定参数，立体开关
    // 只控制 Multiview、眼距与立体输出路径，四种组合均为渲染器合法输入。
    const bool curvedWarp = m_warpMode->currentData().toInt() != 0;
    const bool stereo     = m_stereoMode->currentData().toInt() != 0;
    args << QStringLiteral("--curvedScreenWarp") << (curvedWarp ? QStringLiteral("1") : QStringLiteral("0"));
    if(curvedWarp)
    {
      addNumber(QStringLiteral("--curvedScreenWidth"), m_screenWidth->value());
      addNumber(QStringLiteral("--curvedScreenRadius"), m_radius->value());
      addNumber(QStringLiteral("--curvedScreenBottomHeight"), m_bottomHeight->value());
      addNumber(QStringLiteral("--curvedScreenToScreenDistance"), m_toScreenDistance->value());
      addNumber(QStringLiteral("--curvedScreenSection0"), m_sections[0]->value());
      addNumber(QStringLiteral("--curvedScreenSection1"), m_sections[1]->value());
      addNumber(QStringLiteral("--curvedScreenSection2"), m_sections[2]->value());
    }
    args << QStringLiteral("--stereo") << (stereo ? QStringLiteral("1") : QStringLiteral("0"));
    if(stereo)
    {
      args << QStringLiteral("--activeStereoPresenter") << QStringLiteral("1");
      args << QStringLiteral("--stereoOutputMode") << QString::number(m_outputMode->currentData().toInt());
      args << QStringLiteral("--activeStereoOnlyWindow")
           << (m_onlyStereoWindow->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
      args << QStringLiteral("--activeStereoGpuInterop")
           << (m_gpuInterop->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
      addNumber(QStringLiteral("--eyeSeparation"), m_eyeSeparationMm->value() / 1000.0);
    }
    const QString dynamicSequence = m_dynamicSequence->text().trimmed();
    if(!dynamicSequence.isEmpty())
      args << QStringLiteral("--dynamicSequence") << dynamicSequence;
    const QString cameraMetadata = m_trainingCameraMetadata->text().trimmed();
    if(m_followTrainingCamera->isChecked() && !cameraMetadata.isEmpty())
    {
      args << QStringLiteral("--trainingCameraMetadata") << cameraMetadata;
      args << QStringLiteral("--trainingCameraIndex") << QString::number(m_trainingCameraIndex->value());
    }
    // nvapp 的帧上限默认 1，且同样作用于窗口模式——不传此参数渲染器会在
    // 第一帧后退出。窗口模式必须显式解除（按有符号 int 解析，取安全大值）。
    args << QStringLiteral("--headlessFrameCount") << QString::number(2000000000U);
    return args;
  }

  void startRenderer()
  {
    if(m_renderer->state() != QProcess::NotRunning)
      return;
    if(!QFileInfo::exists(rendererPath()))
    {
      QMessageBox::warning(this, tr("找不到渲染器"), tr("请先编译 CurvedStereoGS；预期路径：\n%1").arg(rendererPath()));
      return;
    }
    const QString scenePath = resolveScenePath(m_scene->currentText().trimmed());
    if(!scenePath.isEmpty() && !QFileInfo::exists(scenePath))
    {
      QMessageBox::warning(this, tr("场景不存在"), tr("找不到场景文件：\n%1").arg(scenePath));
      return;
    }
    const QString dynamicSequencePath = m_dynamicSequence->text().trimmed();
    if(!dynamicSequencePath.isEmpty() && !QFileInfo::exists(dynamicSequencePath))
    {
      QMessageBox::warning(this, tr("动态序列不存在"), tr("找不到动态序列文件：\n%1").arg(dynamicSequencePath));
      return;
    }
    if(m_followTrainingCamera->isChecked())
    {
      const QString metadataPath = m_trainingCameraMetadata->text().trimmed();
      if(metadataPath.isEmpty() || !QFileInfo::exists(metadataPath))
      {
        QMessageBox::warning(this, tr("训练相机文件不存在"),
                             tr("请为当前场景选择 train_meta.json，或取消“启动时使用训练相机”。"));
        return;
      }
    }

    saveSettings();
    m_log->clear();
    m_status->setText(tr("正在启动渲染器…"));
    m_renderer->setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_renderer->start(rendererPath(), rendererArguments());
    updateButtons();
  }

  void stopRenderer()
  {
    if(m_renderer->state() == QProcess::NotRunning)
      return;
    m_renderer->terminate();
    if(!m_renderer->waitForFinished(2500))
    {
      m_renderer->kill();
      m_renderer->waitForFinished(1000);
    }
    updateButtons();
  }

  void appendOutput(const QString& output)
  {
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(output);
    m_log->moveCursor(QTextCursor::End);
    if(output.contains(QStringLiteral("Vulkan side-by-side stereo active"))
      || output.contains(QStringLiteral("falling back to Vulkan side-by-side stereo")))
      m_status->setText(tr("运行中：Vulkan 双眼并排输出，无需四缓冲显卡。"));
    else if(output.contains(QStringLiteral("GPU-shared Vulkan images ->")))
      m_status->setText(tr("运行中：GPU 共享图像已送入 OpenGL 四缓冲。"));
    else if(output.contains(QStringLiteral("no Windows/WGL pixel format supports PFD_STEREO")))
      m_status->setText(tr("当前驱动未提供四缓冲像素格式；请在立体显示目标机上验证。"));
    else if(output.contains(QStringLiteral("CPU readback fallback")))
      m_status->setText(tr("运行中：已回退到 CPU 图像传输。"));
  }

  void updateButtons()
  {
    const bool running = m_renderer->state() != QProcess::NotRunning;
    m_startButton->setEnabled(!running);
    m_restartButton->setEnabled(running);
    m_stopButton->setEnabled(running);
  }

  void restoreSettings()
  {
    QSettings currentSettings;
    QSettings legacySettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("vkgs"),
                             QStringLiteral("vkgs_qt"));
    const QSettings* settings = currentSettings.allKeys().isEmpty() ? &legacySettings : &currentSettings;
    QString scene = settings->value(QStringLiteral("scene")).toString();
    if(!scene.isEmpty() && !QFileInfo::exists(scene))
    {
      const QString relocated = QDir(QCoreApplication::applicationDirPath())
                                    .absoluteFilePath(QStringLiteral("../../models/%1").arg(QFileInfo(scene).fileName()));
      if(QFileInfo::exists(relocated))
        scene = relocated;
    }
    m_scene->setCurrentText(scene);
    if(scene.isEmpty())
      m_scene->setCurrentText(QStringLiteral("basketball_canonical.ply"));
    refreshModelCombo();
    m_dynamicSequence->setText(settings->value(QStringLiteral("dynamicSequence")).toString());
    updateDynamicSequenceForScene(m_scene->currentText());
    const QString savedCameraMetadata = settings->value(QStringLiteral("trainingCameraMetadata")).toString();
    if(!savedCameraMetadata.isEmpty())
      m_trainingCameraMetadata->setText(savedCameraMetadata);
    m_followTrainingCamera->setChecked(settings->value(QStringLiteral("followTrainingCamera"), true).toBool());
    m_trainingCameraIndex->setValue(settings->value(QStringLiteral("trainingCameraIndex"), 8).toInt());
    m_trainingCameraMetadata->setEnabled(m_followTrainingCamera->isChecked() && m_followTrainingCamera->isEnabled());
    m_trainingCameraIndex->setEnabled(m_followTrainingCamera->isChecked() && m_followTrainingCamera->isEnabled());
    m_autoSize->setChecked(settings->value(QStringLiteral("autoSize"), true).toBool());
    m_width->setValue(settings->value(QStringLiteral("width"), 1920).toInt());
    m_height->setValue(settings->value(QStringLiteral("height"), 1080).toInt());
    const int savedOutputMode = m_outputMode->findData(settings->value(QStringLiteral("outputMode"), 0).toInt());
    m_outputMode->setCurrentIndex(savedOutputMode >= 0 ? savedOutputMode : 0);
    const int savedWarpMode = m_warpMode->findData(settings->value(QStringLiteral("curvedWarp"), 1).toInt());
    m_warpMode->setCurrentIndex(savedWarpMode >= 0 ? savedWarpMode : 0);
    const int savedStereoMode = m_stereoMode->findData(settings->value(QStringLiteral("stereoEnabled"), 1).toInt());
    m_stereoMode->setCurrentIndex(savedStereoMode >= 0 ? savedStereoMode : 0);
    m_gpuInterop->setChecked(settings->value(QStringLiteral("gpuInterop"), true).toBool());
    m_onlyStereoWindow->setChecked(settings->value(QStringLiteral("onlyStereoWindow"), true).toBool());
    const std::array<QDoubleSpinBox*, 8> fields = {m_screenWidth, m_radius,      m_bottomHeight, m_toScreenDistance,
                                                   m_sections[0], m_sections[1], m_sections[2],  m_eyeSeparationMm};
    const std::array<const char*, 8>     names  = {"screenWidth", "radius",   "bottomHeight", "toScreenDistance",
                                                   "section0",    "section1", "section2",     "eyeSeparationMm"};
    for(size_t i = 0; i < fields.size(); ++i)
      fields[i]->setValue(settings->value(QLatin1String(names[i]), fields[i]->value()).toDouble());
    updateControlEnablement();
  }

  void saveSettings() const
  {
    QSettings settings;
    settings.setValue(QStringLiteral("scene"), m_scene->currentText());
    settings.setValue(QStringLiteral("dynamicSequence"), m_dynamicSequence->text());
    settings.setValue(QStringLiteral("trainingCameraMetadata"), m_trainingCameraMetadata->text());
    settings.setValue(QStringLiteral("followTrainingCamera"), m_followTrainingCamera->isChecked());
    settings.setValue(QStringLiteral("trainingCameraIndex"), m_trainingCameraIndex->value());
    settings.setValue(QStringLiteral("autoSize"), m_autoSize->isChecked());
    settings.setValue(QStringLiteral("width"), m_width->value());
    settings.setValue(QStringLiteral("height"), m_height->value());
    settings.setValue(QStringLiteral("outputMode"), m_outputMode->currentData().toInt());
    settings.setValue(QStringLiteral("curvedWarp"), m_warpMode->currentData().toInt());
    settings.setValue(QStringLiteral("stereoEnabled"), m_stereoMode->currentData().toInt());
    settings.setValue(QStringLiteral("gpuInterop"), m_gpuInterop->isChecked());
    settings.setValue(QStringLiteral("onlyStereoWindow"), m_onlyStereoWindow->isChecked());
    const std::array<QDoubleSpinBox*, 8> fields = {m_screenWidth, m_radius,      m_bottomHeight, m_toScreenDistance,
                                                   m_sections[0], m_sections[1], m_sections[2],  m_eyeSeparationMm};
    const std::array<const char*, 8>     names  = {"screenWidth", "radius",   "bottomHeight", "toScreenDistance",
                                                   "section0",    "section1", "section2",     "eyeSeparationMm"};
    for(size_t i = 0; i < fields.size(); ++i)
      settings.setValue(QLatin1String(names[i]), fields[i]->value());
  }

  QComboBox*                     m_warpMode{nullptr};
  QComboBox*                     m_stereoMode{nullptr};
  QGroupBox*                     m_geometryGroup{nullptr};
  QComboBox*                     m_scene{nullptr};
  QLineEdit*                     m_dynamicSequence{nullptr};
  QString                        m_lastAutoSequence;  // 上一次自动填入的序列，用于让用户手改优先
  QCheckBox*                     m_followTrainingCamera{nullptr};
  QLineEdit*                     m_trainingCameraMetadata{nullptr};
  QSpinBox*                      m_trainingCameraIndex{nullptr};
  QString                        m_lastAutoTrainingCamera;
  QCheckBox*                     m_autoSize{nullptr};
  QSpinBox*                      m_width{nullptr};
  QSpinBox*                      m_height{nullptr};
  QComboBox*                     m_outputMode{nullptr};
  QCheckBox*                     m_gpuInterop{nullptr};
  QCheckBox*                     m_onlyStereoWindow{nullptr};
  QDoubleSpinBox*                m_screenWidth{nullptr};
  QDoubleSpinBox*                m_radius{nullptr};
  QDoubleSpinBox*                m_bottomHeight{nullptr};
  QDoubleSpinBox*                m_toScreenDistance{nullptr};
  std::array<QDoubleSpinBox*, 3> m_sections{};
  QDoubleSpinBox*                m_eyeSeparationMm{nullptr};
  QPushButton*                   m_startButton{nullptr};
  QPushButton*                   m_restartButton{nullptr};
  QPushButton*                   m_stopButton{nullptr};
  QLabel*                        m_status{nullptr};
  QPlainTextEdit*                m_log{nullptr};
  QProcess*                      m_renderer{nullptr};
};

}  // namespace

int main(int argc, char* argv[])
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("CurvedStereoGS"));
  QCoreApplication::setApplicationName(QStringLiteral("ControlCenter"));
  ControlCenter window;
  window.show();
  return app.exec();
}
