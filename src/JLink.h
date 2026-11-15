#pragma once
#include "Firmware.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>
#include <memory>
struct LinkConfig {
  QString executable, device, interface = "SWD", speed = "4000", serial, remote;
  bool fullErase = false, runAfter = true, unlock = false;
  int timeoutMs = 120000;
};
struct MemoryRange { quint32 address{}, length{}; int sector = -1, bank = -1; };
enum class Operation {
  Connect,
  Read,
  Write,
  Verify,
  Erase,
  Program,
  Run,
  Halt,
  Resume,
  Reset
};
struct LinkJob {
  QString name, program;
  QStringList arguments;
  QString script;
  bool target = true;
  QString unlockKind;
};
class JLink : public QObject {
  Q_OBJECT
public:
  explicit JLink(QObject *parent = nullptr);
  ~JLink() override;
  static QString discover();
  static QString deviceName(const QString &entry);
  static QVariantMap flashRange(const QString &entry);
  static bool configurableFlashBanks(const QString &device);
  static QVector<MemoryRange> flashSectors(const QString &device, const QString &flashLayout = {});
  static QString unlockExecutable(const QString &commander);
  static QString unlockMethod(const QString &commander, const QString &device);
  static QStringList arguments(const LinkConfig &config);
  static bool outputError(const QString &output);
  bool busy() const { return active; }
  void execute(const LinkConfig &, Operation, const Firmware &,
               quint32 readAddress = 0, quint32 readSize = 0,
               const QVector<MemoryRange> &readRanges = {});
  void catalog(const QString &executable);
  void probes(const QString &executable);
  void cancel();
signals:
  void log(const QString &);
  void importantLog(const QString &);
  void stage(const QString &);
  // -1 means the current phase has no reported percentage.
  void progressChanged(int percent);
  void finished(bool success, const QString &message);
  void devices(const QStringList &);
  void flashRanges(const QVariantMap &);
  void probeList(const QStringList &);
  void readDone(const Firmware &);
  void deviceId(const QString &);
  // -1: not checked, 0: connection failed, 1: connected.
  void observed(int probe, int target);

private:
  QProcess process;
  QTimer timer;
  std::unique_ptr<QTemporaryDir> temp;
  QList<LinkJob> jobs;
  LinkJob current;
  QString output, pending, mode, resultPath;
  QString progressPhase;
  int lastProgress = -1;
  qsizetype pendingCursor{};
  QVector<MemoryRange> pendingReads;
  bool active = false, canceled = false;
  int timeout = 120000;
  void launch();
  void reportProgress();
  void complete(bool, const QString &);
  QString save(const QString &, const QByteArray &);
};
