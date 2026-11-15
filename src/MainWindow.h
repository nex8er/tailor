#pragma once
#include "JLink.h"
#include <QMainWindow>
#include <QSettings>
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QTableView;
class QTabWidget;
class QPushButton;
class HexModel;
class MainWindow : public QMainWindow {
public:
  MainWindow();
  ~MainWindow() override;
  void openFile(const QString &);
  void openComparison(const QString &);
  bool saveBufferTo(const QString &);

protected:
  void closeEvent(QCloseEvent *) override;
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;

private:
  QSettings settings;
  JLink link;
  Firmware firmware, comparison;
  QString filePath, comparisonPath;
  QString flashLayout, flashLayoutDevice;
  bool russian;
  bool readRangeEdited = false;
  bool bufferModified = false;
  QVariantMap flashRanges;
  QComboBox *device{}, *transport{}, *serial{}, *interface{}, *speed{},
      *favorites{};
  QLineEdit *remote{}, *base{}, *readAddress{}, *readSize{}, *search{};
  QCheckBox *unlock{};
  QLabel *fileLabel{}, *summary{}, *probeStatus{}, *targetStatus{},
      *probeDot{}, *targetDot{}, *stageLabel{}, *comparisonLabel{}, *mcuId{}, *memoryTitle{};
  QPlainTextEdit *logView{};
  QProgressBar *progress{};
  QTableView *memoryView{}, *comparisonView{};
  HexModel *memoryModel{}, *comparisonModel{};
  QTabWidget *tabs{};
  QPushButton *cancelButton{};
  QList<QWidget *> busyControls;
  QString t(const char *en, const char *ru) const;
  LinkConfig config() const;
  void perform(Operation);
  void presetDialog();
  void applicationSettingsDialog();
  void refreshDevices();
  void updateUnlockAvailability();
  void appendLog(const QString &, bool important = false);
  void saveConfig();
  void updateSummary();
  void updateReadRange();
  bool saveBuffer();
  bool confirmDiscardChanges();
  bool commitBufferEdit();
  void setBufferModified(bool);
  void applyFavorite();
  enum class HexSearch { Address, Bytes, Ascii };
  void findHex(HexSearch);
  void openComparisonFile();
  void updateHexPage();
  void updateSectorFilter();
  QString sectorLabel(const MemoryRange &, int index) const;
  void readSectors();
  void setStatus(int probe, int target);
  void fail(const QString &);
};
