#include "JLink.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <stdexcept>
namespace {
QString quoted(const QString &s) {
  if (s.contains('"') || s.contains('\n') || s.contains('\r'))
    throw std::runtime_error("Invalid command argument");
  return '"' + QDir::toNativeSeparators(s) + '"';
}
void safeToken(const QString &s) {
  static const QRegularExpression rx("^[A-Za-z0-9_.,:/@+\\[\\]()-]+$");
  if (!rx.match(s).hasMatch())
    throw std::runtime_error("Invalid connection parameter");
}
bool probeConnected(const QString &s) {
  static const QRegularExpression rx(R"((S/N\s*:|Serial number\s*:|Connecting to J-Link[^\r\n]*O\.K\.))", QRegularExpression::CaseInsensitiveOption);
  return rx.match(s).hasMatch();
}
bool targetConnected(const QString &s) {
  static const QRegularExpression rx(R"((^|[\r\n])\s*(Connected\.|Found SW-DP|Found JTAG-DP))", QRegularExpression::CaseInsensitiveOption);
  return rx.match(s).hasMatch();
}
bool connectionFailed(const QString &s) {
  static const QRegularExpression rx(R"((cannot|could not|failed to|unable to)\s+connect|no (j-link|emulator|probe|target).*(found|connected)|target (not found|not connected))", QRegularExpression::CaseInsensitiveOption);
  return rx.match(s).hasMatch();
}
} // namespace
JLink::JLink(QObject *parent) : QObject(parent) {
  process.setProcessChannelMode(QProcess::MergedChannels);
  timer.setSingleShot(true);
  connect(&timer, &QTimer::timeout, this, [this] {
    canceled = true;
    emit log("Timeout; stopping CLI. Target state is unknown.");
    process.kill();
  });
  connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
    const auto chunk = QString::fromLocal8Bit(process.readAllStandardOutput());
    output += chunk;
    if (output.size() > 1024 * 1024)
      output.remove(0, output.size() - 1024 * 1024);
    for (const auto ch : chunk) {
      if (ch == '\b') {
        pendingCursor = qMax(qsizetype(0), pendingCursor - 1);
        continue;
      }
      if (ch == '\r') {
        pendingCursor = 0;
        continue;
      }
      if (ch != '\n') {
        if (pendingCursor < pending.size())
          pending[pendingCursor++] = ch;
        else {
          pending.append(ch);
          ++pendingCursor;
        }
        if (ch == ']')
          reportProgress();
        continue;
      }
      auto line = pending.trimmed();
      emit log(line);
      if (!progressPhase.isEmpty()) {
        progressPhase.clear();
        lastProgress = -1;
        emit progressChanged(-1);
      }
      if (line.startsWith("J-Link>")) {
        auto cmd = line.mid(7).trimmed().toLower();
        if (cmd.startsWith("loadfile"))
          emit stage("Write + verify");
        else if (cmd.startsWith("erase"))
          emit stage("Erase");
        else if (cmd.startsWith("savebin"))
          emit stage("Read memory");
        else if (cmd.startsWith("verifybin"))
          emit stage("Verify");
        else if (cmd == "g")
          emit stage("Run");
      }
      pending.clear();
      pendingCursor = 0;
    }
  });
  connect(&process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
              if (current.target || !current.unlockKind.isEmpty())
                emit observed(0, 0);
              complete(false, process.errorString());
            }
          });
  connect(
      &process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this](int code, QProcess::ExitStatus status) {
        timer.stop();
        if (!pending.isEmpty())
          emit log(pending);
        pending.clear();
        pendingCursor = 0;
        if (!active)
          return;
        const bool ok =
            !canceled && code == 0 && status == QProcess::NormalExit &&
            (!outputError(output) ||
             ((mode == "catalog" || mode == "probes") &&
              !output.contains("Unknown command", Qt::CaseInsensitive)));
        if (current.unlockKind == "device-id" && !canceled) {
          static const QRegularExpression rx(R"((?:^|[\r\n])\s*(?:0x)?E0042000\s*=\s*(?:0x)?([0-9a-f]{8})\b)",
                                             QRegularExpression::CaseInsensitiveOption);
          const auto match = rx.match(output);
          bool valid = false;
          const auto value = match.captured(1).toUInt(&valid, 16);
          const auto id = value & 0xfffu;
          if (ok && valid && id && id != 0xfffu) {
            emit deviceId(QString("0x%1").arg(id, 3, 16, QChar('0')).toUpper().replace("0X", "0x"));
            complete(true, "Completed");
          } else {
            emit deviceId({});
            if (connectionFailed(output)) {
              emit observed(probeConnected(output) ? 1 : 0, 0);
              complete(false, "Target connection failed while reading MCU ID.");
            } else {
              emit importantLog("MCU ID could not be read.");
              complete(true, "Completed; MCU ID unavailable.");
            }
          }
          return;
        }
        if (!ok && current.unlockKind == "stm" && !canceled &&
            status == QProcess::NormalExit && !connectionFailed(output)) {
          emit importantLog("Unlock failed or was not confirmed; attempting target connection before programming.");
          launch();
          return;
        }
        if (!ok) {
          if (current.target || !current.unlockKind.isEmpty()) {
            const bool probe = probeConnected(output);
            const bool target = targetConnected(output) && !connectionFailed(output);
            emit observed(probe ? 1 : 0, target ? 1 : 0);
          }
          complete(false, canceled
                              ? "Canceled / timed out. Target state is unknown."
                              : "CLI failed. See log.");
          return;
        }
        if (current.unlockKind == "stm") {
          const bool confirmed = output.contains("Option bytes reset to factory settings", Qt::CaseInsensitive) ||
              output.contains("Option bytes reset successfully", Qt::CaseInsensitive);
          emit importantLog(confirmed ? "Unlock completed successfully."
                                      : "Unlock was not confirmed; attempting target connection before programming.");
        }
        if (current.unlockKind == "gd-read") {
          static const QRegularExpression rx("FMC_OBCTL0 value:\\s*(0x[0-9a-fA-F]{1,8})");
          const auto match = rx.match(output);
          bool valid = false;
          const auto value = match.captured(1).toUInt(&valid, 16);
          if (!match.hasMatch() || !valid) {
            complete(false, "GD32 option bytes could not be read; programming stopped.");
            return;
          }
          const auto protection = (value >> 8) & 0xff;
          if (protection == 0xcc) {
            emit importantLog("GD32 unlock failed: irreversible level 2 protection.");
            complete(false, "GD32 has irreversible level 2 protection; programming stopped.");
            return;
          }
          if (protection != 0xaa) {
            auto write = current;
            write.name = "GD32 Unlock";
            write.unlockKind = "gd-write";
            const auto index = write.arguments.indexOf("ReadOptionBytes");
            write.arguments[index] = "WriteOptionBytes";
            write.arguments << "-SetConfigVal" << QString("Value=0x%1").arg((value & ~0xff00u) | 0xaa00u, 8, 16, QChar('0'));
            jobs.prepend(write);
          } else
            emit importantLog("GD32 read protection is already disabled.");
        } else if (current.unlockKind == "gd-write") {
          if (!output.contains("Option bytes programmed successfully", Qt::CaseInsensitive)) {
            emit importantLog("GD32 unlock was not confirmed.");
            complete(false, "GD32 unlock was not confirmed. Power-cycle target and inspect option bytes before retrying.");
            return;
          }
          emit importantLog("GD32 unlock completed; target power cycle required.");
          complete(false, "GD32 unlock completed. Power-cycle the target, then retry programming. Programming has not started.");
          return;
        }
        if (current.target) {
          const bool target = targetConnected(output) && !connectionFailed(output);
          if (!target) {
            emit observed(probeConnected(output) ? 1 : 0, 0);
            complete(false, "Target connection was not confirmed. See log.");
            return;
          }
          emit observed(1, 1);
        }
        if (mode == "catalog") {
          QFile f(resultPath);
          if (!f.open(QIODevice::ReadOnly)) {
            complete(false, "Device export was not created");
            return;
          }
          QStringList names;
          QVariantMap ranges;
          QSet<QString> ambiguous;
          for (auto l : QString::fromUtf8(f.readAll()).split('\n')) {
            const auto name = deviceName(l);
            if (!name.isEmpty()) {
              names << name;
              const auto range = flashRange(l);
              const auto key = name.toUpper();
              if (!range.isEmpty() && !ambiguous.contains(key)) {
                if (ranges.contains(key) && ranges.value(key).toMap() != range) {
                  ranges.remove(key);
                  ambiguous.insert(key);
                } else {
                  ranges.insert(key, range);
                }
              }
            }
          }
          names.removeDuplicates();
          names.sort(Qt::CaseInsensitive);
          emit flashRanges(ranges);
          emit devices(names);
        } else if (mode == "probes") {
          QStringList serials;
          static const QRegularExpression rx(
              "(?:Serial number|S/N|SerialNumber|Serial)\\s*[:=]\\s*(\\d+)",
              QRegularExpression::CaseInsensitiveOption);
          auto it = rx.globalMatch(output);
          while (it.hasNext())
            serials << it.next().captured(1);
          serials.removeDuplicates();
          emit probeList(serials);
        } else if (mode == "read") {
          Firmware result;
          for (qsizetype i = 0; i < pendingReads.size(); ++i) {
            QFile f(temp->filePath(QString("read%1.bin").arg(i)));
            if (!f.open(QIODevice::ReadOnly) || f.size() != pendingReads[i].length) {
              complete(false, "Incomplete read buffer");
              return;
            }
            const auto data = f.readAll();
            if (data.size() != pendingReads[i].length) {
              complete(false, "Incomplete read buffer");
              return;
            }
            if (!result.segments.isEmpty() && quint64(result.segments.last().address) + result.segments.last().data.size() == pendingReads[i].address)
              result.segments.last().data += data;
            else
              result.segments.append({pendingReads[i].address, data});
          }
          emit readDone(result);
        }
        if (jobs.isEmpty())
          complete(true, "Completed");
        else
          launch();
      });
}
void JLink::reportProgress() {
  static const QRegularExpression rx(
      R"(^\s*((?:Comparing|Erasing|Programming|Verifying|Reading)\s+(?:flash|memory))\s*\[(\d{1,3})%\]$)",
      QRegularExpression::CaseInsensitiveOption);
  const auto match = rx.match(pending.left(pendingCursor));
  if (!match.hasMatch())
    return;
  const auto percent = match.captured(2).toInt();
  if (percent > 100)
    return;
  const auto phase = match.captured(1).simplified();
  if (phase.compare(progressPhase, Qt::CaseInsensitive) != 0) {
    progressPhase = phase;
    lastProgress = -1;
    emit stage(phase);
  }
  if (percent != lastProgress) {
    lastProgress = percent;
    emit progressChanged(percent);
  }
}
JLink::~JLink() {
  process.kill();
  process.waitForFinished(2000);
}
QString JLink::deviceName(const QString &entry) {
  const auto value = entry.trimmed();
  static const QRegularExpression record(R"re(^"([^"]+)",\s*"([^"]+)")re");
  const auto match = record.match(value);
  if (!match.hasMatch())
    return value;
  if (match.captured(1) == "Manufacturer" && match.captured(2) == "Device")
    return {};
  return match.captured(2);
}
bool JLink::configurableFlashBanks(const QString &device) {
  static const QRegularExpression rx("^GD32F450[VZI]G(?:[A-Z0-9]*)$", QRegularExpression::CaseInsensitiveOption);
  return rx.match(device.trimmed()).hasMatch();
}
QVector<MemoryRange> JLink::flashSectors(const QString &device, const QString &flashLayout) {
  // ST RM0090 / RM0008; GigaDevice GD32F4xx User Manual, flash organization.
  const auto name = device.trimmed().toUpper();
  static const QRegularExpression gd450("^GD32F450[VZI]([EGIK])(?:[A-Z0-9]*)$");
  const auto gd = gd450.match(name);
  if (gd.hasMatch()) {
    const auto density = gd.captured(1);
    if (density == "G" && flashLayout != "single" && flashLayout != "dual") return {};
    const quint32 size = density == "E" ? 512u * 1024 : density == "G" ? 1024u * 1024
                       : density == "I" ? 2048u * 1024 : 3072u * 1024;
    const bool dual1m = density == "G" && flashLayout == "dual";
    QVector<MemoryRange> ranges;
    for (quint32 offset = 0; offset < size;) {
      const quint32 bankStart = dual1m ? (offset < 512u * 1024 ? 0 : 512u * 1024)
                                       : (offset < 1024u * 1024 ? 0 : 1024u * 1024);
      const auto within = offset - bankStart;
      const quint32 length = within < 64u * 1024 ? 16u * 1024
                           : within < 128u * 1024 ? 64u * 1024
                           : within < 1024u * 1024 ? 128u * 1024 : 256u * 1024;
      const int bank = bankStart ? 1 : 0;
      const int sector = bank && dual1m ? int(ranges.size()) + 4 : int(ranges.size());
      ranges.append({0x08000000u + offset, length, sector, bank});
      offset += length;
    }
    return ranges;
  }
  static const QRegularExpression f4("^(?:STM32F40[57]|GD32F40[57])[A-Z]([EG])(?:[A-Z0-9]*)$");
  static const QRegularExpression f1("^STM32F10[123][A-Z]([8BCDE])(?:[A-Z0-9]*)$");
  const auto m4 = f4.match(name);
  const auto m1 = f1.match(name);
  QVector<MemoryRange> result;
  quint32 size = 0;
  if (m4.hasMatch())
    size = m4.captured(1) == "E" ? 512 * 1024 : 1024 * 1024;
  else if (m1.hasMatch()) {
    const auto density = m1.captured(1);
    size = density == "8" ? 64 * 1024 : density == "B" ? 128 * 1024
         : density == "C" ? 256 * 1024 : density == "D" ? 384 * 1024 : 512 * 1024;
  } else return {};
  for (quint32 offset = 0; offset < size;) {
    const quint32 length = m4.hasMatch() ? (offset < 64 * 1024 ? 16 * 1024 : offset < 128 * 1024 ? 64 * 1024 : 128 * 1024)
                                       : (size <= 128 * 1024 ? 1024 : 2048);
    result.append({0x08000000u + offset, length});
    offset += length;
  }
  return result;
}
QVariantMap JLink::flashRange(const QString &entry) {
  static const QRegularExpression prefix(R"re(^\s*"[^"]+",\s*"[^"]+",\s*"[^"]+",\s*)re");
  static const QRegularExpression pair(R"re(\{\s*(0[xX][0-9a-fA-F]+)\s*,\s*(0[xX][0-9a-fA-F]+)\s*\})re");
  const auto head = prefix.match(entry);
  if (!head.hasMatch())
    return {};
  const auto tail = entry.mid(head.capturedEnd()).trimmed();
  if (!tail.startsWith('{'))
    return {};
  int depth = 0, end = -1;
  for (int i = 0; i < tail.size(); ++i) {
    if (tail[i] == '{')
      ++depth;
    else if (tail[i] == '}' && --depth == 0) {
      end = i + 1;
      break;
    }
  }
  if (end < 0)
    return {};
  auto parts = pair.globalMatch(tail.left(end));
  quint64 start = 0, length = 0;
  while (parts.hasNext()) {
    const auto part = parts.next();
    bool addressOk = false, sizeOk = false;
    const auto address = part.captured(1).toULongLong(&addressOk, 16);
    const auto size = part.captured(2).toULongLong(&sizeOk, 16);
    if (!addressOk || !sizeOk || !size || address > 0xffffffffULL ||
        size > 0x100000000ULL || address + size > 0x100000000ULL ||
        (length && address != start + length))
      return {};
    if (!length)
      start = address;
    length += size;
    if (length > 256ULL * 1024 * 1024)
      return {};
  }
  if (!length)
    return {};
  return {{"address", static_cast<quint32>(start)}, {"length", static_cast<quint32>(length)}};
}
QString JLink::discover() {
  QStringList dirs;
#ifdef Q_OS_WIN
  for (const auto &root : {qEnvironmentVariable("ProgramFiles"),
                           qEnvironmentVariable("ProgramFiles(x86)")}) {
    QDir d(root + "/SEGGER");
    for (const auto &n :
         d.entryList({"JLink*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
      dirs << d.filePath(n);
  }
#elif defined(Q_OS_MACOS)
  dirs << "/Applications/SEGGER/JLink";
  QDir d("/Applications/SEGGER");
  for (const auto &n :
       d.entryList({"JLink*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
    dirs << d.filePath(n);
#else
  dirs << "/opt/SEGGER/JLink" << "/usr/local/SEGGER/JLink";
#endif
  for (const auto &name : {QString("JLinkExe"), QString("JLink.exe")}) {
    const auto p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty())
      return p;
    for (const auto &d : dirs) {
      const auto path = d + '/' + name;
      if (QFileInfo(path).isExecutable())
        return path;
    }
  }
  return {};
}
QString JLink::unlockExecutable(const QString &commander) {
  for (const auto &n : {"JLinkSTM32Exe", "JLinkSTM32.exe"}) {
    const auto canonical = QFileInfo(commander).canonicalFilePath();
    auto p = QFileInfo(canonical.isEmpty() ? commander : canonical)
                 .dir()
                 .filePath(n);
    if (QFileInfo(p).isExecutable())
      return p;
  }
  return {};
}
QString JLink::unlockMethod(const QString &commander, const QString &device) {
  const auto name = device.trimmed().toUpper();
  const auto canonical = QFileInfo(commander).canonicalFilePath();
  const auto dir = QFileInfo(canonical.isEmpty() ? commander : canonical).dir();
  QString devpro;
  for (const auto &candidate : {"DevPro", "DevPro.exe"})
    if (QFileInfo(dir.filePath(candidate)).isExecutable()) {
      devpro = dir.filePath(candidate);
      break;
    }
  static const QRegularExpression stm("^STM32(F[01234]|G4|L[0145]|WB|WL)[A-Z0-9]+$");
  const auto match = stm.match(name);
  if (match.hasMatch()) {
    const auto family = "STM32" + match.captured(1);
    const auto script = dir.filePath("Script/PCode_DevPro_ST_" + family + ".pex");
    if (!devpro.isEmpty() && QFileInfo::exists(script))
      return "devpro-stm:" + script;
    if (!unlockExecutable(commander).isEmpty())
      return "legacy:" + family + "xxxx";
  }
  static const QRegularExpression gd("^GD32(?:F4(?!03)|A4)[A-Z0-9]+$");
  const auto script = dir.filePath("Script/PCode_DevPro_GigaDevice_GD32A4_GD32F4.pex");
  if (gd.match(name).hasMatch() && !devpro.isEmpty() && QFileInfo::exists(script))
    return "devpro-gd:" + script;
  return {};
}
QStringList JLink::arguments(const LinkConfig &c) {
  safeToken(c.device);
  safeToken(c.interface);
  safeToken(c.speed);
  QStringList a = {"-NoGui", "1",   "-ExitOnError", "1",      "-Device",
                   c.device, "-If", c.interface,    "-Speed", c.speed};
  if (!c.remote.isEmpty()) {
    safeToken(c.remote);
    a << "-IP" << c.remote;
  }
  // -SelectEmuBySN also works with older Commander releases, unlike -USB.
  else if (!c.serial.isEmpty()) {
    safeToken(c.serial);
    a << "-SelectEmuBySN" << c.serial;
  }
  return a;
}
bool JLink::outputError(const QString &s) {
  static const QRegularExpression rx(
      "(^|[\\r\\n])[^\\r\\n]*(\\bERROR\\b|\\bFAILED\\b|Cannot connect|Unknown "
      "command|Syntax error|Verification failed|does not match|Could not|not "
      "supported|Invalid parameter|not successful|unsuccessful)",
      QRegularExpression::CaseInsensitiveOption);
  for (const auto &line : s.split('\n')) {
    if (line.contains("J-Link Commander will now exit on Error",
                      Qt::CaseInsensitive))
      continue;
    if (rx.match(line).hasMatch())
      return true;
  }
  return false;
}
QString JLink::save(const QString &name, const QByteArray &data) {
  const auto path = temp->filePath(name);
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
    throw std::runtime_error("Cannot write temporary file");
  return path;
}
void JLink::execute(const LinkConfig &c, Operation op, const Firmware &fw,
                    quint32 addr, quint32 length, const QVector<MemoryRange> &readRanges) {
  if (active)
    throw std::runtime_error("CLI is busy");
  auto args = arguments(c);
  if (!QFileInfo(c.executable).isExecutable())
    throw std::runtime_error("Select installed J-Link Commander in Settings");
  temp = std::make_unique<QTemporaryDir>();
  if (!temp->isValid())
    throw std::runtime_error("Cannot create temporary directory");
  jobs.clear();
  mode = "operation";
  timeout = c.timeoutMs;
  auto add = [&](QString name, QStringList lines) {
    jobs.append({name, c.executable, args,
                 "ExitOnError 1\nconnect\n" + lines.join('\n') + "\nexit\n",
                 true, {}});
  };
  if ((op == Operation::Write || op == Operation::Program ||
       op == Operation::Verify) &&
      fw.segments.isEmpty())
    throw std::runtime_error("Open firmware first");
  if (op == Operation::Program && c.unlock) {
    const auto method = unlockMethod(c.executable, c.device);
    if (method.isEmpty() || c.interface.compare("SWD", Qt::CaseInsensitive) != 0)
      throw std::runtime_error("No supported unlock method for this device and installed J-Link; SWD is required");
    QStringList ua;
    QString tool;
    QString kind = "stm";
    if (method.startsWith("legacy:")) {
      tool = unlockExecutable(c.executable);
      ua = {"-SetDeviceFamily", method.mid(7), "-Speed", c.speed, "-Exit", "1"};
    } else {
      const auto canonical = QFileInfo(c.executable).canonicalFilePath();
      const auto dir = QFileInfo(canonical.isEmpty() ? c.executable : canonical).dir();
      tool = dir.filePath(QFileInfo(dir.filePath("DevPro")).isExecutable() ? "DevPro" : "DevPro.exe");
      const bool gd = method.startsWith("devpro-gd:");
      kind = gd ? "gd-read" : "stm";
      ua = {"-operation", gd ? "ReadOptionBytes" : "PerformUnlock", "-if", "SWD", "-speed", c.speed,
            "-ScriptFile", method.mid(method.indexOf(':') + 1)};
      if (gd)
        ua << "-SetConfigVal" << "OptionName=FMC_OBCTL0";
    }
    if (!c.remote.isEmpty())
      ua << "-IP" << c.remote;
    else if (!c.serial.isEmpty())
      ua << "-USB" << c.serial;
    jobs.append({"Unlock", tool, ua, {}, false, kind});
  }
  QStringList commands;
  if (op == Operation::Read) {
    pendingReads = readRanges.isEmpty() ? QVector<MemoryRange>{{addr, length}} : readRanges;
    std::sort(pendingReads.begin(), pendingReads.end(), [](const auto &a, const auto &b) { return a.address < b.address; });
    quint64 total = 0, previousEnd = 0;
    for (const auto &range : pendingReads) {
      const auto end = quint64(range.address) + range.length;
      total += range.length;
      if (!range.length || end > 0x100000000ULL || range.address < previousEnd || total > 256ULL * 1024 * 1024)
        throw std::runtime_error("Invalid or overlapping read ranges (max 256 MiB)");
      previousEnd = end;
    }
    mode = "read";
    commands << "halt";
    for (qsizetype i = 0; i < pendingReads.size(); ++i)
      commands << QString("savebin %1 0x%2 0x%3")
                      .arg(quoted(temp->filePath(QString("read%1.bin").arg(i))))
                      .arg(pendingReads[i].address, 0, 16)
                      .arg(pendingReads[i].length, 0, 16);
  }
  if (op == Operation::Erase || (op == Operation::Program && c.fullErase))
    commands << "erase";
  if (op == Operation::Write || op == Operation::Program) {
    const auto file = save("firmware.hex", fw.toHex());
    commands << "loadfile " + quoted(file);
  }
  if (op == Operation::Verify) {
    int i = 0;
    for (const auto &s : fw.segments) {
      const auto file = save(QString("verify%1.bin").arg(i++), s.data);
      commands << QString("verifybin %1 0x%2")
                      .arg(quoted(file))
                      .arg(s.address, 0, 16);
    }
  }
  if (op == Operation::Run || (op == Operation::Program && c.runAfter))
    commands << "r" << "g";
  if (op == Operation::Halt)
    commands << "exec SetRestartOnClose = 0" << "halt";
  if (op == Operation::Resume)
    commands << "g";
  if (op == Operation::Reset)
    commands << "r";
  if (op == Operation::Connect)
    commands << "ShowHWStatus";
  // LoadFile performs SEGGER's flash-loader verification before the following
  // reset/go.
  add(op == Operation::Program ? "Connect → erase → write → verify → run"
                               : "J-Link",
      commands);
  if (op == Operation::Connect) {
    emit deviceId({});
    static const QRegularExpression stmId("^STM32F[14][A-Z0-9]+$", QRegularExpression::CaseInsensitiveOption);
    if (stmId.match(c.device.trimmed()).hasMatch())
      jobs.append({"Read MCU ID", c.executable, args,
                   "ExitOnError 1\nconnect\nmem32 0xE0042000 1\nexit\n", false, "device-id"});
  }
  active = true;
  canceled = false;
  launch();
}
void JLink::catalog(const QString &exe) {
  if (active)
    return;
  temp = std::make_unique<QTemporaryDir>();
  resultPath = temp->filePath("devices.txt");
  mode = "catalog";
  jobs = {{"Loading devices",
           exe,
           {"-NoGui", "1"},
           "ExpDevList " + quoted(resultPath) + "\nexit\n",
           false, {}}};
  active = true;
  canceled = false;
  launch();
}
void JLink::probes(const QString &exe) {
  if (active)
    return;
  temp = std::make_unique<QTemporaryDir>();
  mode = "probes";
  jobs = {
      {"USB probes", exe, {"-NoGui", "1"}, "ShowEmuList USB\nexit\n", false, {}}};
  active = true;
  canceled = false;
  launch();
}
void JLink::launch() {
  try {
    current = jobs.takeFirst();
    output.clear();
    pending.clear();
    pendingCursor = 0;
    progressPhase.clear();
    lastProgress = -1;
    auto a = current.arguments;
    if (!current.script.isEmpty()) {
      auto path = save("commands.jlink", current.script.toLocal8Bit());
      a << "-CommandFile" << path;
      emit log(current.script.trimmed());
    }
    emit stage(current.name);
    process.start(current.program, a);
    timer.start(timeout);
  } catch (const std::exception &e) {
    complete(false, QString::fromUtf8(e.what()));
  }
}
void JLink::complete(bool ok, const QString &message) {
  if (!active)
    return;
  active = false;
  timer.stop();
  jobs.clear();
  emit finished(ok, message);
}
void JLink::cancel() {
  if (active) {
    canceled = true;
    process.kill();
  }
}
