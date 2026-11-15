#include "MainWindow.h"
#include "MdiIcons.h"
#include "ThemeManager.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QScrollBar>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QtTest>
#include <memory>
class UiTest : public QObject {
  Q_OBJECT
  std::unique_ptr<spotty::ThemeManager> theme;
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("TailorTests");
    QCoreApplication::setApplicationName("UiTest");
    theme = std::make_unique<spotty::ThemeManager>();
    theme->setTheme(spotty::ThemeManager::Theme::Dark);
    QVERIFY(spotty::MdiIcons::initialize());
  }
  void settingsAndFile() {
    QSettings settings;
    settings.clear();
    settings.setValue("commander", "/missing/JLinkExe");
    settings.setValue("flashCatalogCommander", "/missing/JLinkExe");
    settings.setValue("flashRanges", QVariantMap{{"STM32F407VG", QVariantMap{{"address", 0x08000000u}, {"length", 0x00100000u}}}});
    settings.setValue("devices", QStringList{"\"ST\", \"STM32F407VG\", \"Cortex-M4\""});
    settings.setValue("device", "\"ST\", \"STM32F407VG\", \"Cortex-M4\"");
    MainWindow w;
    auto readAddress = w.findChild<QLineEdit *>("readAddress");
    auto readSize = w.findChild<QLineEdit *>("readSize");
    QVERIFY(readAddress);
    QVERIFY(readSize);
    QCOMPARE(readAddress->text(), QString("0x08000000"));
    QCOMPARE(readSize->text(), QString("1M"));
    bool normalizedDevice = false;
    for (const auto combo : w.findChildren<QComboBox *>())
      normalizedDevice |= combo->currentText() == "STM32F407VG";
    QVERIFY(normalizedDevice);
    w.show();
    QTest::qWait(20);
    auto tabs = w.findChild<QTabWidget *>();
    QVERIFY(tabs);
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(tabs->tabText(0), QString("Program"));
    tabs->tabBar()->setFocus();
    QTest::keyClick(tabs->tabBar(), Qt::Key_Right);
    QCOMPARE(tabs->currentIndex(), 1);
    QTest::keyClick(tabs->tabBar(), Qt::Key_Left);
    QCOMPARE(tabs->currentIndex(), 0);
    if (const auto screenshot = qEnvironmentVariable("TAILOR_UI_SCREENSHOT"); !screenshot.isEmpty()) {
      const auto width = qEnvironmentVariableIntValue("TAILOR_UI_WIDTH");
      const auto height = qEnvironmentVariableIntValue("TAILOR_UI_HEIGHT");
      w.resize(width > 0 ? width : 900, height > 0 ? height : 650);
      QTest::qWait(20);
      w.grab().save(screenshot);
      tabs->setCurrentIndex(1);
      QTest::qWait(20);
      w.grab().save(screenshot + ".hex.png");
      tabs->setCurrentIndex(0);
    }
    auto buttons = w.findChildren<QPushButton *>();
    QVERIFY(std::any_of(buttons.begin(), buttons.end(), [](auto b) { return b->text() == "Halt"; }));
    QVERIFY(std::any_of(buttons.begin(), buttons.end(), [](auto b) { return b->text() == "Resume"; }));
    auto it = std::find_if(buttons.begin(), buttons.end(), [](auto b) {
      return b->text() == QString("Application settings");
    });
    QVERIFY(it != buttons.end());
    bool languageOptionsVisible = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto d = qobject_cast<QDialog *>(widget)) {
          for (auto combo : d->findChildren<QComboBox *>())
            if (combo->findData("en") >= 0 && combo->findData("ru") >= 0)
              languageOptionsVisible = combo->currentData().toString() == "en";
          d->reject();
        }
    });
    (*it)->click();
    QVERIFY(languageOptionsVisible);
    QVERIFY(w.isVisible());
    QTemporaryDir temp;
    QFile f(temp.filePath("test.bin"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("abcdefgh");
    f.close();
    w.openFile(f.fileName());
    auto labels = w.findChildren<QLabel *>();
    const auto summary = std::find_if(labels.begin(), labels.end(), [](auto label) {
      return label->property("bufferSummary").toBool();
    });
    QVERIFY(summary != labels.end());
    QCOMPARE((*summary)->text(), QString("Buffer: 8 bytes  ·  File: 8 bytes\nMD5: e8dc4081b13434b45189a720b77b6818"));
    const auto baseAddress = w.findChild<QLineEdit *>("baseAddress");
    const auto openFirmware = w.findChild<QPushButton *>("openFirmware");
    const auto bufferIcon = w.findChild<QLabel *>("bufferIcon");
    QVERIFY(!w.findChild<QLineEdit *>("bufferChecksum"));
    QVERIFY(baseAddress && openFirmware && bufferIcon);
    w.resize(900, 650);
    QTest::qWait(20);
    QCOMPARE(baseAddress->geometry().center().y(), openFirmware->geometry().center().y());
    QVERIFY(baseAddress->geometry().left() > openFirmware->geometry().right());
    QVERIFY(baseAddress->geometry().right() < baseAddress->parentWidget()->width());
    QVERIFY(bufferIcon->geometry().left() < (*summary)->geometry().left());
    QVERIFY((*summary)->geometry().right() < (*summary)->parentWidget()->width());
    QFile hex(temp.filePath("test.hex"));
    QVERIFY(hex.open(QIODevice::WriteOnly));
    const auto hexData = Firmware{{{0x08000000, "abcd"}, {0x08001000, "efgh"}}}.toHex();
    QCOMPARE(hex.write(hexData), hexData.size());
    hex.close();
    w.openFile(hex.fileName());
    QVERIFY((*summary)->text().contains("MD5: e8dc4081b13434b45189a720b77b6818"));
    QVERIFY((*summary)->text().contains("Buffer: 8 bytes"));
    QVERIFY((*summary)->text().contains(QString("File: %1 bytes").arg(hexData.size())));
    auto selectChecksum = [&](const QString &algorithm, bool accept) {
      bool found = false;
      QTimer::singleShot(30, [&] {
        for (auto widget : QApplication::topLevelWidgets())
          if (auto dialog = qobject_cast<QDialog *>(widget))
            if (auto combo = dialog->findChild<QComboBox *>("checksumAlgorithm")) {
              const auto index = combo->findData(algorithm);
              if (index >= 0) {
                found = true;
                combo->setCurrentIndex(index);
                accept ? dialog->accept() : dialog->reject();
              }
            }
      });
      (*it)->click();
      return found;
    };
    QVERIFY(selectChecksum("crc32", true));
    QCOMPARE(settings.value("checksumAlgorithm").toString(), QString("crc32"));
    QVERIFY((*summary)->text().contains("CRC-32/Ethernet: aeef2a50"));
    QVERIFY(selectChecksum("sha256", true));
    QCOMPARE(settings.value("checksumAlgorithm").toString(), QString("sha256"));
    QVERIFY((*summary)->text().contains("SHA-256: 9c56cc51b374c3ba189210d5b6d4bf57790d351c96c47c02190ecf1e430635ab"));
    QTest::qWait(20);
    QVERIFY((*summary)->height() >= (*summary)->heightForWidth((*summary)->width()));
    QVERIFY(selectChecksum("md5", false));
    QCOMPARE(settings.value("checksumAlgorithm").toString(), QString("sha256"));
    QVERIFY((*summary)->text().contains("SHA-256:"));
    {
      MainWindow reopened;
      reopened.openFile(hex.fileName());
      const auto reopenedLabels = reopened.findChildren<QLabel *>();
      const auto reopenedSummary = std::find_if(reopenedLabels.begin(), reopenedLabels.end(), [](auto label) {
        return label->property("bufferSummary").toBool();
      });
      QVERIFY(reopenedSummary != reopenedLabels.end());
      QVERIFY((*reopenedSummary)->text().contains("SHA-256:"));
    }
    auto memory = w.findChild<QTableView *>("memoryView");
    QVERIFY(memory);
    QVERIFY(memory->model()->rowCount() > 0);
    if (const auto screenshot = qEnvironmentVariable("TAILOR_UI_SCREENSHOT"); !screenshot.isEmpty()) {
      w.grab().save(screenshot + ".loaded.png");
      tabs->setCurrentIndex(1);
      QTest::qWait(20);
      w.grab().save(screenshot + ".loaded.hex.png");
    }
    w.grab().save(temp.filePath("ui.png"));
    settings.clear();
  }
  void hexComparisonAndSectors() {
    QSettings settings;
    settings.clear();
    settings.setValue("commander", "/missing/JLinkExe");
    settings.setValue("flashCatalogCommander", "/missing/JLinkExe");
    settings.setValue("devices", QStringList{"STM32F407ZG"});
    settings.setValue("device", "STM32F407ZG");
    QTemporaryDir dir;
    const auto workingPath = dir.filePath("working.hex");
    const auto comparisonPath = dir.filePath("comparison.hex");
    QFile working(workingPath), comparisonFile(comparisonPath);
    QVERIFY(working.open(QIODevice::WriteOnly));
    working.write(Firmware{{{0x08000000, QByteArray(2048, 'A')}, {0x08010000, " More "}}}.toHex());
    working.close();
    QVERIFY(comparisonFile.open(QIODevice::WriteOnly));
    auto bytes = QByteArray(2048, 'A');
    bytes[5] = 'B';
    bytes[15] = 'C';
    comparisonFile.write(Firmware{{{0x08000000, bytes}, {0x08020000, "other"}}}.toHex());
    comparisonFile.close();
    MainWindow w;
    w.openFile(workingPath);
    w.resize(900, 650);
    w.show();
    auto tabs = w.findChild<QTabWidget *>();
    tabs->setCurrentIndex(1);
    const auto view = w.findChild<QTableView *>("memoryView");
    const auto peer = w.findChild<QTableView *>("comparisonView");
    const auto filter = w.findChild<QComboBox *>("hexSectorFilter");
    QVERIFY(view && peer && filter);
    QVERIFY(!view->showGrid());
    QVERIFY(!peer->showGrid());
    QCOMPARE(filter->currentIndex(), 0);
    QVERIFY(view->model()->rowCount() > 128);
    QCOMPARE(filter->count(), 13);
    filter->setCurrentIndex(5);
    QCOMPARE(view->model()->rowCount(), 1);
    const auto search = w.findChild<QLineEdit *>("hexSearch");
    const auto findAscii = w.findChild<QPushButton *>("findAscii");
    QVERIFY(search && findAscii);
    search->setText(" More ");
    findAscii->click();
    QCOMPARE(filter->currentIndex(), 0);
    QCOMPARE(view->currentIndex().data().toString(), QString("20"));
    QCOMPARE(view->model()->index(view->currentIndex().row(), 0).data().toString(), QString("08010000"));
    bool rejectedNonAscii = false;
    search->setText(QString::fromUtf8("тест"));
    QTimer::singleShot(20, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto message = qobject_cast<QMessageBox *>(widget)) {
          rejectedNonAscii = message->text().contains("ASCII");
          message->accept();
        }
    });
    findAscii->click();
    QVERIFY(rejectedNonAscii);
    w.resize(1600, 800);
    w.openComparison(comparisonPath);
    QTest::qWait(30);
    const auto split = w.findChild<QSplitter *>("memorySplit");
    QVERIFY(split);
    QVERIFY(split->width() < w.width() - 200);
    QVERIFY(qAbs(split->mapTo(&w, split->rect().center()).x() - w.width() / 2) < 5);
    QCOMPARE(view->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(peer->horizontalScrollBar()->maximum(), 0);
    if (const auto screenshot = qEnvironmentVariable("TAILOR_UI_COMPARISON_WIDE_SCREENSHOT"); !screenshot.isEmpty())
      w.grab().save(screenshot);
    w.resize(900, 650);
    QTest::qWait(30);
    QVERIFY(view->horizontalScrollBar()->maximum() > 0);
    view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->maximum());
    QCOMPARE(view->horizontalScrollBar()->value(), peer->horizontalScrollBar()->value());
    if (const auto screenshot = qEnvironmentVariable("TAILOR_UI_COMPARISON_SCREENSHOT"); !screenshot.isEmpty())
      w.grab().save(screenshot);
    QVERIFY(peer->isVisible());
    QCOMPARE(view->model()->rowCount(), peer->model()->rowCount());
    QVERIFY(view->model()->index(0, 6).data(Qt::BackgroundRole).isValid());
    QVERIFY(view->model()->index(0, 16).data(Qt::BackgroundRole).isValid());
    QVERIFY(!view->model()->index(0, 5).data(Qt::BackgroundRole).isValid());
    view->verticalScrollBar()->setValue(20);
    QCOMPARE(view->verticalScrollBar()->value(), peer->verticalScrollBar()->value());
    QCOMPARE(view->model()->columnCount(), 18);
    QCOMPARE(view->model()->headerData(16, Qt::Horizontal).toString(), QString("F"));
    w.findChild<QPushButton *>("closeComparison")->click();
    QVERIFY(!peer->isVisible());
    QCOMPARE(view->model()->columnCount(), 18);
    QCOMPARE(filter->currentIndex(), 0);
    settings.clear();
  }
  void russianPreference() {
    QSettings settings;
    settings.clear();
    settings.setValue("language", "ru");
    settings.setValue("commander", "/missing/JLinkExe");
    settings.setValue("flashCatalogCommander", "/missing/JLinkExe");
    settings.setValue("flashRanges", QVariantMap{{"STM32F407VG", QVariantMap{{"address", 0x08000000u}, {"length", 0x00100000u}}}});
    settings.setValue("devices", QStringList{"STM32F407VG"});
    MainWindow w;
    w.resize(900, 650);
    w.show();
    QTest::qWait(20);
    const auto baseAddress = w.findChild<QLineEdit *>("baseAddress");
    const auto openFirmware = w.findChild<QPushButton *>("openFirmware");
    QVERIFY(baseAddress && openFirmware);
    QCOMPARE(baseAddress->geometry().center().y(), openFirmware->geometry().center().y());
    QVERIFY(baseAddress->geometry().left() > openFirmware->geometry().right());
    QVERIFY(baseAddress->geometry().right() < baseAddress->parentWidget()->width());
    auto tabs = w.findChild<QTabWidget *>();
    QVERIFY(tabs);
    QCOMPARE(tabs->tabText(0), QString::fromUtf8("Прошивка"));
    tabs->setCurrentIndex(1);
    QTest::qWait(20);
    const auto saveHex = w.findChild<QPushButton *>("hexSaveBuffer");
    const auto filter = w.findChild<QComboBox *>("hexSectorFilter");
    QVERIFY(saveHex && filter);
    QCOMPARE(saveHex->geometry().center().y(), filter->geometry().center().y());
    QVERIFY(filter->geometry().right() < filter->parentWidget()->width());
    tabs->setCurrentIndex(0);
    if (const auto screenshot = qEnvironmentVariable("TAILOR_UI_SCREENSHOT"); !screenshot.isEmpty()) {
      w.resize(900, 650);
      w.show();
      QTest::qWait(20);
      w.grab().save(screenshot + ".ru.png");
    }
    settings.clear();
  }
  void editAndSaveBuffer() {
    QSettings settings;
    settings.clear();
    settings.setValue("commander", "/missing/JLinkExe");
    settings.setValue("flashCatalogCommander", "/missing/JLinkExe");
    QTemporaryDir dir;
    const auto source = dir.filePath("source.hex");
    const auto saved = dir.filePath("edited.hex");
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(Firmware{{{0x08000000, "AB"}, {0x08001000, "CD"}}}.toHex());
    file.close();
    MainWindow w;
    w.openFile(source);
    w.resize(1200, 800);
    w.show();
    w.findChild<QTabWidget *>()->setCurrentIndex(1);
    QTest::qWait(30);
    auto view = w.findChild<QTableView *>("memoryView");
    auto model = view->model();
    const auto split = w.findChild<QSplitter *>("memorySplit");
    QVERIFY(split);
    QVERIFY(split->width() > 500 && split->width() < 850);
    QVERIFY(qAbs(split->mapTo(&w, split->rect().center()).x() - w.width() / 2) < 5);
    const auto cell = model->index(0, 1);
    QVERIFY(cell.flags() & Qt::ItemIsEditable);
    QVERIFY(!(model->index(0, 0).flags() & Qt::ItemIsEditable));
    QVERIFY(!(model->index(0, 3).flags() & Qt::ItemIsEditable));
    QVERIFY(!model->setData(cell, "ZZ", Qt::EditRole));
    QVERIFY(!model->setData(cell, "F", Qt::EditRole));
    QCOMPARE(cell.data().toString(), QString("41"));
    view->setCurrentIndex(cell);
    view->edit(cell);
    QCoreApplication::processEvents();
    auto editor = view->findChild<QLineEdit *>();
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClicks(editor, "5A");
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_COMPARE(cell.data().toString(), QString("5A"));
    QVERIFY(model->index(0, 17).data().toString().startsWith("ZB"));
    const auto labels = w.findChildren<QLabel *>();
    const auto summary = std::find_if(labels.begin(), labels.end(), [](auto label) { return label->property("bufferSummary").toBool(); });
    QVERIFY(summary != labels.end());
    QVERIFY((*summary)->text().contains(QString::fromLatin1(QCryptographicHash::hash("ZBCD", QCryptographicHash::Md5).toHex())));
    bool canceled = false;
    QTimer::singleShot(20, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto message = qobject_cast<QMessageBox *>(widget)) {
          canceled = true;
          message->button(QMessageBox::Cancel)->click();
        }
    });
    w.openFile(source);
    QVERIFY(canceled);
    QCOMPARE(cell.data().toString(), QString("5A"));
    QVERIFY(w.saveBufferTo(saved));
    const auto result = Firmware::load(saved, 0);
    QCOMPARE(result.segments.size(), 2);
    QCOMPARE(result.segments.first().data, QByteArray("ZB"));
    QCOMPARE(Firmware::load(source, 0).segments.first().data, QByteArray("AB"));
    QVERIFY(w.close());
    settings.clear();
  }
  void closingWithActiveEditPrompts() {
    QSettings settings;
    settings.clear();
    settings.setValue("commander", "/missing/JLinkExe");
    settings.setValue("flashCatalogCommander", "/missing/JLinkExe");
    QTemporaryDir dir;
    QFile file(dir.filePath("data.bin"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("AB");
    file.close();
    MainWindow w;
    w.openFile(file.fileName());
    w.show();
    w.findChild<QTabWidget *>()->setCurrentIndex(1);
    auto view = w.findChild<QTableView *>("memoryView");
    auto cell = view->model()->index(0, 1);
    view->setCurrentIndex(cell);
    view->edit(cell);
    QCoreApplication::processEvents();
    auto editor = view->findChild<QLineEdit *>();
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClicks(editor, "5A");
    bool prompted = false;
    QTimer::singleShot(20, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto message = qobject_cast<QMessageBox *>(widget)) {
          prompted = true;
          message->button(QMessageBox::Cancel)->click();
        }
    });
    QVERIFY(!w.close());
    QVERIFY(prompted);
    QCOMPARE(cell.data().toString(), QString("5A"));
    QVERIFY(w.saveBufferTo(dir.filePath("edited.bin")));
    QVERIFY(w.close());
    settings.clear();
  }
  void readKeepsCurrentTab() {
    QSettings settings;
    settings.clear();
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    QVERIFY(QFileInfo(fake).isExecutable());
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("flashRanges", QVariantMap{{"STM32F407VG", QVariantMap{{"address", 0x08000000u}, {"length", 32u}}}});
    settings.setValue("devices", QStringList{"STM32F407VG"});
    settings.setValue("device", "STM32F407VG");
    MainWindow w;
    w.show();
    const auto tabs = w.findChild<QTabWidget *>();
    const auto memory = w.findChild<QTableView *>("memoryView");
    QVERIFY(tabs);
    QVERIFY(memory);
    auto buttons = w.findChildren<QPushButton *>();
    const auto read = std::find_if(buttons.begin(), buttons.end(), [](auto button) {
      return button->text() == "Read memory";
    });
    QVERIFY(read != buttons.end());
    QCOMPARE(tabs->currentIndex(), 0);
    (*read)->click();
    QTRY_VERIFY(memory->model()->rowCount() > 0);
    QCOMPARE(tabs->currentIndex(), 0);
    const auto labels = w.findChildren<QLabel *>();
    const auto summary = std::find_if(labels.begin(), labels.end(), [](auto label) {
      return label->property("bufferSummary").toBool();
    });
    QVERIFY(summary != labels.end());
    const auto md5 = QString::fromLatin1(QCryptographicHash::hash(QByteArray(32, 'X'), QCryptographicHash::Md5).toHex());
    QVERIFY((*summary)->text().contains("MD5: " + md5));
    settings.clear();
  }
  void readSelectedSectors() {
    QSettings settings;
    settings.clear();
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("devices", QStringList{"STM32F407ZG"});
    settings.setValue("device", "STM32F407ZG");
    MainWindow w;
    w.show();
    bool selected = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto dialog = qobject_cast<QDialog *>(widget))
          if (auto list = dialog->findChild<QListWidget *>("sectorList")) {
            list->item(0)->setCheckState(Qt::Checked);
            list->item(2)->setCheckState(Qt::Checked);
            selected = true;
            dialog->accept();
          }
    });
    w.findChild<QPushButton *>("readSectors")->click();
    QVERIFY(selected);
    auto view = w.findChild<QTableView *>("memoryView");
    QTRY_COMPARE(view->model()->rowCount(), 2048);
    QCOMPARE(view->model()->index(1024, 0).data().toString(), QString("08008000"));
    settings.clear();
  }
  void readSelectedHexPage() {
    QSettings settings;
    settings.clear();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("devices", QStringList{"GD32F450ZG"});
    settings.setValue("device", "GD32F450ZG");
    settings.setValue("flashLayout", "dual");
    settings.setValue("flashLayoutDevice", "GD32F450ZG");
    MainWindow w;
    w.show();
    const auto tabs = w.findChild<QTabWidget *>();
    tabs->setCurrentIndex(1);
    const auto filter = w.findChild<QComboBox *>("hexSectorFilter");
    const auto read = w.findChild<QPushButton *>("hexReadSelected");
    const auto view = w.findChild<QTableView *>("memoryView");
    QVERIFY(filter && read && view);
    QVERIFY(!read->isEnabled());
    filter->setCurrentIndex(9);
    QVERIFY(read->isEnabled());
    read->click();
    QTRY_COMPARE(view->model()->rowCount(), 1024);
    QCOMPARE(view->model()->index(0, 0).data().toString(), QString("08080000"));
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(!read->isEnabled());
    QVERIFY(view->model()->setData(view->model()->index(0, 1), "5A", Qt::EditRole));
    filter->setCurrentIndex(9);
    bool canceled = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto message = qobject_cast<QMessageBox *>(widget)) {
          canceled = true;
          message->button(QMessageBox::Cancel)->click();
        }
    });
    read->click();
    QVERIFY(canceled);
    QCOMPARE(view->model()->index(0, 1).data().toString(), QString("5A"));
    QVERIFY(w.saveBufferTo(dir.filePath("read-page.hex")));
    settings.clear();
  }
  void gd450BankPreset() {
    QSettings settings;
    settings.clear();
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("devices", QStringList{"GD32F450ZG"});
    settings.setValue("device", "GD32F450ZG");
    MainWindow w;
    w.show();
    const auto filter = w.findChild<QComboBox *>("hexSectorFilter");
    const auto configure = w.findChild<QPushButton *>("configurePreset");
    QVERIFY(filter && configure);
    QCOMPARE(filter->count(), 2);
    QVERIFY(filter->itemText(1).contains("DBS"));
    bool configured = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto dialog = qobject_cast<QDialog *>(widget))
          if (auto banks = dialog->findChild<QComboBox *>("presetFlashLayout")) {
            if (!banks->isEnabled() || !banks->currentData().toString().isEmpty()) { dialog->reject(); return; }
            banks->setCurrentIndex(banks->findData("dual"));
            if (const auto screenshot = qEnvironmentVariable("TAILOR_UI_BANK_SCREENSHOT"); !screenshot.isEmpty())
              dialog->grab().save(screenshot);
            configured = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
          }
    });
    configure->click();
    QVERIFY(configured);
    QCOMPARE(filter->count(), 17);
    QVERIFY(filter->itemText(9).contains("Sector 12"));
    QVERIFY(filter->itemText(9).contains("Bank 1"));
    QCOMPARE(filter->itemData(9).toMap().value("address").toUInt(), 0x08080000u);
    QCOMPARE(filter->itemData(9).toMap().value("length").toUInt(), 16u * 1024);
    QCOMPARE(settings.value("flashLayout").toString(), QString("dual"));
    {
      MainWindow reopened;
      QCOMPARE(reopened.findChild<QComboBox *>("hexSectorFilter")->count(), 17);
    }
    bool selected = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto dialog = qobject_cast<QDialog *>(widget))
          if (auto list = dialog->findChild<QListWidget *>("sectorList")) {
            selected = list->item(8)->text().contains("Sector 12");
            list->item(8)->setCheckState(Qt::Checked);
            dialog->accept();
          }
    });
    w.findChild<QPushButton *>("readSectors")->click();
    QVERIFY(selected);
    const auto view = w.findChild<QTableView *>("memoryView");
    QTRY_COMPARE(view->model()->rowCount(), 1024);
    QCOMPARE(view->model()->index(0, 0).data().toString(), QString("08080000"));
    bool changed = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets())
        if (auto dialog = qobject_cast<QDialog *>(widget))
          if (auto banks = dialog->findChild<QComboBox *>("presetFlashLayout")) {
            if (banks->currentData().toString() != "dual") { dialog->reject(); return; }
            banks->setCurrentIndex(banks->findData("single"));
            changed = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
          }
    });
    configure->click();
    QVERIFY(changed);
    QCOMPARE(filter->count(), 13);
    QVERIFY(filter->itemText(9).contains("Sector 8"));
    QCOMPARE(filter->itemData(9).toMap().value("length").toUInt(), 128u * 1024);
    QCOMPARE(settings.value("flashLayout").toString(), QString("single"));
    settings.clear();
  }
  void connectionErrorsAreVisible() {
    QSettings settings;
    settings.clear();
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("devices", QStringList{"STM32F407VG"});
    settings.setValue("device", "STM32F407VG");
    MainWindow w;
    w.show();
    const auto probe = w.findChild<QLabel *>("probeStatus");
    const auto target = w.findChild<QLabel *>("targetStatus");
    const auto id = w.findChild<QLabel *>("mcuId");
    const auto log = w.findChild<QPlainTextEdit *>("operationLog");
    QVERIFY(probe && target && log && id);
    QCOMPARE(probe->property("connectionState").toInt(), -1);
    auto buttons = w.findChildren<QPushButton *>();
    const auto check = std::find_if(buttons.begin(), buttons.end(), [](auto button) {
      return button->text() == "Check connection";
    });
    QVERIFY(check != buttons.end());
    (*check)->click();
    QTRY_COMPARE(id->text(), QString("MCU ID: 0x413"));
    QTRY_VERIFY((*check)->isEnabled());
    qputenv("TAILOR_FAKE_CONNECTION", "target");
    (*check)->click();
    QTRY_COMPARE(target->property("connectionState").toInt(), 0);
    QCOMPARE(probe->property("connectionState").toInt(), 1);
    QCOMPARE(id->text(), QString("MCU ID: unavailable"));
    const auto found = log->document()->find("ERROR: Cannot connect to target");
    QVERIFY(!found.isNull());
    QVERIFY(found.charFormat().fontWeight() >= QFont::Bold);
    qputenv("TAILOR_FAKE_CONNECTION", "probe");
    (*check)->click();
    QTRY_COMPARE(probe->property("connectionState").toInt(), 0);
    QCOMPARE(target->property("connectionState").toInt(), 0);
    qunsetenv("TAILOR_FAKE_CONNECTION");
    settings.clear();
  }
  void liveProgress() {
    QSettings settings;
    settings.clear();
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("devices", QStringList{"PROGRESS"});
    settings.setValue("device", "PROGRESS");
    MainWindow w;
    w.show();
    const auto bar = w.findChild<QProgressBar *>("operationProgress");
    const auto stage = w.findChild<QLabel *>("operationStage");
    QVERIFY(bar && stage);
    const auto buttons = w.findChildren<QPushButton *>();
    const auto check = std::find_if(buttons.begin(), buttons.end(), [](auto button) {
      return button->text() == "Check connection";
    });
    QVERIFY(check != buttons.end());
    (*check)->click();
    QTRY_COMPARE_WITH_TIMEOUT(bar->value(), 50, 3000);
    QCOMPARE(bar->maximum(), 100);
    QCOMPARE(stage->text(), QString("Programming flash"));
    QTRY_COMPARE_WITH_TIMEOUT(stage->text(), QString("Completed"), 3000);
    QCOMPARE(bar->value(), bar->maximum());
    qputenv("TAILOR_FAKE_PROGRESS_FAIL", "1");
    (*check)->click();
    QTRY_COMPARE_WITH_TIMEOUT(stage->text(), QString("Failed / canceled"), 3000);
    QCOMPARE(bar->value(), 0);
    qunsetenv("TAILOR_FAKE_PROGRESS_FAIL");
    settings.clear();
  }
  void restoresPresetAndUnlock() {
    QSettings settings;
    settings.clear();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
    const auto commander = temp.filePath("JLink.exe");
    const auto utility = temp.filePath("JLinkSTM32.exe");
#else
    const auto commander = temp.filePath("JLinkExe");
    const auto utility = temp.filePath("JLinkSTM32Exe");
#endif
    QVERIFY(QFile::copy(fake, commander));
    QVERIFY(QFile::copy(fake, utility));
    settings.setValue("commander", commander);
    settings.setValue("flashCatalogCommander", commander);
    settings.setValue("devices", QStringList{"STM32F407VG", "STM32F407ZG"});
    settings.setValue("device", "STM32F407VG");
    settings.setValue("favoriteNames", QStringList{"Board A", "Board B"});
    const QJsonObject preset{{"device", "STM32F407ZG"}, {"interface", "SWD"},
                             {"speed", "1000"}, {"transport", 0},
                             {"serial", "123456"}, {"remote", ""},
                             {"base", "0x08000000"}};
    settings.setValue("presets", QVariantMap{{"Board B", QJsonDocument(preset).toJson(QJsonDocument::Compact)}});
    {
      MainWindow w;
      const auto favorites = w.findChild<QComboBox *>("favoritePresets");
      const auto unlock = w.findChild<QCheckBox *>("unlock");
      QVERIFY(favorites && unlock);
      QVERIFY(unlock->isEnabled());
      QVERIFY(!unlock->isChecked());
      favorites->setCurrentIndex(favorites->findText("Board B"));
      QVERIFY(QMetaObject::invokeMethod(favorites, "activated", Qt::DirectConnection,
                                       Q_ARG(int, favorites->currentIndex())));
      unlock->setChecked(true);
      QCOMPARE(settings.value("lastFavorite").toString(), QString("Board B"));
      QVERIFY(settings.value("unlock").toBool());
    }
    {
      MainWindow reopened;
      const auto favorites = reopened.findChild<QComboBox *>("favoritePresets");
      const auto unlock = reopened.findChild<QCheckBox *>("unlock");
      QVERIFY(favorites && unlock);
      QCOMPARE(favorites->currentText(), QString("Board B"));
      QVERIFY(unlock->isEnabled());
      QVERIFY(unlock->isChecked());
      const auto device = reopened.findChild<QComboBox *>("selectedMcu");
      QVERIFY(device);
      QCOMPARE(device->currentText(), QString("STM32F407ZG"));
      reopened.show();
      const auto buttons = reopened.findChildren<QPushButton *>();
      const auto program = std::find_if(buttons.begin(), buttons.end(), [](auto button) {
        return button->text() == "Program  ·  F5";
      });
      QVERIFY(program != buttons.end());
      bool warned = false;
      QTimer::singleShot(30, [&] {
        for (auto widget : QApplication::topLevelWidgets())
          if (auto warning = qobject_cast<QMessageBox *>(widget); warning && warning->isVisible()) {
            warned = true;
            warning->done(QMessageBox::No);
          }
      });
      (*program)->click();
      QVERIFY(warned);
    }
    settings.setValue("lastFavorite", "Removed preset");
    settings.setValue("device", "UNSUPPORTED_MCU");
    settings.setValue("unlock", true);
    {
      MainWindow unsupported;
      const auto favorites = unsupported.findChild<QComboBox *>("favoritePresets");
      const auto unlock = unsupported.findChild<QCheckBox *>("unlock");
      QVERIFY(favorites && unlock);
      QCOMPARE(favorites->currentIndex(), 0);
      QVERIFY(!unlock->isEnabled());
      QVERIFY(!unlock->isChecked());
    }
    QVERIFY(!settings.value("unlock").toBool());
    settings.setValue("device", "STM32F407ZG");
    {
      MainWindow availableAgain;
      const auto unlock = availableAgain.findChild<QCheckBox *>("unlock");
      QVERIFY(unlock);
      QVERIFY(unlock->isEnabled());
      QVERIFY(!unlock->isChecked());
    }
    settings.clear();
  }
  void presetDeviceSearchMatchesSubstring() {
    QSettings settings;
    settings.clear();
    settings.setValue("commander", "/missing/JLinkExe");
    settings.setValue("flashCatalogCommander", "/missing/JLinkExe");
    settings.setValue("devices", QStringList{"STM32F103C8", "STM32F407ZG", "GD32F407VG", "NRF52840"});
    MainWindow w;
    w.show();
    const auto configure = w.findChild<QPushButton *>("configurePreset");
    QVERIFY(configure);
    bool matched = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets()) {
        auto dialog = qobject_cast<QDialog *>(widget);
        if (!dialog || !dialog->isVisible())
          continue;
        const auto combo = dialog->findChild<QComboBox *>("presetDevice");
        if (combo && combo->completer()) {
          combo->lineEdit()->setFocus();
          combo->lineEdit()->selectAll();
          QTest::keyClicks(combo->lineEdit(), "f407");
          QTest::qWait(30);
          const auto completer = combo->completer();
          const auto model = completer->completionModel();
          QStringList matches;
          for (int row = 0; row < model->rowCount(); ++row)
            matches << model->index(row, 0).data().toString();
          matched = completer->filterMode() == Qt::MatchContains &&
                    completer->caseSensitivity() == Qt::CaseInsensitive &&
                    matches.contains("STM32F407ZG") && matches.contains("GD32F407VG") &&
                    matches.size() == 2;
        }
        dialog->reject();
      }
    });
    configure->click();
    QVERIFY(matched);
    settings.clear();
  }
  void presetSearchAfterCatalogRefresh() {
    QSettings settings;
    settings.clear();
    auto fake = QCoreApplication::applicationDirPath() + "/fake_jlink";
#ifdef Q_OS_WIN
    fake += ".exe";
#endif
    settings.setValue("commander", fake);
    settings.setValue("flashCatalogCommander", fake);
    settings.setValue("devices", QStringList{"STM32F103C8"});
    MainWindow w;
    w.show();
    const auto configure = w.findChild<QPushButton *>("configurePreset");
    QVERIFY(configure);
    bool updated = false;
    QTimer::singleShot(30, [&] {
      for (auto widget : QApplication::topLevelWidgets()) {
        auto dialog = qobject_cast<QDialog *>(widget);
        if (!dialog || !dialog->isVisible())
          continue;
        const auto combo = dialog->findChild<QComboBox *>("presetDevice");
        const auto refresh = dialog->findChild<QPushButton *>("refreshPresetDevices");
        if (combo && refresh) {
          QEventLoop wait;
          connect(combo->model(), &QAbstractItemModel::rowsInserted, &wait, &QEventLoop::quit);
          QTimer::singleShot(3000, &wait, &QEventLoop::quit);
          refresh->click();
          wait.exec();
          combo->lineEdit()->setFocus();
          combo->lineEdit()->selectAll();
          QTest::keyClicks(combo->lineEdit(), "407");
          QTest::qWait(30);
          const auto results = combo->completer()->completionModel();
          updated = results->rowCount() == 1 &&
                    results->index(0, 0).data().toString() == "STM32F407VG";
        }
        dialog->reject();
      }
    });
    configure->click();
    QVERIFY(updated);
    settings.clear();
  }
};
QTEST_MAIN(UiTest)
#include "UiTest.moc"
