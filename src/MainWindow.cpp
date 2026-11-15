#include "MainWindow.h"
#include "MdiIcons.h"
#include "ThemeManager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QPainter>
#include <QtWidgets>
#include <memory>
#include <algorithm>
using spotty::MdiIcons;
namespace mdi = spotty::mdi;
namespace {
constexpr char32_t MdiLayersOutline = 0xF09FE;
template <class T, class... A> T *make(A &&...a) {
  return std::make_unique<T>(std::forward<A>(a)...).release();
}
void writeFile(const QString &path, const QByteArray &data) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() ||
      !f.commit())
    throw std::runtime_error("Cannot save file");
}
} // namespace
class HexTable : public QTableView {
public:
  using QTableView::QTableView;
protected:
  void paintEvent(QPaintEvent *event) override {
    QTableView::paintEvent(event);
    if (!model() || model()->columnCount() < 3) return;
    QPainter painter(viewport());
    const auto *theme = spotty::ThemeManager::instance();
    painter.setPen(theme ? theme->colors().controlBorder : palette().color(QPalette::Mid));
    for (const auto column : {1, model()->columnCount() - 1}) {
      const auto x = columnViewportPosition(column);
      if (x > 0 && x < viewport()->width())
        painter.drawLine(x, 0, x, viewport()->height());
    }
  }
};
class HexDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override {
    auto editor = make<QLineEdit>(parent);
    editor->setMaxLength(2);
    editor->setValidator(make<QRegularExpressionValidator>(QRegularExpression("[0-9A-Fa-f]{2}"), editor));
    editor->setAlignment(Qt::AlignCenter);
    return editor;
  }
  void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    QStyledItemDelegate::paint(painter, option, index);
    if (index.data(Qt::BackgroundRole).isValid()) {
      const auto *theme = spotty::ThemeManager::instance();
      auto color = theme ? theme->colors().errorText : QColor(Qt::red);
      color.setAlpha(100);
      painter->fillRect(option.rect, color);
    }
  }
};
class HexModel : public QAbstractTableModel {
public:
  Firmware firmware;
  Firmware other;
  bool comparing = false;
  bool editable = false;
  int rowBytes = 16;
  QVector<quint32> rows;
  quint32 pageStart = 0;
  quint32 pageSize = 0;
  explicit HexModel(QObject *p) : QAbstractTableModel(p) {}
  void setFirmware(const Firmware &f, const Firmware &peer = {}, bool compare = false, quint32 start = 0, quint32 size = 0) {
    beginResetModel();
    firmware = f;
    other = peer;
    comparing = compare;
    pageStart = start;
    pageSize = size;
    rows.clear();
    const quint64 pageEnd = quint64(pageStart) + pageSize;
    for (const auto &source : {f, compare ? peer : Firmware{}})
    for (const auto &segment : source.segments) {
      const quint64 begin = segment.address;
      const quint64 end = begin + segment.data.size();
      const quint64 visibleBegin = pageSize ? qMax(begin, quint64(pageStart)) : begin;
      const quint64 visibleEnd = pageSize ? qMin(end, pageEnd) : end;
      if (visibleBegin >= visibleEnd)
        continue;
      for (auto address = visibleBegin & ~quint64(rowBytes - 1); address < visibleEnd; address += rowBytes)
        rows.append(static_cast<quint32>(address));
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    endResetModel();
  }
  static int byteAt(const Firmware &f, quint64 address) {
    for (const auto &segment : f.segments)
      if (address >= segment.address && address - segment.address < quint64(segment.data.size()))
        return static_cast<unsigned char>(segment.data.at(static_cast<qsizetype>(address - segment.address)));
    return -1;
  }
  int rowCount(const QModelIndex &p = {}) const override {
    return p.isValid() ? 0 : static_cast<int>(rows.size());
  }
  int columnCount(const QModelIndex &p = {}) const override {
    return p.isValid() ? 0 : rowBytes + 2;
  }
  QVariant headerData(int s, Qt::Orientation o, int role) const override {
    if (role != Qt::DisplayRole || o != Qt::Horizontal)
      return {};
    return s == 0    ? QString("Address")
           : s == rowBytes + 1 ? QString("ASCII")
                     : QString::number(s - 1, 16).toUpper();
  }
  QVariant data(const QModelIndex &i, int role) const override {
    if (!i.isValid())
      return {};
    if (role == Qt::TextAlignmentRole)
      return int(Qt::AlignCenter);
    const auto address = quint64(rows[i.row()]);
    if (role == Qt::BackgroundRole && comparing && i.column() >= 1 && i.column() <= rowBytes) {
      const auto a = byteAt(firmware, address + i.column() - 1);
      const auto b = byteAt(other, address + i.column() - 1);
      if (a != b) {
        const auto *theme = spotty::ThemeManager::instance();
        return QBrush((theme ? theme->colors().errorText : QColor(Qt::red)).darker(250));
      }
    }
    if (role == Qt::ToolTipRole && comparing && i.column() >= 1 && i.column() <= rowBytes &&
        byteAt(firmware, address + i.column() - 1) != byteAt(other, address + i.column() - 1))
      return QString("Different or missing byte");
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    if (i.column() == 0)
      return QString("%1")
          .arg(address, 8, 16, QChar('0'))
          .toUpper();
    if (i.column() == rowBytes + 1) {
      QString a;
      for (int n = 0; n < rowBytes; ++n) {
        const auto b = byteAt(firmware, address + n);
        a += b < 0 ? QChar(' ') : b >= 32 && b < 127 ? QChar(b) : QChar('.');
      }
      return a;
    }
    const auto n = byteAt(firmware, address + i.column() - 1);
    if (n < 0)
      return {};
    return QString("%1")
        .arg(n, 2, 16, QChar('0'))
        .toUpper();
  }
  Qt::ItemFlags flags(const QModelIndex &i) const override {
    auto result = QAbstractTableModel::flags(i);
    if (editable && i.isValid() && i.column() >= 1 && i.column() <= rowBytes &&
        byteAt(firmware, quint64(rows[i.row()]) + i.column() - 1) >= 0)
      result |= Qt::ItemIsEditable;
    return result;
  }
  bool setData(const QModelIndex &i, const QVariant &value, int role) override {
    if (role != Qt::EditRole || !(flags(i) & Qt::ItemIsEditable)) return false;
    static const QRegularExpression hex("^[0-9a-fA-F]{2}$");
    const auto text = value.toString();
    if (!hex.match(text).hasMatch()) return false;
    const auto address = quint64(rows[i.row()]) + i.column() - 1;
    for (auto &segment : firmware.segments) {
      if (address < segment.address || address - segment.address >= quint64(segment.data.size())) continue;
      const auto offset = static_cast<qsizetype>(address - segment.address);
      const auto byte = static_cast<char>(text.toUInt(nullptr, 16));
      if (segment.data[offset] == byte) return true;
      segment.data[offset] = byte;
      emit dataChanged(i, i, {Qt::DisplayRole, Qt::EditRole, Qt::BackgroundRole});
      emit dataChanged(index(i.row(), rowBytes + 1), index(i.row(), rowBytes + 1), {Qt::DisplayRole});
      return true;
    }
    return false;
  }
  QModelIndex atAddress(quint32 a) const {
    const auto mask = quint32(rowBytes - 1);
    const auto row = std::lower_bound(rows.cbegin(), rows.cend(), a & ~mask);
    if (row != rows.cend() && *row == (a & ~mask) && byteAt(firmware, a) >= 0)
      return index(int(row - rows.cbegin()), int(a & mask) + 1);
    return {};
  }
  quint32 firstAddress() const {
    return firmware.segments.isEmpty() ? 0 : firmware.segments.first().address;
  }
};
QString MainWindow::t(const char *en, const char *ru) const {
  return QString::fromUtf8(russian ? ru : en);
}
MainWindow::MainWindow()
    : russian(settings.value("language", "en").toString() == "ru") {
  setWindowTitle("Tailor · J-Link");
  resize(1200, 800);
  setMinimumSize(900, 650);
  setAcceptDrops(true);
  auto root = make<QWidget>();
  setCentralWidget(root);
  auto outer = make<QVBoxLayout>(root);
  outer->setContentsMargins(12, 12, 12, 8);
  outer->setSpacing(10);
  auto upper = make<QWidget>();
  upper->setObjectName("card");
  auto statusLayout = make<QHBoxLayout>(upper);
  statusLayout->setContentsMargins(12, 8, 12, 8);
  statusLayout->setSpacing(8);
  probeDot = make<QLabel>();
  probeDot->setObjectName("statusDot");
  probeStatus = make<QLabel>();
  probeStatus->setObjectName("probeStatus");
  targetDot = make<QLabel>();
  targetDot->setObjectName("statusDot");
  targetStatus = make<QLabel>();
  targetStatus->setObjectName("targetStatus");
  statusLayout->addWidget(probeDot);
  statusLayout->addWidget(probeStatus);
  auto separator = make<QFrame>();
  separator->setObjectName("statusSeparator");
  separator->setFrameShape(QFrame::VLine);
  statusLayout->addWidget(separator);
  statusLayout->addWidget(targetDot);
  statusLayout->addWidget(targetStatus);
  statusLayout->addStretch();
  auto check = make<QPushButton>(MdiIcons::icon(mdi::Refresh),
                                 t("Check connection", "Проверить соединение"));
  auto prefs = make<QPushButton>(MdiIcons::icon(mdi::Cog),
                                 t("Application settings", "Настройки приложения"));
  statusLayout->addWidget(check);
  statusLayout->addWidget(prefs);
  busyControls << check << prefs;
  connect(check, &QPushButton::clicked, this,
          [this] { perform(Operation::Connect); });
  connect(prefs, &QPushButton::clicked, this,
          &MainWindow::applicationSettingsDialog);
  setStatus(-1, -1);
  outer->addWidget(upper);
  tabs = make<QTabWidget>();
  outer->addWidget(tabs, 1);
  auto mainPage = make<QWidget>();
  auto mainLayout = make<QVBoxLayout>(mainPage);
  mainLayout->setContentsMargins(0, 4, 0, 0);
  mainLayout->setSpacing(12);

  // These fields are deliberately kept out of the main workflow. The preset dialog
  // owns their editing surface; the widgets retain the selected preset for operations.
  auto configStore = make<QWidget>(upper);
  configStore->hide();
  auto connection = make<QGridLayout>(configStore);
  transport = make<QComboBox>();
  transport->addItems({"USB", "Remote Server"});
  transport->setCurrentIndex(settings.value("transport", 0).toInt());
  serial = make<QComboBox>();
  serial->setEditable(true);
  serial->setPlaceholderText(
      t("Serial number / auto", "Серийный номер / авто"));
  serial->setCurrentText(settings.value("serial").toString());
  remote =
      make<QLineEdit>(settings.value("remote", "localhost:19020").toString());
  remote->setPlaceholderText("hostname:port / tunnel:...");
  device = make<QComboBox>();
  device->setObjectName("selectedMcu");
  device->setEditable(true);
  QStringList cachedDevices;
  for (const auto &entry : settings.value("devices").toStringList()) {
    const auto name = JLink::deviceName(entry);
    if (!name.isEmpty())
      cachedDevices.append(name);
  }
  cachedDevices.removeDuplicates();
  device->addItems(cachedDevices);
  device->setCurrentText(JLink::deviceName(settings.value("device", "STM32F407VG").toString()));
  flashLayout = settings.value("flashLayout").toString();
  flashLayoutDevice = settings.value("flashLayoutDevice").toString();
  if (flashLayoutDevice.compare(device->currentText(), Qt::CaseInsensitive) != 0) {
    flashLayout.clear();
    flashLayoutDevice.clear();
  }
  device->setMinimumWidth(230);
  auto completer = make<QCompleter>(device->model(), device);
  completer->setCaseSensitivity(Qt::CaseInsensitive);
  completer->setFilterMode(Qt::MatchContains);
  device->setCompleter(completer);
  interface = make<QComboBox>();
  interface->addItems({"SWD", "JTAG"});
  interface->setCurrentText(settings.value("interface", "SWD").toString());
  speed = make<QComboBox>();
  speed->setEditable(true);
  speed->addItems({"100", "1000", "4000", "8000", "auto", "adaptive"});
  speed->setCurrentText(settings.value("speed", "4000").toString());
  auto refresh = make<QPushButton>(MdiIcons::icon(mdi::Refresh),
                                   t("Devices", "Список МК"));
  connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
  auto probeRefresh =
      make<QPushButton>(MdiIcons::icon(mdi::Usb), t("Find USB", "Найти USB"));
  connect(probeRefresh, &QPushButton::clicked, this,
          [this] { link.probes(config().executable); });
  connection->addWidget(make<QLabel>(t("Connection", "Подключение")), 0, 0);
  connection->addWidget(transport, 1, 0);
  connection->addWidget(serial, 1, 1, 1, 2);
  connection->addWidget(remote, 1, 1, 1, 2);
  connection->addWidget(probeRefresh, 1, 3);
  connection->addWidget(make<QLabel>(t("Target device", "Микроконтроллер")), 2,
                        0);
  connection->addWidget(device, 3, 0, 1, 2);
  connection->addWidget(interface, 3, 2);
  connection->addWidget(speed, 3, 3);
  connection->addWidget(refresh, 3, 4);
  favorites = make<QComboBox>();
  favorites->setObjectName("favoritePresets");
  favorites->addItem(t("Favorite presets…", "Избранные пресеты…"));
  favorites->addItems(settings.value("favoriteNames").toStringList());
  favorites->setCurrentIndex(qMax(0, favorites->findText(settings.value("lastFavorite").toString())));
  connect(favorites, qOverload<int>(&QComboBox::activated), this,
          [this] { applyFavorite(); });
  auto transportState = [this] {
    serial->setVisible(transport->currentIndex() == 0);
    remote->setVisible(transport->currentIndex() == 1);
  };
  connect(transport, qOverload<int>(&QComboBox::currentIndexChanged), this,
          transportState);
  transportState();
  speed->setToolTip(
      t("Debug clock, kHz", "Частота отладочного интерфейса, кГц"));
  auto fwCard = make<QWidget>();
  fwCard->setObjectName("card");
  auto fwLayout = make<QVBoxLayout>(fwCard);
  fwLayout->setContentsMargins(16, 16, 16, 16);
  fwLayout->setSpacing(10);
  auto presetHeading = make<QLabel>(t("Target device", "Устройство"));
  presetHeading->setObjectName("panelTitle");
  fwLayout->addWidget(presetHeading);
  auto presetRow = make<QHBoxLayout>();
  presetRow->addWidget(make<QLabel>(t("Preset", "Пресет")));
  favorites->setMinimumWidth(180);
  presetRow->addWidget(favorites, 1);
  auto configurePreset = make<QPushButton>(
      MdiIcons::icon(mdi::Cog), t("Configure…", "Настроить…"));
  configurePreset->setObjectName("configurePreset");
  presetRow->addWidget(configurePreset);
  connect(configurePreset, &QPushButton::clicked, this,
          &MainWindow::presetDialog);
  fwLayout->addLayout(presetRow);
  auto targetSummary = make<QLabel>();
  targetSummary->setObjectName("hintLabel");
  targetSummary->setWordWrap(true);
  const auto updateTargetSummary = [this, targetSummary] {
    const auto connectionName = transport->currentIndex() == 0
        ? (serial->currentText().isEmpty() ? QStringLiteral("USB")
           : QStringLiteral("USB · ") + serial->currentText())
        : remote->text();
    targetSummary->setText(QString("%1  ·  %2  ·  %3 kHz  ·  %4")
        .arg(device->currentText().isEmpty() ? t("No MCU selected", "МК не выбран")
                                             : device->currentText(),
             interface->currentText(), speed->currentText(), connectionName));
  };
  for (auto field : {device, interface, speed, transport, serial})
    connect(field, &QComboBox::currentTextChanged, this, updateTargetSummary);
  connect(remote, &QLineEdit::textChanged, this, updateTargetSummary);
  updateTargetSummary();
  fwLayout->addWidget(targetSummary);
  mcuId = make<QLabel>(t("MCU ID: unavailable", "ID МК: недоступен"));
  mcuId->setObjectName("mcuId");
  mcuId->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mcuId->setToolTip(t("Device identifier read from the target; not its unique serial number.",
                       "Идентификатор модели, считанный из МК; не уникальный серийный номер."));
  fwLayout->addWidget(mcuId);
  auto bufferCard = make<QWidget>();
  bufferCard->setObjectName("card");
  auto bufferLayout = make<QVBoxLayout>(bufferCard);
  bufferLayout->setContentsMargins(16, 16, 16, 16);
  bufferLayout->setSpacing(10);
  auto bufferHeading = make<QLabel>(t("Firmware buffer", "Буфер прошивки"));
  bufferHeading->setObjectName("panelTitle");
  bufferLayout->addWidget(bufferHeading);
  auto fileRow = make<QHBoxLayout>();
  auto open = make<QPushButton>(MdiIcons::icon(mdi::FolderOpen),
                                t("Open firmware…", "Открыть прошивку…"));
  open->setObjectName("openFirmware");
  open->setShortcut(QKeySequence::Open);
  fileRow->addWidget(open);
  fileLabel = make<QLabel>(t("No file selected", "Файл не выбран"));
  fileLabel->setToolTip(t("Open or drop a .hex or .bin file", "Откройте или перетащите файл .hex или .bin"));
  fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  fileRow->addWidget(fileLabel, 1);
  base = make<QLineEdit>(settings.value("base", "0x08000000").toString());
  base->setObjectName("baseAddress");
  base->setMinimumWidth(105);
  base->setMaximumWidth(140);
  auto baseLabel = make<QLabel>(t("BIN address", "Адрес BIN"));
  baseLabel->setBuddy(base);
  fileRow->addWidget(baseLabel);
  fileRow->addWidget(base);
  bufferLayout->addLayout(fileRow);
  bufferLayout->addStretch();
  summary = make<QLabel>(t("No contents loaded", "Буфер не загружен"));
  summary->setObjectName("hintLabel");
  summary->setProperty("bufferSummary", true);
  summary->setWordWrap(true);
  summary->setMinimumHeight(summary->fontMetrics().lineSpacing() * 3);
  summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  auto bufferRow = make<QHBoxLayout>();
  auto bufferIcon = make<QLabel>();
  bufferIcon->setObjectName("bufferIcon");
  bufferIcon->setPixmap(MdiIcons::mutedIcon(MdiLayersOutline, 20).pixmap(20, 20));
  bufferRow->addWidget(bufferIcon, 0, Qt::AlignTop);
  bufferRow->addWidget(summary, 1);
  bufferLayout->addLayout(bufferRow);
  auto operationsCard = make<QWidget>();
  operationsCard->setObjectName("card");
  auto operationsLayout = make<QVBoxLayout>(operationsCard);
  operationsLayout->setContentsMargins(16, 16, 16, 16);
  operationsLayout->setSpacing(8);
  auto operationsHeading = make<QLabel>(t("Operations", "Операции"));
  operationsHeading->setObjectName("panelTitle");
  operationsLayout->addWidget(operationsHeading);
  auto actions = make<QGridLayout>();
  actions->setSpacing(8);
  auto button = [&](const QString &text, char32_t icon, Operation op, int row, int column, int span = 1) {
    auto b = make<QPushButton>(MdiIcons::icon(icon), text);
    actions->addWidget(b, row, column, 1, span);
    connect(b, &QPushButton::clicked, this, [this, op] { perform(op); });
    return b;
  };
  auto prog = button(t("Program  ·  F5", "Прошить  ·  F5"), mdi::Flash, Operation::Program, 0, 0, 2);
  prog->setToolTip(t("Connect, program, verify and optionally run", "Подключиться, записать, проверить и при необходимости запустить"));
  prog->setDefault(true);
  prog->setShortcut(QKeySequence(Qt::Key_F5));
  button(t("Write", "Записать"), mdi::FileUpload, Operation::Write, 1, 0);
  button(t("Verify", "Проверить"), mdi::Check, Operation::Verify, 1, 1);
  auto run = button(t("Run", "Запустить"), mdi::Play, Operation::Run, 2, 0);
  run->setToolTip(t("Reset and start the CPU", "Сбросить и запустить ядро"));
  button(t("Reset", "Сброс"), mdi::Refresh, Operation::Reset, 2, 1);
  auto halt = button(t("Halt", "Остановить"), mdi::Pause, Operation::Halt, 3, 0);
  halt->setToolTip(t("Halt the CPU without resetting it", "Остановить ядро без сброса"));
  auto resume = button(t("Resume", "Продолжить"), mdi::Play, Operation::Resume, 3, 1);
  resume->setToolTip(t("Resume the CPU without resetting it", "Продолжить выполнение без сброса"));
  button(t("Erase all", "Стереть всё"), mdi::Broom, Operation::Erase, 4, 0, 2);
  operationsLayout->addLayout(actions);
  unlock = make<QCheckBox>(t("Unlock (may erase flash)",
                             "Unlock (возможно стирание флеш)"));
  unlock->setObjectName("unlock");
  unlock->setToolTip(
      t("Legacy SEGGER STM32 Utility; select family in Settings",
        "Утилита SEGGER STM32 Utility; выберите семейство в настройках"));
  operationsLayout->addWidget(unlock);
  operationsLayout->addStretch();
  auto workArea = make<QHBoxLayout>();
  workArea->setSpacing(12);
  auto firmwareArea = make<QVBoxLayout>();
  firmwareArea->setSpacing(12);
  firmwareArea->addWidget(fwCard);
  firmwareArea->addWidget(bufferCard, 1);
  workArea->addLayout(firmwareArea, 3);
  workArea->addWidget(operationsCard, 2);
  mainLayout->addLayout(workArea);
  busyControls << fwCard << bufferCard << operationsCard;
  connect(open, &QPushButton::clicked, this, [this] {
    auto p = QFileDialog::getOpenFileName(
        this, t("Open firmware", "Открыть прошивку"),
        settings.value("lastDir").toString(), "Firmware (*.hex *.bin)");
    if (!p.isEmpty())
      openFile(p);
  });
  connect(base, &QLineEdit::editingFinished, this, [this] {
    if (!filePath.isEmpty() &&
        QFileInfo(filePath).suffix().compare("bin", Qt::CaseInsensitive) == 0)
      openFile(filePath);
  });
  auto hexPage = make<QWidget>();
  auto hexLayout = make<QVBoxLayout>(hexPage);
  hexLayout->setContentsMargins(0, 4, 0, 0);
  auto hexControls = make<QWidget>();
  hexControls->setObjectName("card");
  auto hexControlsLayout = make<QVBoxLayout>(hexControls);
  hexControlsLayout->setContentsMargins(16, 12, 16, 12);
  auto hexHeading = make<QLabel>(t("Buffer and comparison", "Буфер и сравнение"));
  hexHeading->setObjectName("panelTitle");
  hexControlsLayout->addWidget(hexHeading);
  auto tools = make<QHBoxLayout>();
  auto compare = make<QPushButton>(MdiIcons::icon(mdi::ContentDuplicate),
                                   t("Open comparison…", "Открыть для сравнения…"));
  compare->setObjectName("openComparison");
  auto closeCompare = make<QPushButton>(MdiIcons::icon(mdi::Close),
                                        t("Close comparison", "Закрыть сравнение"));
  closeCompare->setObjectName("closeComparison");
  comparisonLabel = make<QLabel>(t("No comparison file", "Файл сравнения не открыт"));
  comparisonLabel->setObjectName("hintLabel");
  tools->addWidget(compare);
  tools->addWidget(closeCompare);
  auto hexSave = make<QPushButton>(MdiIcons::icon(mdi::ContentSave), t("Save buffer…", "Сохранить буфер…"));
  hexSave->setObjectName("hexSaveBuffer");
  connect(hexSave, &QPushButton::clicked, this, [this] { saveBuffer(); });
  tools->addWidget(hexSave);
  tools->addStretch();
  search = make<QLineEdit>();
  search->setObjectName("hexSearch");
  search->setPlaceholderText(t("Address, hex bytes or ASCII text",
                               "Адрес, HEX-байты или текст ASCII"));
  auto searchTools = make<QHBoxLayout>();
  searchTools->addWidget(search, 1);
  auto go = make<QPushButton>(MdiIcons::icon(mdi::ArrowCollapseRight),
                              t("Go to address", "К адресу"));
  auto find = make<QPushButton>(MdiIcons::icon(mdi::Magnify),
                                t("Find bytes", "Найти байты"));
  auto findAscii = make<QPushButton>(MdiIcons::icon(mdi::Magnify),
                                     t("Find ASCII", "Найти ASCII"));
  findAscii->setObjectName("findAscii");
  findAscii->setToolTip(t("Find ASCII text in the working buffer (case-sensitive)",
                          "Найти текст ASCII в рабочем буфере с учётом регистра"));
  searchTools->addWidget(go);
  searchTools->addWidget(find);
  searchTools->addWidget(findAscii);
  hexControlsLayout->addLayout(tools);
  hexControlsLayout->addLayout(searchTools);
  auto sectorFilter = make<QComboBox>();
  sectorFilter->setObjectName("hexSectorFilter");
  sectorFilter->setAccessibleName(t("Memory sector", "Сектор памяти"));
  sectorFilter->setMinimumWidth(150);
  tools->addWidget(make<QLabel>(t("View", "Показать")));
  tools->addWidget(sectorFilter);
  auto pageTools = make<QHBoxLayout>();
  pageTools->addWidget(comparisonLabel);
  auto differenceHint = make<QLabel>(t("Highlighted bytes differ or are missing", "Выделены отличающиеся и отсутствующие байты"));
  differenceHint->setObjectName("comparisonHint");
  differenceHint->setVisible(false);
  pageTools->addWidget(differenceHint);
  pageTools->addStretch();
  auto readPage = make<QPushButton>(MdiIcons::icon(mdi::FileImport),
      t("Read selected page", "Прочитать страницу"));
  readPage->setObjectName("hexReadSelected");
  readPage->setEnabled(false);
  readPage->setToolTip(t("Read the selected flash sector or page from the target into the working buffer.",
                         "Прочитать выбранный сектор или страницу флеш из МК в рабочий буфер."));
  pageTools->addWidget(readPage);
  hexControlsLayout->addLayout(pageTools);
  hexLayout->addWidget(hexControls);
  closeCompare->setEnabled(false);
  memoryView = make<HexTable>();
  memoryView->setObjectName("memoryView");
  comparisonView = make<HexTable>();
  comparisonView->setObjectName("comparisonView");
  memoryModel = make<HexModel>(memoryView);
  comparisonModel = make<HexModel>(comparisonView);
  memoryView->setModel(memoryModel);
  comparisonView->setModel(comparisonModel);
  for (auto view : {memoryView, comparisonView}) {
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setProperty("hexTable", true);
    view->setItemDelegate(make<HexDelegate>(view));
    view->setEditTriggers(view == memoryView ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed : QAbstractItemView::NoEditTriggers);
    view->setShowGrid(false);
    view->verticalHeader()->hide();
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setMinimumSectionSize(qMax(24, view->fontMetrics().horizontalAdvance("FF") + 8));
    view->setColumnWidth(0, 100);
    for (int i = 1; i <= 16; ++i)
      view->setColumnWidth(i, qMax(24, view->fontMetrics().horizontalAdvance("FF") + 8));
    view->setColumnWidth(17, qMax(120, view->fontMetrics().horizontalAdvance("FFFFFFFFFFFFFFFF") + 16));
    view->setAlternatingRowColors(true);
  }
  auto memoryPane = make<QWidget>();
  auto memoryPaneLayout = make<QVBoxLayout>(memoryPane);
  memoryPaneLayout->setContentsMargins(0, 0, 0, 0);
  memoryTitle = make<QLabel>(t("Working buffer", "Рабочий буфер"));
  memoryTitle->setObjectName("hintLabel");
  memoryTitle->setToolTip(t("Double-click a byte or press F2 to edit it", "Дважды нажмите на байт или нажмите F2 для изменения"));
  memoryPaneLayout->addWidget(memoryTitle);
  memoryPaneLayout->addWidget(memoryView, 1);
  auto comparisonPane = make<QWidget>();
  comparisonPane->setObjectName("comparisonPane");
  auto comparisonPaneLayout = make<QVBoxLayout>(comparisonPane);
  comparisonPaneLayout->setContentsMargins(0, 0, 0, 0);
  auto comparisonTitle = make<QLabel>(t("Comparison file", "Файл сравнения"));
  comparisonTitle->setObjectName("hintLabel");
  comparisonPaneLayout->addWidget(comparisonTitle);
  comparisonPaneLayout->addWidget(comparisonView, 1);
  comparisonPane->setVisible(false);
  auto memorySplit = make<QSplitter>(Qt::Horizontal);
  memorySplit->setObjectName("memorySplit");
  memorySplit->addWidget(memoryPane);
  memorySplit->addWidget(comparisonPane);
  memorySplit->setStretchFactor(0, 1);
  memorySplit->setStretchFactor(1, 1);
  auto memoryContainer = make<QWidget>();
  memoryContainer->setObjectName("memoryContainer");
  auto memoryContainerLayout = make<QVBoxLayout>(memoryContainer);
  memoryContainerLayout->setContentsMargins(0, 0, 0, 0);
  memoryContainerLayout->addWidget(memorySplit);
  auto centeredMemory = make<QHBoxLayout>();
  centeredMemory->addStretch(0);
  centeredMemory->addWidget(memoryContainer, 1);
  centeredMemory->addStretch(0);
  hexLayout->addLayout(centeredMemory, 1);
  memoryModel->editable = true;
  connect(memoryModel, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &first, const QModelIndex &, const QList<int> &) {
    if (first.column() < 1 || first.column() > memoryModel->rowBytes) return;
    firmware = memoryModel->firmware;
    comparisonModel->other = firmware;
    if (comparisonModel->rowCount() > first.row())
      emit comparisonModel->dataChanged(comparisonModel->index(first.row(), first.column()), comparisonModel->index(first.row(), first.column()), {Qt::BackgroundRole});
    setBufferModified(true);
    updateSummary();
  });
  busyControls << hexPage;
  connect(memoryView->verticalScrollBar(), &QScrollBar::valueChanged, comparisonView->verticalScrollBar(), &QScrollBar::setValue);
  connect(comparisonView->verticalScrollBar(), &QScrollBar::valueChanged, memoryView->verticalScrollBar(), &QScrollBar::setValue);
  connect(memoryView->horizontalScrollBar(), &QScrollBar::valueChanged, comparisonView->horizontalScrollBar(), &QScrollBar::setValue);
  connect(comparisonView->horizontalScrollBar(), &QScrollBar::valueChanged, memoryView->horizontalScrollBar(), &QScrollBar::setValue);
  connect(compare, &QPushButton::clicked, this, [this, closeCompare, differenceHint] {
    openComparisonFile();
    closeCompare->setEnabled(!comparisonPath.isEmpty());
    differenceHint->setVisible(!comparisonPath.isEmpty());
  });
  connect(closeCompare, &QPushButton::clicked, this, [this, closeCompare, differenceHint] {
    comparison = {};
    comparisonPath.clear();
    findChild<QWidget *>("comparisonPane")->setVisible(false);
    comparisonLabel->setText(t("No comparison file", "Файл сравнения не открыт"));
    closeCompare->setEnabled(false);
    differenceHint->setVisible(false);
    updateHexPage();
  });
  connect(sectorFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateHexPage(); });
  connect(sectorFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, [sectorFilter, readPage] {
    readPage->setEnabled(sectorFilter->currentData().toMap().value("length").toUInt() != 0);
  });
  connect(readPage, &QPushButton::clicked, this, [this, sectorFilter] {
    const auto range = sectorFilter->currentData().toMap();
    const auto length = range.value("length").toUInt();
    if (!length || !confirmDiscardChanges()) return;
    try {
      saveConfig();
      link.execute(config(), Operation::Read, firmware, range.value("address").toUInt(), length);
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
  });
  connect(go, &QPushButton::clicked, this, [this] { findHex(HexSearch::Address); });
  connect(find, &QPushButton::clicked, this, [this] { findHex(HexSearch::Bytes); });
  connect(findAscii, &QPushButton::clicked, this, [this] { findHex(HexSearch::Ascii); });
  tabs->addTab(hexPage, t("HEX viewer", "Просмотр HEX"));
  auto readPanel = make<QWidget>();
  auto readPanelLayout = make<QVBoxLayout>(readPanel);
  readPanelLayout->setContentsMargins(16, 12, 16, 12);
  auto readLayout = make<QHBoxLayout>();
  auto readHeading = make<QLabel>(t("Read memory", "Чтение памяти"));
  readHeading->setObjectName("panelTitle");
  if (russian) {
    readPanelLayout->addWidget(readHeading);
  } else {
    readLayout->addWidget(readHeading);
    readLayout->addSpacing(12);
  }
  readPanelLayout->addLayout(readLayout);
  readAddress = make<QLineEdit>();
  readSize = make<QLineEdit>();
  readAddress->setMinimumWidth(120);
  readSize->setMinimumWidth(90);
  readAddress->setObjectName("readAddress");
  readSize->setObjectName("readSize");
  readAddress->setPlaceholderText(t("Address", "Адрес"));
  readSize->setPlaceholderText(t("Enter length", "Введите длину"));
  readAddress->setToolTip(t("Defaults to a contiguous flash range from the J-Link catalog; enter manually if unavailable.",
                             "По умолчанию — непрерывный диапазон флеш из каталога J-Link; при отсутствии задайте вручную."));
  connect(readAddress, &QLineEdit::textEdited, this, [this] { readRangeEdited = true; });
  connect(readSize, &QLineEdit::textEdited, this, [this] { readRangeEdited = true; });
  readSize->setToolTip(t("Decimal or 0x hexadecimal; K = 1024, M = 1048576 bytes (e.g. 64K)",
                         "Десятичное или 0x HEX; K = 1024, M = 1048576 байт (например, 64K)"));
  auto readAddressLabel = make<QLabel>(t("Start address", "Начальный адрес"));
  readAddressLabel->setBuddy(readAddress);
  auto readSizeLabel = make<QLabel>(t("Length, bytes", "Длина, байт"));
  readSizeLabel->setBuddy(readSize);
  readLayout->addWidget(readAddressLabel);
  readLayout->addWidget(readAddress, 1);
  readLayout->addWidget(readSizeLabel);
  readLayout->addWidget(readSize, 1);
  auto read = make<QPushButton>(MdiIcons::icon(mdi::FileImport),
                                t("Read memory", "Прочитать память"));
  auto readSelected = make<QPushButton>(t("Select sectors…", "Выбрать секторы…"));
  readSelected->setObjectName("readSectors");
  auto save = make<QPushButton>(MdiIcons::icon(mdi::ContentSave),
                                t("Save contents…", "Сохранить содержимое…"));
  auto readActions = make<QHBoxLayout>();
  readActions->addStretch();
  readActions->addWidget(read);
  readActions->addWidget(readSelected);
  readActions->addWidget(save);
  readPanelLayout->addLayout(readActions);
  connect(read, &QPushButton::clicked, this,
          [this] { perform(Operation::Read); });
  connect(readSelected, &QPushButton::clicked, this, &MainWindow::readSectors);
  connect(save, &QPushButton::clicked, this, &MainWindow::saveBuffer);
  readPanel->setObjectName("card");
  mainLayout->addWidget(readPanel);
  busyControls << readPanel;
  auto logs = make<QWidget>();
  logs->setObjectName("card");
  auto logsLayout = make<QVBoxLayout>(logs);
  logsLayout->setContentsMargins(12, 8, 12, 12);
  auto logTools = make<QHBoxLayout>();
  auto logHeading = make<QLabel>(t("Operation log", "Журнал операций"));
  logHeading->setObjectName("panelTitle");
  logTools->addWidget(logHeading);
  logTools->addStretch();
  auto exportLog = make<QPushButton>(MdiIcons::icon(mdi::FileExport),
                                     t("Export", "Экспорт"));
  auto clear =
      make<QPushButton>(MdiIcons::icon(mdi::Broom), t("Clear", "Очистить"));
  logTools->addWidget(exportLog);
  logTools->addWidget(clear);
  logsLayout->addLayout(logTools);
  logView = make<QPlainTextEdit>();
  logView->setObjectName("operationLog");
  logView->setReadOnly(true);
  logView->setMaximumBlockCount(10000);
  logView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  logsLayout->addWidget(logView);
  mainLayout->addWidget(logs, 1);
  connect(clear, &QPushButton::clicked, logView, &QPlainTextEdit::clear);
  connect(exportLog, &QPushButton::clicked, this, [this] {
    auto p = QFileDialog::getSaveFileName(
        this, t("Export log", "Экспорт журнала"), "tailor.log");
    if (!p.isEmpty())
      try {
        writeFile(p, logView->toPlainText().toUtf8());
      } catch (const std::exception &e) {
        fail(e.what());
      }
  });
  auto statusSeparator = make<QFrame>();
  statusSeparator->setObjectName("footerDivider");
  statusSeparator->setFixedHeight(1);
  mainLayout->addWidget(statusSeparator);
  auto bottom = make<QHBoxLayout>();
  auto stageIcon = make<QLabel>();
  stageIcon->setPixmap(MdiIcons::mutedIcon(mdi::Check, 18).pixmap(18, 18));
  bottom->addWidget(stageIcon);
  stageLabel = make<QLabel>(t("Ready", "Готово"));
  stageLabel->setObjectName("operationStage");
  bottom->addWidget(stageLabel);
  bottom->addStretch();
  progress = make<QProgressBar>();
  progress->setObjectName("operationProgress");
  progress->setFixedWidth(160);
  progress->setRange(0, 1);
  progress->setValue(0);
  bottom->addWidget(progress);
  cancelButton =
      make<QPushButton>(MdiIcons::icon(mdi::Stop), t("Cancel", "Отмена"));
  cancelButton->setEnabled(false);
  bottom->addWidget(cancelButton);
  mainLayout->addLayout(bottom);
  tabs->insertTab(0, mainPage, t("Program", "Прошивка"));
  tabs->setCurrentIndex(0);
  connect(cancelButton, &QPushButton::clicked, &link, &JLink::cancel);
  connect(&link, &JLink::log, this, [this](const QString &s) {
    appendLog(s, JLink::outputError(s) || s.contains("Timeout", Qt::CaseInsensitive));
  });
  connect(&link, &JLink::importantLog, this, [this](const QString &s) { appendLog(s, true); });
  connect(&link, &JLink::stage, this, [this, stageIcon](const QString &s) {
    auto label = s;
    if (s.compare("Comparing flash", Qt::CaseInsensitive) == 0)
      label = t("Comparing flash", "Сравнение флеш");
    else if (s.compare("Erasing flash", Qt::CaseInsensitive) == 0)
      label = t("Erasing flash", "Стирание флеш");
    else if (s.compare("Programming flash", Qt::CaseInsensitive) == 0)
      label = t("Programming flash", "Запись флеш");
    else if (s.compare("Verifying flash", Qt::CaseInsensitive) == 0)
      label = t("Verifying flash", "Проверка флеш");
    else if (s.compare("Reading memory", Qt::CaseInsensitive) == 0)
      label = t("Reading memory", "Чтение памяти");
    stageLabel->setText(label);
    stageIcon->setPixmap(MdiIcons::mutedIcon(mdi::ClockOutline, 18).pixmap(18, 18));
    appendLog(s);
    progress->setRange(0, 0);
    cancelButton->setEnabled(true);
    for (auto c : busyControls)
      c->setEnabled(false);
  });
  connect(&link, &JLink::progressChanged, this, [this](int percent) {
    if (percent < 0) {
      progress->setRange(0, 0);
    } else {
      progress->setRange(0, 100);
      progress->setValue(percent);
    }
  });
  connect(&link, &JLink::finished, this, [this, stageIcon](bool ok, const QString &s) {
    progress->setRange(0, 1);
    progress->setValue(ok ? 1 : 0);
    cancelButton->setEnabled(false);
    for (auto c : busyControls)
      c->setEnabled(true);
    updateUnlockAvailability();
    const bool powerCycle = s.startsWith("GD32 unlock completed.");
    stageLabel->setText(powerCycle ? t("Power cycle required", "Требуется перезапуск питания")
                        : ok ? t("Completed", "Завершено")
                             : t("Failed / canceled", "Ошибка / отмена"));
    stageIcon->setPixmap(MdiIcons::mutedIcon(ok ? mdi::Check : mdi::AlertCircleOutline, 18).pixmap(18, 18));
    appendLog(s, !ok);
    if (powerCycle)
      QMessageBox::information(this, t("Power cycle required", "Требуется перезапуск питания"),
          t("GD32 protection was removed. Power-cycle the target, then click Program again. Programming has not started.",
            "Защита GD32 снята. Выключите и включите питание МК, затем снова нажмите «Прошить». Запись ещё не начиналась."));
  });
  connect(&link, &JLink::flashRanges, this, [this](const QVariantMap &ranges) {
    flashRanges = ranges;
    settings.setValue("flashRanges", ranges);
    settings.setValue("flashCatalogCommander", config().executable);
    if (!readRangeEdited)
      updateReadRange();
  });
  connect(&link, &JLink::devices, this, [this](const QStringList &d) {
    auto old = device->currentText();
    const QSignalBlocker blocker(device);
    device->clear();
    device->addItems(d);
    device->setCurrentText(old);
    settings.setValue("devices", d);
    appendLog(t("Devices loaded: ", "Загружено устройств: ") +
              QString::number(d.size()));
  });
  connect(&link, &JLink::probeList, this, [this](const QStringList &s) {
    auto old = serial->currentText();
    serial->clear();
    serial->addItems(s);
    serial->setCurrentText(s.contains(old) ? old : s.value(0));
  });
  connect(&link, &JLink::observed, this, [this](int p, int target) {
    setStatus(p, target);
    if (target == 0)
      mcuId->setText(t("MCU ID: unavailable", "ID МК: недоступен"));
  });
  connect(&link, &JLink::deviceId, this, [this](const QString &id) {
    mcuId->setText(id.isEmpty() ? t("MCU ID: unavailable", "ID МК: недоступен")
                               : t("MCU ID: ", "ID МК: ") + id);
  });
  connect(&link, &JLink::readDone, this, [this](const Firmware &f) {
    firmware = f;
    setBufferModified(false);
    filePath.clear();
    fileLabel->setText(t("Target memory buffer", "Буфер памяти МК"));
    fileLabel->setToolTip(t("Contents read from target memory", "Содержимое считано из памяти МК"));
    findChild<QComboBox *>("hexSectorFilter")->setCurrentIndex(0);
    updateSummary();
    updateHexPage();
  });
  for (auto field : {device, interface, speed, transport, serial})
    connect(field, &QComboBox::currentTextChanged, this, [this] {
      setStatus(-1, -1);
      mcuId->setText(t("MCU ID: unavailable", "ID МК: недоступен"));
    });
  connect(remote, &QLineEdit::textChanged, this, [this] {
    setStatus(-1, -1);
    mcuId->setText(t("MCU ID: unavailable", "ID МК: недоступен"));
  });
  connect(device, &QComboBox::currentTextChanged, this,
          [this] {
            if (flashLayoutDevice.compare(device->currentText(), Qt::CaseInsensitive) != 0) {
              flashLayout.clear();
              flashLayoutDevice.clear();
            }
            readRangeEdited = false;
            updateReadRange();
            updateUnlockAvailability();
            updateSectorFilter();
          });
  connect(interface, &QComboBox::currentTextChanged, this,
          [this] { updateUnlockAvailability(); });
  updateUnlockAvailability();
  restoreGeometry(settings.value("geometry").toByteArray());
  if (settings.value("commander").toString().isEmpty())
    settings.setValue("commander", JLink::discover());
  updateUnlockAvailability();
  unlock->setChecked(unlock->isEnabled() && settings.value("unlock", false).toBool());
  connect(unlock, &QCheckBox::toggled, this, [this](bool checked) {
    settings.setValue("unlock", checked);
  });
  const auto commander = settings.value("commander").toString();
  if (settings.value("flashCatalogCommander").toString() == commander)
    flashRanges = settings.value("flashRanges").toMap();
  updateReadRange();
  updateSectorFilter();
  appendLog("J-Link Commander: " + settings.value("commander").toString());
  if (QFileInfo(commander).isExecutable() &&
      (device->count() == 0 || !settings.contains("flashCatalogCommander") ||
       settings.value("flashCatalogCommander").toString() != commander))
    QTimer::singleShot(0, this, &MainWindow::refreshDevices);
}
MainWindow::~MainWindow() { saveConfig(); }
void MainWindow::appendLog(const QString &s, bool important) {
  const auto line = QTime::currentTime().toString("HH:mm:ss.zzz") + "  " + s;
  logView->appendPlainText(line);
  auto cursor = logView->textCursor();
  cursor.movePosition(QTextCursor::End);
  cursor.setPosition(static_cast<int>(qMax(qsizetype(0), cursor.position() - line.size())), QTextCursor::KeepAnchor);
  QTextCharFormat format;
  format.setFontWeight(important ? QFont::Bold : QFont::Normal);
  cursor.mergeCharFormat(format);
}
void MainWindow::fail(const QString &s) {
  appendLog(s, true);
  QMessageBox::warning(this, "Tailor", s);
}
LinkConfig MainWindow::config() const {
  LinkConfig c;
  c.executable = settings.value("commander").toString();
  c.device = device->currentText();
  c.interface = interface->currentText();
  c.speed = speed->currentText();
  if (transport->currentIndex() == 0)
    c.serial = serial->currentText();
  else
    c.remote = remote->text();
  c.fullErase = settings.value("fullErase", false).toBool();
  c.runAfter = settings.value("runAfter", true).toBool();
  c.timeoutMs = settings.value("timeout", 120).toInt() * 1000;
  c.unlock = unlock->isChecked();
  return c;
}
void MainWindow::saveConfig() {
  settings.setValue("geometry", saveGeometry());
  settings.setValue("transport", transport->currentIndex());
  settings.setValue("serial", serial->currentText());
  settings.setValue("remote", remote->text());
  settings.setValue("device", device->currentText());
  settings.setValue("interface", interface->currentText());
  settings.setValue("speed", speed->currentText());
  settings.setValue("base", base->text());
  settings.setValue("lastFavorite", favorites->currentIndex() > 0 ? favorites->currentText() : QString());
  settings.setValue("unlock", unlock->isChecked());
  settings.setValue("flashLayout", flashLayout);
  settings.setValue("flashLayoutDevice", flashLayoutDevice);
}
void MainWindow::openFile(const QString &p) {
  if (link.busy()) {
    fail(t("Wait for the current operation", "Дождитесь завершения операции"));
    return;
  }
  try {
    if (!confirmDiscardChanges()) {
      if (p == filePath && QFileInfo(p).suffix().compare("bin", Qt::CaseInsensitive) == 0 && !firmware.segments.isEmpty())
        base->setText(QString("0x%1").arg(firmware.segments.first().address, 8, 16, QChar('0')));
      return;
    }
    auto f = Firmware::load(p, addressValue(base->text()));
    firmware = std::move(f);
    setBufferModified(false);
    filePath = p;
    fileLabel->setText(QFileInfo(p).fileName());
    fileLabel->setToolTip(p);
    findChild<QComboBox *>("hexSectorFilter")->setCurrentIndex(0);
    updateHexPage();
    settings.setValue("lastDir", QFileInfo(p).absolutePath());
    updateSummary();
  } catch (const std::exception &e) {
    fail(QString::fromUtf8(e.what()));
  }
}
void MainWindow::updateSummary() {
  if (firmware.segments.isEmpty()) {
    summary->setText(t("No contents loaded", "Буфер не загружен"));
    summary->setToolTip({});
    summary->setAccessibleDescription({});
    return;
  }
  const QLocale locale(russian ? QLocale::Russian : QLocale::English);
  const auto count = firmware.size();
  auto text = t("Buffer: ", "Буфер: ") + locale.toString(count) + t(" bytes", " байт");
  if (count >= 1024)
    text += " (" + locale.formattedDataSize(count, 2, QLocale::DataSizeIecFormat) + ")";
  if (!filePath.isEmpty())
    text += t("  ·  File: ", "  ·  Файл: ") + locale.toString(QFileInfo(filePath).size()) + t(" bytes", " байт");
  const auto selected = settings.value("checksumAlgorithm", "md5").toString();
  const auto algorithm = selected == "crc32" ? ChecksumAlgorithm::Crc32Ethernet
                       : selected == "sha256" ? ChecksumAlgorithm::Sha256
                                               : ChecksumAlgorithm::Md5;
  const auto label = algorithm == ChecksumAlgorithm::Crc32Ethernet ? "CRC-32/Ethernet"
                   : algorithm == ChecksumAlgorithm::Sha256 ? "SHA-256" : "MD5";
  text += QString("\n%1: %2").arg(label, firmware.checksum(algorithm));
  summary->setText(text);
  const auto details = QString("%1: %2\n0x%3 — 0x%4")
      .arg(t("Segments", "Сегментов"))
      .arg(firmware.segments.size())
      .arg(firmware.segments.first().address, 8, 16, QChar('0'))
      .arg(quint64(firmware.segments.last().address) +
               quint64(firmware.segments.last().data.size()) - 1, 8, 16, QChar('0'));
  const auto description = details + "\n" + t("Checksum covers data bytes in address order; address gaps are omitted.",
      "Контрольная сумма вычисляется по байтам данных в порядке адресов без заполнения промежутков.");
  summary->setToolTip(description);
  summary->setAccessibleDescription(description);
}
void MainWindow::perform(Operation op) {
  try {
    if (!commitBufferEdit()) return;
    if (op == Operation::Read && !confirmDiscardChanges()) return;
    saveConfig();
    auto c = config();
    if (op == Operation::Erase ||
        (op == Operation::Program && (c.fullErase || c.unlock))) {
      auto warning = t("This operation may erase all flash and reset option bytes. Continue?",
                       "Операция может стереть всю флеш и сбросить option bytes. Продолжить?");
      if (c.unlock && JLink::unlockMethod(c.executable, c.device).startsWith("devpro-gd:"))
        warning += t("\n\nGD32 requires nRESET connected and a target power cycle after unlock; programming will stop until then.",
                     "\n\nДля GD32 необходимо подключить nRESET; после снятия защиты нужно перезапустить питание МК. До этого запись будет остановлена.");
      if (QMessageBox::warning(
              this, t("Erase confirmation", "Подтверждение стирания"),
              warning,
              QMessageBox::Yes | QMessageBox::No,
              QMessageBox::No) != QMessageBox::Yes)
        return;
    }
    const auto readStart = op == Operation::Read ? addressValue(readAddress->text()) : 0;
    const auto readLength = op == Operation::Read ? lengthValue(readSize->text()) : 0;
    link.execute(c, op, firmware, readStart, readLength);
  } catch (const std::exception &e) {
    if (!QFileInfo(settings.value("commander").toString()).isExecutable())
      setStatus(0, -1);
    fail(e.what());
  }
}
void MainWindow::updateUnlockAvailability() {
  const auto method = JLink::unlockMethod(settings.value("commander").toString(), device->currentText());
  const bool available = !method.isEmpty() && interface->currentText() == "SWD";
  unlock->setEnabled(available);
  if (!available)
    unlock->setChecked(false);
  unlock->setToolTip(
      available ? (method.startsWith("devpro-gd:")
          ? t("GD32: requires nRESET. Unlock erases flash and requires a target power cycle.", "GD32: необходим nRESET. Unlock стирает флеш; затем выключите и включите питание МК.")
          : t("STM32: resets option bytes; removing read protection erases flash.", "STM32: сбрасывает option bytes; снятие защиты чтения стирает флеш."))
                : t("No supported unlock method for this preset and installed J-Link; SWD is required. GD32F4/A4 requires Device Provisioner and its GD32 script.",
                    "Для пресета и установленного J-Link нет поддерживаемого метода unlock; требуется SWD. Для GD32F4/A4 нужны Device Provisioner и скрипт GD32."));
}
void MainWindow::updateReadRange() {
  const auto range = flashRanges.value(device->currentText().trimmed().toUpper()).toMap();
  if (range.isEmpty()) {
    readAddress->clear();
    readSize->clear();
    readSize->setPlaceholderText(t("Enter length", "Введите длину"));
    return;
  }
  const auto start = range.value("address").toUInt();
  const auto length = range.value("length").toUInt();
  readAddress->setText(QString("0x%1").arg(start, 8, 16, QChar('0')));
  readSize->setText(length % (1024 * 1024) == 0 ? QString::number(length / (1024 * 1024)) + "M"
                    : length % 1024 == 0 ? QString::number(length / 1024) + "K"
                                         : QString::number(length));
}
void MainWindow::refreshDevices() { link.catalog(config().executable); }
void MainWindow::setBufferModified(bool modified) {
  bufferModified = modified;
  memoryTitle->setText(modified ? t("Working buffer · modified", "Рабочий буфер · изменён")
                                : t("Working buffer", "Рабочий буфер"));
  auto text = fileLabel->text();
  if (text.endsWith(" *")) text.chop(2);
  fileLabel->setText(modified ? text + " *" : text);
}
bool MainWindow::confirmDiscardChanges() {
  if (!commitBufferEdit()) return false;
  if (!bufferModified) return true;
  const auto answer = QMessageBox::warning(this, t("Unsaved buffer", "Несохранённый буфер"),
      t("Save changes to the working buffer?", "Сохранить изменения рабочего буфера?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  return answer == QMessageBox::Discard || (answer == QMessageBox::Save && saveBuffer());
}
bool MainWindow::commitBufferEdit() {
  auto editor = memoryView->findChild<QLineEdit *>();
  if (!editor || !editor->isVisible()) return true;
  if (!editor->hasAcceptableInput() || !memoryModel->setData(memoryView->currentIndex(), editor->text(), Qt::EditRole)) {
    fail(t("Enter exactly two hexadecimal digits", "Введите ровно две шестнадцатеричные цифры"));
    return false;
  }
  editor->clearFocus();
  return true;
}
bool MainWindow::saveBuffer() {
  if (!commitBufferEdit()) return false;
  if (firmware.segments.isEmpty()) {
    fail(t("No memory contents to save", "Нет содержимого для сохранения"));
    return false;
  }
  auto p = QFileDialog::getSaveFileName(
      this, t("Save contents", "Сохранить содержимое"), filePath.isEmpty() ? "contents.hex" : filePath,
      "Intel HEX (*.hex);;Binary (*.bin)");
  if (p.isEmpty())
    return false;
  return saveBufferTo(p);
}
bool MainWindow::saveBufferTo(const QString &p) {
  if (firmware.segments.isEmpty()) return false;
  try {
    if (!p.endsWith(".hex", Qt::CaseInsensitive) && !p.endsWith(".bin", Qt::CaseInsensitive))
      throw std::runtime_error("Choose a .hex or .bin filename");
    if (!p.endsWith(".hex", Qt::CaseInsensitive) && firmware.segments.size() != 1)
      throw std::runtime_error("Non-contiguous buffer: save as Intel HEX to preserve addresses");
    writeFile(p, p.endsWith(".hex", Qt::CaseInsensitive)
                     ? firmware.toHex()
                     : firmware.segments.first().data);
    filePath = p;
    if (p.endsWith(".bin", Qt::CaseInsensitive))
      base->setText(QString("0x%1").arg(firmware.segments.first().address, 8, 16, QChar('0')));
    fileLabel->setText(QFileInfo(p).fileName());
    fileLabel->setToolTip(p);
    setBufferModified(false);
    settings.setValue("lastDir", QFileInfo(p).absolutePath());
    updateSummary();
    return true;
  } catch (const std::exception &e) {
    fail(QString::fromUtf8(e.what()));
    return false;
  }
}
void MainWindow::findHex(HexSearch mode) {
  try {
    const auto *model = memoryModel;
    auto view = memoryView;
    QModelIndex i;
    if (mode == HexSearch::Address) {
      const auto value = addressValue(search->text());
      findChild<QComboBox *>("hexSectorFilter")->setCurrentIndex(0);
      i = model->atAddress(value);
    }
    else {
      findChild<QComboBox *>("hexSectorFilter")->setCurrentIndex(0);
      auto text = search->text();
      QByteArray bytes;
      if (mode == HexSearch::Ascii) {
        if (text.isEmpty() || std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() > 0x7e; }))
          throw std::runtime_error(t("Enter printable ASCII text", "Введите печатный текст ASCII").toStdString());
        bytes = text.toLatin1();
      } else {
        text.remove(' ');
        static const QRegularExpression rx("^(?:[0-9a-fA-F]{2})+$");
        if (!rx.match(text).hasMatch())
          throw std::runtime_error("Enter complete hexadecimal bytes");
        bytes = QByteArray::fromHex(text.toLatin1());
      }
      for (const auto &s : model->firmware.segments) {
        auto n = s.data.indexOf(bytes);
        if (n >= 0) {
          i = model->atAddress(
              static_cast<quint32>(quint64(s.address) + quint64(n)));
          break;
        }
      }
    }
    if (i.isValid()) {
      view->setCurrentIndex(i);
      view->scrollTo(i, QAbstractItemView::PositionAtCenter);
    } else
      fail(t("Not found", "Не найдено"));
  } catch (const std::exception &e) {
    fail(e.what());
  }
}
void MainWindow::updateHexPage() {
  const auto filter = findChild<QComboBox *>("hexSectorFilter");
  if (!filter || !memoryModel || !comparisonModel)
    return;
  const auto range = filter->currentData().toMap();
  const auto start = range.value("address").toUInt();
  const auto length = range.value("length").toUInt();
  const bool active = !comparisonPath.isEmpty();
  memoryModel->setFirmware(firmware, comparison, active, start, length);
  comparisonModel->setFirmware(comparison, firmware, active, start, length);
  for (auto view : {memoryView, comparisonView}) {
    const int bytes = 16;
    view->setColumnWidth(0, active ? 90 : 100);
    for (int i = 1; i <= bytes; ++i) {
      view->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Fixed);
      view->setColumnWidth(i, qMax(24, view->fontMetrics().horizontalAdvance("FF") + 8));
    }
    view->horizontalHeader()->setSectionResizeMode(bytes + 1, QHeaderView::Fixed);
    view->setColumnWidth(bytes + 1, qMax(145, view->fontMetrics().horizontalAdvance(QString(bytes, 'F')) + 32));
  }
  const auto singleWidth = memoryView->columnWidth(0) + 16 * memoryView->columnWidth(1) +
      memoryView->columnWidth(17) + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 16;
  auto split = findChild<QWidget *>("memoryContainer");
  if (active) {
    const auto paneWidth = memoryView->columnWidth(0) + 16 * memoryView->columnWidth(1) +
        memoryView->columnWidth(17) + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 32;
    split->setMinimumWidth(0);
    split->setMaximumWidth(2 * paneWidth + findChild<QSplitter *>("memorySplit")->handleWidth());
  } else {
    split->setFixedWidth(singleWidth);
  }
}
QString MainWindow::sectorLabel(const MemoryRange &range, int index) const {
  const auto sector = range.sector >= 0 ? range.sector : index;
  const auto bank = range.bank >= 0 ? t("Bank ", "Банк ") + QString::number(range.bank) + " · " : QString();
  return bank + t("Sector ", "Сектор ") + QString::number(sector);
}
void MainWindow::updateSectorFilter() {
  auto filter = findChild<QComboBox *>("hexSectorFilter");
  if (!filter) return;
  const QSignalBlocker blocker(filter);
  filter->clear();
  filter->addItem(t("Entire buffer", "Весь буфер"));
  if (auto readPage = findChild<QPushButton *>("hexReadSelected"))
    readPage->setEnabled(false);
  if (JLink::configurableFlashBanks(device->currentText()))
    filter->setToolTip(flashLayout.isEmpty()
        ? t("Sector map unavailable: choose the MCU DBS mode in preset configuration.", "Карта секторов недоступна: выберите режим DBS микроконтроллера в настройке пресета.")
        : t("Sector map is selected manually and does not verify or change the MCU DBS option. Bank labels assume no bank swap.", "Карта секторов выбрана вручную и не проверяет и не изменяет бит DBS МК. Названия банков указаны без перестановки."));
  else
    filter->setToolTip({});
  const auto sectors = JLink::flashSectors(device->currentText(), flashLayout);
  if (sectors.isEmpty() && JLink::configurableFlashBanks(device->currentText())) {
    filter->addItem(t("Choose DBS in preset settings", "Укажите DBS в настройке пресета"));
    if (auto model = qobject_cast<QStandardItemModel *>(filter->model()))
      model->item(filter->count() - 1)->setEnabled(false);
  }
  for (qsizetype i = 0; i < sectors.size(); ++i) {
    const auto &s = sectors[i];
    filter->addItem(sectorLabel(s, int(i)) + QString(" · 0x%1 · %2 KiB").arg(s.address, 8, 16, QChar('0')).arg(s.length / 1024),
                    QVariantMap{{"address", s.address}, {"length", s.length}});
  }
  updateHexPage();
}
void MainWindow::readSectors() {
  const auto sectors = JLink::flashSectors(device->currentText(), flashLayout);
  if (sectors.isEmpty()) {
    QMessageBox::information(this, t("Sectors unavailable", "Секторы недоступны"),
      JLink::configurableFlashBanks(device->currentText())
        ? t("Select Single or Dual bank in preset configuration to match the MCU DBS option bit. This does not change the MCU.", "Выберите Single или Dual bank в настройке пресета в соответствии с битом DBS МК. Это не меняет настройки МК.")
        : t("No verified sector layout for this device. Use a manual read range.", "Для этого МК нет проверенной разметки секторов. Укажите диапазон вручную."));
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(t("Read flash sectors", "Чтение секторов флеш"));
  auto layout = make<QVBoxLayout>(&dialog);
  layout->addWidget(make<QLabel>(t("Select sectors to read into the buffer:", "Выберите секторы для чтения в буфер:")));
  auto list = make<QListWidget>();
  list->setObjectName("sectorList");
  list->setSelectionMode(QAbstractItemView::NoSelection);
  for (qsizetype i = 0; i < sectors.size(); ++i) {
    const auto &s = sectors[i];
    auto item = make<QListWidgetItem>(sectorLabel(s, int(i)) + QString("    0x%1 – 0x%2    %3 KiB")
       .arg(s.address, 8, 16, QChar('0')).arg(s.address + s.length - 1, 8, 16, QChar('0')).arg(s.length / 1024), list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  layout->addWidget(list);
  auto buttons = make<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText(t("Read selected", "Прочитать выбранные"));
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.resize(520, 480);
  if (dialog.exec() != QDialog::Accepted) return;
  QVector<MemoryRange> selected;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->checkState() == Qt::Checked) selected.append(sectors[i]);
  if (selected.isEmpty()) {
    fail(t("Select at least one sector", "Выберите хотя бы один сектор"));
    return;
  }
  if (!confirmDiscardChanges()) return;
  try {
    saveConfig();
    link.execute(config(), Operation::Read, firmware, 0, 0, selected);
  } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}
void MainWindow::openComparisonFile() {
  auto path = QFileDialog::getOpenFileName(
      this, t("Open comparison file", "Открыть файл для сравнения"),
      settings.value("lastDir").toString(), "Firmware (*.hex *.bin)");
  if (path.isEmpty())
    return;
  openComparison(path);
}
void MainWindow::openComparison(const QString &path) {
  try {
    comparison = Firmware::load(path, addressValue(base->text()));
    comparisonPath = path;
    findChild<QLabel *>("comparisonHint")->show();
    findChild<QPushButton *>("closeComparison")->setEnabled(true);
    comparisonLabel->setText(QFileInfo(path).fileName());
    comparisonLabel->setToolTip(path);
    findChild<QWidget *>("comparisonPane")->setVisible(true);
    updateHexPage();
    findChild<QSplitter *>("memorySplit")->setSizes({1, 1});
    QTimer::singleShot(0, this, [this] {
      if (!comparisonPath.isEmpty())
        comparisonView->verticalScrollBar()->setValue(memoryView->verticalScrollBar()->value());
    });
  } catch (const std::exception &e) {
    fail(QString::fromUtf8(e.what()));
  }
}
void MainWindow::setStatus(int probe, int target) {
  const auto *theme = spotty::ThemeManager::instance();
  const auto colors = theme ? theme->colors() : spotty::ThemeColors{};
  const auto icon = [&](int state) {
    const auto color = state < 0 ? colors.textMuted : state ? colors.okText : colors.errorText;
    return MdiIcons::icon(state < 0 ? mdi::CircleOutline : mdi::CheckboxBlankCircle, color, 18)
        .pixmap(18, 18);
  };
  probeDot->setPixmap(icon(probe));
  targetDot->setPixmap(icon(target));
  probeStatus->setProperty("connectionState", probe);
  targetStatus->setProperty("connectionState", target);
  probeStatus->setText(probe < 0 ? t("J-Link · not checked", "J-Link · не проверен")
                       : probe ? t("J-Link · connected", "J-Link · подключён")
                               : t("J-Link · connection failed", "J-Link · ошибка подключения"));
  targetStatus->setText(target < 0 ? t("Target · not checked", "МК · не проверен")
                        : target ? t("Target · connected", "МК · подключён")
                                 : t("Target · connection failed", "МК · ошибка подключения"));
  probeDot->setToolTip(probeStatus->text());
  targetDot->setToolTip(targetStatus->text());
}
void MainWindow::presetDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(t("Preset configuration", "Настройка пресета"));
  dialog.resize(620, 420);
  auto layout = make<QVBoxLayout>(&dialog);
  auto form = make<QFormLayout>();
  auto name = make<QLineEdit>(favorites->currentIndex() > 0 ? favorites->currentText()
                                                             : device->currentText());
  auto selectedDevice = make<QComboBox>();
  selectedDevice->setObjectName("presetDevice");
  selectedDevice->setEditable(true);
  for (int i = 0; i < device->count(); ++i)
    selectedDevice->addItem(device->itemText(i));
  selectedDevice->setCurrentText(device->currentText());
  auto deviceCompleter = make<QCompleter>(selectedDevice->model(), selectedDevice);
  deviceCompleter->setCaseSensitivity(Qt::CaseInsensitive);
  deviceCompleter->setFilterMode(Qt::MatchContains);
  selectedDevice->setCompleter(deviceCompleter);
  auto selectedTransport = make<QComboBox>();
  selectedTransport->addItems({"USB", "Remote Server"});
  selectedTransport->setCurrentIndex(transport->currentIndex());
  auto selectedSerial = make<QLineEdit>(serial->currentText());
  auto selectedRemote = make<QLineEdit>(remote->text());
  auto selectedInterface = make<QComboBox>();
  selectedInterface->addItems({"SWD", "JTAG"});
  selectedInterface->setCurrentText(interface->currentText());
  auto selectedSpeed = make<QComboBox>();
  selectedSpeed->setEditable(true);
  selectedSpeed->addItems({"100", "1000", "4000", "8000", "auto", "adaptive"});
  selectedSpeed->setCurrentText(speed->currentText());
  auto selectedBase = make<QLineEdit>(base->text());
  auto selectedBank = make<QComboBox>();
  selectedBank->setObjectName("presetFlashLayout");
  selectedBank->addItem(t("Not specified", "Не указана"), "");
  selectedBank->addItem(t("Single bank (DBS = 0)", "Один банк (DBS = 0)"), "single");
  selectedBank->addItem(t("Dual bank (DBS = 1)", "Два банка (DBS = 1)"), "dual");
  selectedBank->setCurrentIndex(qMax(0, selectedBank->findData(flashLayout)));
  selectedBank->setEnabled(JLink::configurableFlashBanks(selectedDevice->currentText()));
  selectedBank->setToolTip(t("Choose the existing MCU DBS configuration. This setting only changes the displayed sector map; it does not read or write option bytes. Bank labels assume no bank swap.",
                             "Выберите текущую настройку DBS микроконтроллера. Меняется только отображаемая разметка; option bytes не считываются и не записываются. Названия банков указаны без перестановки банков."));
  connect(selectedDevice, &QComboBox::currentTextChanged, &dialog, [selectedDevice, selectedBank](const QString &) {
    selectedBank->setCurrentIndex(0);
    selectedBank->setEnabled(JLink::configurableFlashBanks(selectedDevice->currentText()));
  });
  auto deviceRefresh = make<QPushButton>(MdiIcons::icon(mdi::Refresh),
                                         t("Refresh MCU list", "Обновить список МК"));
  deviceRefresh->setObjectName("refreshPresetDevices");
  form->addRow(t("Preset name", "Имя пресета"), name);
  form->addRow(t("Target MCU", "Микроконтроллер"), selectedDevice);
  form->addRow(t("Flash layout", "Разметка флеш"), selectedBank);
  form->addRow(t("Connection", "Подключение"), selectedTransport);
  form->addRow(t("USB serial number", "Серийный номер USB"), selectedSerial);
  form->addRow(t("Remote Server", "Remote Server"), selectedRemote);
  form->addRow(t("Interface", "Интерфейс"), selectedInterface);
  form->addRow(t("Speed, kHz", "Частота, кГц"), selectedSpeed);
  form->addRow(t("BIN base address", "Базовый адрес BIN"), selectedBase);
  form->addRow(deviceRefresh);
  layout->addLayout(form);
  auto note = make<QLabel>(t("Flash layout must match MCU DBS option bytes; this preset does not change or verify them. Bank labels assume no bank swap.",
                             "Разметка должна соответствовать биту DBS в option bytes МК; пресет не изменяет и не проверяет его. Названия банков указаны без их перестановки."));
  note->setObjectName("hintLabel");
  note->setWordWrap(true);
  layout->addWidget(note);
  auto buttons = make<QDialogButtonBox>(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  auto remove = buttons->addButton(t("Delete", "Удалить"), QDialogButtonBox::DestructiveRole);
  remove->setEnabled(favorites->currentIndex() > 0);
  layout->addWidget(buttons);
  auto transportState = [=] {
    const bool usb = selectedTransport->currentIndex() == 0;
    selectedSerial->setEnabled(usb);
    selectedRemote->setEnabled(!usb);
  };
  connect(selectedTransport, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
          transportState);
  transportState();
  connect(deviceRefresh, &QPushButton::clicked, this, [this] { refreshDevices(); });
  connect(&link, &JLink::devices, &dialog, [selectedDevice](const QStringList &devices) {
    const auto previous = selectedDevice->currentText();
    const QSignalBlocker blocker(selectedDevice);
    selectedDevice->clear();
    selectedDevice->addItems(devices);
    selectedDevice->setCurrentText(previous);
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(remove, &QPushButton::clicked, &dialog, [&] {
    const auto oldName = favorites->currentText();
    auto all = settings.value("presets").toMap();
    all.remove(oldName);
    settings.setValue("presets", all);
    settings.setValue("favoriteNames", all.keys());
    favorites->clear();
    favorites->addItem(t("Favorite presets…", "Избранные пресеты…"));
    favorites->addItems(all.keys());
    saveConfig();
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    const auto trimmed = name->text().trimmed();
    if (trimmed.isEmpty()) {
      QMessageBox::warning(&dialog, "Tailor", t("Preset name is required", "Укажите имя пресета"));
      return;
    }
    QJsonObject object{{"device", selectedDevice->currentText()},
                       {"flashLayout", selectedBank->isEnabled() ? selectedBank->currentData().toString() : QString()},
                       {"interface", selectedInterface->currentText()},
                       {"speed", selectedSpeed->currentText()},
                       {"transport", selectedTransport->currentIndex()},
                       {"serial", selectedSerial->text()},
                       {"remote", selectedRemote->text()},
                       {"base", selectedBase->text()}};
    auto all = settings.value("presets").toMap();
    if (favorites->currentIndex() > 0 && favorites->currentText() != trimmed)
      all.remove(favorites->currentText());
    all[trimmed] = QJsonDocument(object).toJson(QJsonDocument::Compact);
    settings.setValue("presets", all);
    settings.setValue("favoriteNames", all.keys());
    favorites->clear();
    favorites->addItem(t("Favorite presets…", "Избранные пресеты…"));
    favorites->addItems(all.keys());
    favorites->setCurrentText(trimmed);
    applyFavorite();
    dialog.accept();
  });
  dialog.exec();
}
void MainWindow::applyFavorite() {
  if (favorites->currentIndex() == 0)
    return;
  auto all = settings.value("presets").toMap();
  auto o =
      QJsonDocument::fromJson(all.value(favorites->currentText()).toByteArray())
          .object();
  device->setCurrentText(JLink::deviceName(o["device"].toString()));
  flashLayout = o["flashLayout"].toString();
  if (flashLayout != "single" && flashLayout != "dual") flashLayout.clear();
  flashLayoutDevice = device->currentText();
  updateSectorFilter();
  interface->setCurrentText(o["interface"].toString());
  speed->setCurrentText(o["speed"].toString());
  transport->setCurrentIndex(o["transport"].toInt());
  serial->setCurrentText(o["serial"].toString());
  remote->setText(o["remote"].toString());
  base->setText(o["base"].toString());
  if (!filePath.isEmpty())
    openFile(filePath);
  saveConfig();
}
void MainWindow::applicationSettingsDialog() {
  QDialog d(this);
  d.setWindowTitle(t("Settings", "Настройки"));
  d.resize(650, 350);
  QFormLayout form(&d);
  QLineEdit exe(settings.value("commander").toString());
  auto row = make<QWidget>();
  auto layout = make<QHBoxLayout>(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(&exe);
  QPushButton browse(MdiIcons::icon(mdi::FolderOpen), t("Browse", "Обзор"));
  layout->addWidget(&browse);
  connect(&browse, &QPushButton::clicked, &d, [&] {
    auto p = QFileDialog::getOpenFileName(&d, "J-Link Commander", exe.text());
    if (!p.isEmpty())
      exe.setText(p);
  });
  form.addRow("J-Link Commander", row);
  QComboBox erase;
  erase.addItems({t("Affected sectors (SEGGER loader)",
                    "Затронутые секторы (загрузчик SEGGER)"),
                  t("Entire flash", "Вся флеш")});
  erase.setCurrentIndex(settings.value("fullErase", false).toBool() ? 1 : 0);
  form.addRow(t("One Click erase", "Стирание One Click"), &erase);
  QCheckBox run(t("Reset and run after programming",
                  "Сбросить и запустить после записи"));
  run.setChecked(settings.value("runAfter", true).toBool());
  form.addRow(&run);
  QSpinBox timeout;
  timeout.setRange(5, 3600);
  timeout.setValue(settings.value("timeout", 120).toInt());
  timeout.setSuffix(" s");
  form.addRow(t("Operation timeout", "Таймаут операции"), &timeout);
  QComboBox checksumAlgorithm;
  checksumAlgorithm.setObjectName("checksumAlgorithm");
  checksumAlgorithm.addItem("MD5", "md5");
  checksumAlgorithm.addItem("CRC-32/Ethernet", "crc32");
  checksumAlgorithm.addItem("SHA-256", "sha256");
  checksumAlgorithm.setCurrentIndex(qMax(0, checksumAlgorithm.findData(settings.value("checksumAlgorithm", "md5"))));
  form.addRow(t("Buffer checksum", "Контрольная сумма буфера"), &checksumAlgorithm);
  QComboBox language;
  language.addItem("English", "en");
  language.addItem("Русский", "ru");
  language.setCurrentIndex(language.findData(russian ? "ru" : "en"));
  form.addRow(t("Language (restart required)", "Язык (после перезапуска)"),
              &language);
  QLabel note(t("Unlock method is selected automatically for the preset and installed J-Link software.",
                "Метод unlock выбирается автоматически по пресету и установленному ПО J-Link."));
  note.setWordWrap(true);
  form.addRow(&note);
  QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  form.addRow(&buttons);
  connect(&buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  connect(&buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  if (d.exec() == QDialog::Accepted) {
    const auto previousCommander = settings.value("commander").toString();
    settings.setValue("commander", exe.text());
    settings.setValue("fullErase", erase.currentIndex() == 1);
    settings.setValue("runAfter", run.isChecked());
    settings.setValue("timeout", timeout.value());
    settings.setValue("language", language.currentData());
    settings.setValue("checksumAlgorithm", checksumAlgorithm.currentData());
    updateSummary();
    appendLog("Commander: " + exe.text());
    if (previousCommander != exe.text()) {
      flashRanges.clear();
      if (!readRangeEdited)
        updateReadRange();
      if (QFileInfo(exe.text()).isExecutable())
        QTimer::singleShot(0, this, &MainWindow::refreshDevices);
    }
    updateUnlockAvailability();
  }
  layout->removeWidget(&exe);
  layout->removeWidget(&browse);
  exe.setParent(nullptr);
  browse.setParent(nullptr);
}
void MainWindow::closeEvent(QCloseEvent *e) {
  if (link.busy()) {
    QMessageBox::information(this, "Tailor",
                             t("Cancel the operation before closing.",
                               "Отмените операцию перед закрытием."));
    e->ignore();
    return;
  }
  if (!confirmDiscardChanges()) {
    e->ignore();
    return;
  }
  saveConfig();
  e->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
  if (e->mimeData()->hasUrls() && !link.busy())
    e->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *e) {
  if (!e->mimeData()->urls().isEmpty())
    openFile(e->mimeData()->urls().first().toLocalFile());
}
