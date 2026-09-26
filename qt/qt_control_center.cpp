#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
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
    auto* intro = new QLabel(tr("Qt 管理模型与曲面标定；可选择四缓冲主动立体或 Vulkan 并排立体。修改参数后点击“应用并重启”。"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* settingsPanel  = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->addWidget(makeSceneGroup());
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
      m_status->setText(tr("渲染器正在运行；等待立体输出能力检测。"));
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
    m_scene      = new QLineEdit;
    m_scene->setPlaceholderText(tr("选择点云或项目文件；留空时使用渲染器默认场景"));
    auto* browse = new QPushButton(tr("浏览…"));
    layout->addWidget(m_scene, 1);
    layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
      const QString filename =
          QFileDialog::getOpenFileName(this, tr("选择 3DGS 场景"), m_scene->text(),
                                       tr("3DGS 场景 (*.ply *.spz *.csgs *.vkgs)"));
      if(!filename.isEmpty())
        m_scene->setText(filename);
    });
    return group;
  }

  QGroupBox* makeOutputGroup()
  {
    auto* group = new QGroupBox(tr("立体输出"));
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
    m_outputMode = new QComboBox;
    m_outputMode->addItem(tr("自动：四缓冲优先，不支持时 Vulkan 并排"), 0);
    m_outputMode->addItem(tr("WGL 四缓冲主动立体"), 1);
    m_outputMode->addItem(tr("Vulkan Multiview 并排（Half-SBS）"), 2);
    form->addRow(tr("输出模式"), m_outputMode);
    form->addRow(m_gpuInterop);
    form->addRow(m_onlyStereoWindow);
    connect(m_outputMode, &QComboBox::currentIndexChanged, this, [this] {
      const bool usesQuadBuffer = m_outputMode->currentData().toInt() != 2;
      m_gpuInterop->setEnabled(usesQuadBuffer);
      m_onlyStereoWindow->setEnabled(usesQuadBuffer);
    });
    return group;
  }

  QGroupBox* makeGeometryGroup()
  {
    auto* group        = new QGroupBox(tr("三段式曲面标定 · 单位：米"));
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

  QStringList rendererArguments() const
  {
    QStringList args;
    auto addNumber = [&args](const QString& name, double value) { args << name << QString::number(value, 'f', 5); };

    if(!m_scene->text().trimmed().isEmpty())
      args << QStringLiteral("--scene") << m_scene->text().trimmed();
    if(!m_autoSize->isChecked())
      args << QStringLiteral("--size") << QString::number(m_width->value()) << QString::number(m_height->value());
    args << QStringLiteral("--activeStereoPresenter") << QStringLiteral("1");
    args << QStringLiteral("--stereoOutputMode") << QString::number(m_outputMode->currentData().toInt());
    args << QStringLiteral("--activeStereoOnlyWindow")
         << (m_onlyStereoWindow->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    args << QStringLiteral("--activeStereoGpuInterop")
         << (m_gpuInterop->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    args << QStringLiteral("--curvedScreenWarp") << QStringLiteral("1");
    addNumber(QStringLiteral("--curvedScreenWidth"), m_screenWidth->value());
    addNumber(QStringLiteral("--curvedScreenRadius"), m_radius->value());
    addNumber(QStringLiteral("--curvedScreenBottomHeight"), m_bottomHeight->value());
    addNumber(QStringLiteral("--curvedScreenToScreenDistance"), m_toScreenDistance->value());
    addNumber(QStringLiteral("--curvedScreenSection0"), m_sections[0]->value());
    addNumber(QStringLiteral("--curvedScreenSection1"), m_sections[1]->value());
    addNumber(QStringLiteral("--curvedScreenSection2"), m_sections[2]->value());
    addNumber(QStringLiteral("--eyeSeparation"), m_eyeSeparationMm->value() / 1000.0);
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
    const QString scenePath = m_scene->text().trimmed();
    if(!scenePath.isEmpty() && !QFileInfo::exists(scenePath))
    {
      QMessageBox::warning(this, tr("场景不存在"), tr("找不到场景文件：\n%1").arg(scenePath));
      return;
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
    m_scene->setText(scene);
    m_autoSize->setChecked(settings->value(QStringLiteral("autoSize"), true).toBool());
    m_width->setValue(settings->value(QStringLiteral("width"), 1920).toInt());
    m_height->setValue(settings->value(QStringLiteral("height"), 1080).toInt());
    const int savedOutputMode = m_outputMode->findData(settings->value(QStringLiteral("outputMode"), 0).toInt());
    m_outputMode->setCurrentIndex(savedOutputMode >= 0 ? savedOutputMode : 0);
    m_gpuInterop->setChecked(settings->value(QStringLiteral("gpuInterop"), true).toBool());
    m_onlyStereoWindow->setChecked(settings->value(QStringLiteral("onlyStereoWindow"), true).toBool());
    const std::array<QDoubleSpinBox*, 8> fields = {m_screenWidth, m_radius,      m_bottomHeight, m_toScreenDistance,
                                                   m_sections[0], m_sections[1], m_sections[2],  m_eyeSeparationMm};
    const std::array<const char*, 8>     names  = {"screenWidth", "radius",   "bottomHeight", "toScreenDistance",
                                                   "section0",    "section1", "section2",     "eyeSeparationMm"};
    for(size_t i = 0; i < fields.size(); ++i)
      fields[i]->setValue(settings->value(QLatin1String(names[i]), fields[i]->value()).toDouble());
  }

  void saveSettings() const
  {
    QSettings settings;
    settings.setValue(QStringLiteral("scene"), m_scene->text());
    settings.setValue(QStringLiteral("autoSize"), m_autoSize->isChecked());
    settings.setValue(QStringLiteral("width"), m_width->value());
    settings.setValue(QStringLiteral("height"), m_height->value());
    settings.setValue(QStringLiteral("outputMode"), m_outputMode->currentData().toInt());
    settings.setValue(QStringLiteral("gpuInterop"), m_gpuInterop->isChecked());
    settings.setValue(QStringLiteral("onlyStereoWindow"), m_onlyStereoWindow->isChecked());
    const std::array<QDoubleSpinBox*, 8> fields = {m_screenWidth, m_radius,      m_bottomHeight, m_toScreenDistance,
                                                   m_sections[0], m_sections[1], m_sections[2],  m_eyeSeparationMm};
    const std::array<const char*, 8>     names  = {"screenWidth", "radius",   "bottomHeight", "toScreenDistance",
                                                   "section0",    "section1", "section2",     "eyeSeparationMm"};
    for(size_t i = 0; i < fields.size(); ++i)
      settings.setValue(QLatin1String(names[i]), fields[i]->value());
  }

  QLineEdit*                     m_scene{nullptr};
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
