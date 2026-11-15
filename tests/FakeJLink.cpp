#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  auto args = app.arguments();
  QTextStream out(stdout);
  auto value = [&](const QString &key) {
    auto i = args.indexOf(key);
    return i >= 0 ? args.value(i + 1) : QString();
  };
  if (args.contains("-SetDeviceFamily")) {
    const auto outcome = qEnvironmentVariable("TAILOR_FAKE_UNLOCK");
    if (outcome == "connection") {
      out << "S/N: 123456789\nERROR: Failed to connect to target\n";
      return 1;
    }
    if (outcome == "error") {
      out << "Unlock not successful.\n";
      return 1;
    }
    out << (outcome == "fail" ? "No confirmation\n" : "Option bytes reset to factory settings.\n");
    return 0;
  }
  if (args.contains("-operation")) {
    if (value("-operation") == "ReadOptionBytes") {
      const auto protection = qEnvironmentVariable("TAILOR_FAKE_UNLOCK", "bb");
      out << "FMC_OBCTL0 value: 0x2fff" << protection << "ed\n";
    } else if (value("-operation") == "WriteOptionBytes") {
      if (value("-SetConfigVal") != "OptionName=FMC_OBCTL0" ||
          !args.contains("Value=0x2fffaaed"))
        return 2;
      out << "Option bytes programmed successfully\n";
    } else {
      out << "Option bytes reset successfully!\n";
    }
    return 0;
  }
  QFile f(value("-CommandFile"));
  if (!f.open(QIODevice::ReadOnly))
    return 2;
  const auto script = QString::fromLocal8Bit(f.readAll());
  if (qEnvironmentVariableIsSet("TAILOR_FAKE_ECHO_SCRIPT"))
    out << script;
  if (qEnvironmentVariable("TAILOR_FAKE_CONNECTION") == "probe") {
    out << "ERROR: Cannot connect to J-Link\n";
    return 1;
  }
  if (qEnvironmentVariable("TAILOR_FAKE_CONNECTION") == "target") {
    out << "S/N: 123456789\nERROR: Cannot connect to target\n";
    return 1;
  }
  if (value("-Device") == "SLOW") {
    QThread::sleep(10);
    return 0;
  }
  if (value("-Device") == "FAIL") {
    out << "ERROR: programming failed\n";
    return 0;
  }
  if (script.contains("ExpDevList")) {
    QRegularExpression rx("ExpDevList \\\"([^\\\"]+)\\\"");
    auto m = rx.match(script);
    QFile d(m.captured(1));
    if (!d.open(QIODevice::WriteOnly))
      return 2;
    d.write("\"Manufacturer\", \"Device\", \"Core\", {Flash areas}, {RAM areas}\r\n"
            "\"ST\", \"STM32F407VG\", \"Cortex-M4\", {0x08000000, 0x00100000}\r\n"
            "STM32F103C8\n");
    return 0;
  }
  out << "Connecting to J-Link via USB...O.K.\nS/N: 123456789\nCortex-M4 "
         "identified.\nConnected.\n";
  if (script.contains("mem32 0xE0042000")) {
    if (qEnvironmentVariable("TAILOR_FAKE_ID") == "fail") {
      out << "ERROR: memory read failed\n";
      return 1;
    }
    out << "E0042000 = 10076413\n";
  }
  if (value("-Device") == "PROGRESS") {
    out << "Unrelated output [099%]\nProgramming flash [0" << Qt::flush;
    QThread::msleep(20);
    out << "00%]" << Qt::flush;
    QThread::msleep(20);
    out << "\b\b\b\b\b050%]" << Qt::flush;
    QThread::msleep(200);
    out << "\b\b\b\b\b101%]" << Qt::flush;
    out << "\b\b\b\b\b100%] Done.\r\n";
    out << "Erasing flash     [000%]\rErasing flash     [100%] Done.\n";
    if (qEnvironmentVariable("TAILOR_FAKE_PROGRESS_FAIL") == "1")
      out << "ERROR: programming failed\n";
    return 0;
  }
  QRegularExpression rx("savebin \\\"([^\\\"]+)\\\" 0x[0-9a-f]+ 0x([0-9a-f]+)");
  auto matches = rx.globalMatch(script);
  while (matches.hasNext()) {
    auto m = matches.next();
    if (qEnvironmentVariableIsSet("TAILOR_FAKE_INCOMPLETE_READ") && m.captured(2) == "8")
      continue;
    QFile d(m.captured(1));
    if (!d.open(QIODevice::WriteOnly))
      return 2;
    d.write(QByteArray(m.captured(2).toInt(nullptr, 16), 'X'));
  }
  out << "O.K.\n";
  return 0;
}
