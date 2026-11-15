#include "Firmware.h"
#include "JLink.h"
#include <QtTest>
class CoreTest : public QObject {
  Q_OBJECT
private slots:
  void checksums() {
    const Firmware empty;
    QCOMPARE(empty.checksum(ChecksumAlgorithm::Crc32Ethernet), QString("00000000"));
    const Firmware segmented{{{0x08000000, "1234"}, {0x08001000, "56789"}}};
    QCOMPARE(segmented.checksum(ChecksumAlgorithm::Md5), QString("25f9e794323b453885f5181f1b624d0b"));
    QCOMPARE(segmented.checksum(ChecksumAlgorithm::Crc32Ethernet), QString("cbf43926"));
    QCOMPARE(segmented.checksum(ChecksumAlgorithm::Sha256), QString("15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225"));
    const Firmware bytes{{{0, QByteArray::fromHex("00ff80")}}};
    QCOMPARE(bytes.checksum(ChecksumAlgorithm::Md5), QString("4fd64cb80c3f86e1e9d0f559af8b370f"));
    QCOMPARE(bytes.checksum(ChecksumAlgorithm::Crc32Ethernet), QString("81dda740"));
  }
  void roundtrip() {
    Firmware f{
        {{0x0800fff8, QByteArray::fromHex("01020304050607080910111213141516")},
         {0x90000000, "abc"}}};
    auto out = Firmware::parseHex(f.toHex());
    QCOMPARE(out.size(), f.size());
    QCOMPARE(out.segments.size(), 2);
    QCOMPARE(out.segments[0].address, f.segments[0].address);
    QCOMPARE(out.segments[0].data, f.segments[0].data);
    QCOMPARE(out.segments[1].address, 0x90000000u);
  }
  void invalid() {
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error, Firmware::parseHex(":0100000000FE\n:00000001FF\n"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             Firmware::parseHex(":0100000000FF\n"));
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        Firmware::parseHex(":0100000000FF\n:0100000000FF\n:00000001FF\n"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, addressValue("0x100000000"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, addressValue("-1"));
  }
  void args() {
    LinkConfig c;
    c.device = "STM32F407VG";
    c.remote = "localhost:19020";
    auto a = JLink::arguments(c);
    QVERIFY(a.contains("-IP"));
    QVERIFY(a.contains("localhost:19020"));
    c.remote = "host\nErase";
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, JLink::arguments(c));
  }
  void lengths() {
    QCOMPARE(lengthValue("010"), 10u);
    QCOMPARE(lengthValue("0x10"), 16u);
    QCOMPARE(lengthValue("64k"), 65536u);
    QCOMPARE(lengthValue("64 K"), 65536u);
    QCOMPARE(lengthValue("1m"), 1048576u);
    QCOMPARE(lengthValue("1M"), 1048576u);
    QCOMPARE(lengthValue("0x10K"), 16384u);
    QCOMPARE(lengthValue("0X10m"), 16777216u);
    QCOMPARE(lengthValue("0xffffffff"), 0xffffffffu);
    for (const auto value : {"", "-1", "1.5K", "0x", "1KB", "4294967296", "4096M", "0x100000000K"})
      QVERIFY_THROWS_EXCEPTION(std::runtime_error, lengthValue(value));
  }
  void errors() {
    QVERIFY(JLink::outputError("\nERROR: erase failed"));
    QVERIFY(JLink::outputError("FAILED: Cannot connect"));
    QVERIFY(!JLink::outputError("ExitOnError 1\nO.K.\n"));
    QVERIFY(!JLink::outputError("J-Link Commander will now exit on Error\nO.K.\n"));
    QVERIFY(JLink::outputError("J-Link Commander will now exit on Error\nERROR: failed\n"));
  }
  void automaticUnlockSelection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
#ifdef Q_OS_WIN
    const auto commander = dir.filePath("JLink.exe");
    const auto legacy = dir.filePath("JLinkSTM32.exe");
    const auto devproPath = dir.filePath("DevPro.exe");
#else
    const auto commander = dir.filePath("JLinkExe");
    const auto legacy = dir.filePath("JLinkSTM32Exe");
    const auto devproPath = dir.filePath("DevPro");
#endif
    QFile utility(legacy);
    QVERIFY(utility.open(QIODevice::WriteOnly));
    utility.close();
    QVERIFY(utility.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QCOMPARE(JLink::unlockMethod(commander, "STM32F407ZG"), QString("legacy:STM32F4xxxx"));
    QVERIFY(JLink::unlockMethod(commander, "GD32F407VG").isEmpty());
    QVERIFY(JLink::unlockMethod(commander, "STM32F7XX").isEmpty());
    QVERIFY(QDir(dir.path()).mkdir("Script"));
    QFile devpro(devproPath);
    QVERIFY(devpro.open(QIODevice::WriteOnly));
    devpro.close();
    QVERIFY(devpro.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QFile script(dir.filePath("Script/PCode_DevPro_GigaDevice_GD32A4_GD32F4.pex"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.close();
    QVERIFY(JLink::unlockMethod(commander, "GD32F407VG").startsWith("devpro-gd:"));
    QVERIFY(JLink::unlockMethod(commander, "GD32F403VG").isEmpty());
  }
  void unlockSequence() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto fake = QString::fromUtf8(FAKE_JLINK_PATH);
#ifdef Q_OS_WIN
    const auto commander = dir.filePath("JLink.exe");
    const auto legacy = dir.filePath("JLinkSTM32.exe");
    const auto devpro = dir.filePath("DevPro.exe");
#else
    const auto commander = dir.filePath("JLinkExe");
    const auto legacy = dir.filePath("JLinkSTM32Exe");
    const auto devpro = dir.filePath("DevPro");
#endif
    QVERIFY(QFile::copy(fake, commander));
    QVERIFY(QFile::copy(fake, legacy));
    QVERIFY(QFile::copy(fake, devpro));
    QVERIFY(QDir(dir.path()).mkdir("Script"));
    QFile script(dir.filePath("Script/PCode_DevPro_GigaDevice_GD32A4_GD32F4.pex"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.close();
    LinkConfig c;
    c.executable = commander;
    c.unlock = true;
    c.device = "STM32F407ZG";
    Firmware fw{{{0x08000000, "test"}}};
    JLink link;
    QSignalSpy done(&link, &JLink::finished), stages(&link, &JLink::stage),
        important(&link, &JLink::importantLog), observed(&link, &JLink::observed);
    qputenv("TAILOR_FAKE_UNLOCK", "fail");
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(stages.size(), 2);
    QVERIFY(important.takeFirst().at(0).toString().contains("not confirmed"));
    qputenv("TAILOR_FAKE_CONNECTION", "target");
    stages.clear();
    observed.clear();
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(!done.takeFirst().at(0).toBool());
    QCOMPARE(stages.size(), 2);
    QCOMPARE(observed.last().at(0).toInt(), 1);
    QCOMPARE(observed.last().at(1).toInt(), 0);
    important.clear();
    qunsetenv("TAILOR_FAKE_CONNECTION");
    qputenv("TAILOR_FAKE_UNLOCK", "error");
    stages.clear();
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(stages.size(), 2);
    QVERIFY(important.takeFirst().at(0).toString().contains("failed"));
    qputenv("TAILOR_FAKE_UNLOCK", "connection");
    stages.clear();
    observed.clear();
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(!done.takeFirst().at(0).toBool());
    QCOMPARE(stages.size(), 1);
    QCOMPARE(observed.last().at(0).toInt(), 1);
    QCOMPARE(observed.last().at(1).toInt(), 0);
    qunsetenv("TAILOR_FAKE_UNLOCK");
    stages.clear();
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(stages.size(), 2);
    QVERIFY(important.takeFirst().at(0).toString().contains("successfully"));
    qputenv("TAILOR_FAKE_UNLOCK", "bb");
    stages.clear();
    c.device = "GD32F407VG";
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    const auto result = done.takeFirst();
    QVERIFY(!result.at(0).toBool());
    QVERIFY(result.at(1).toString().contains("Power-cycle"));
    QCOMPARE(stages.size(), 2);
    qputenv("TAILOR_FAKE_UNLOCK", "cc");
    stages.clear();
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(1).toString().contains("irreversible"));
    QCOMPARE(stages.size(), 1);
    qputenv("TAILOR_FAKE_UNLOCK", "aa");
    stages.clear();
    link.execute(c, Operation::Program, fw);
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(stages.size(), 2);
    qunsetenv("TAILOR_FAKE_UNLOCK");
  }
  void cliRead() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "STM32F407VG";
    QSignalSpy done(&link, &JLink::finished), read(&link, &JLink::readDone);
    Firmware result;
    connect(&link, &JLink::readDone, &link,
            [&](const Firmware &f) { result = f; });
    link.execute(c, Operation::Read, {}, 0x08000000, 32);
    QVERIFY(done.wait());
    QCOMPARE(done.first().first().toBool(), true);
    QCOMPARE(read.size(), 1);
    QCOMPARE(result.size(), 32);
    QCOMPARE(result.segments.first().address, 0x08000000u);
  }
  void sectorLayouts() {
    for (const auto name : {"STM32F407ZG", "GD32F407VG"}) {
      const auto sectors = JLink::flashSectors(name);
      QCOMPARE(sectors.size(), 12);
      QCOMPARE(sectors[0].address, 0x08000000u);
      QCOMPARE(sectors[3].length, 16u * 1024);
      QCOMPARE(sectors[4].length, 64u * 1024);
      QCOMPARE(sectors[5].length, 128u * 1024);
      QCOMPARE(sectors.last().address + sectors.last().length, 0x08100000u);
    }
    QCOMPARE(JLink::flashSectors("STM32F103C8").size(), 64);
    QCOMPARE(JLink::flashSectors("STM32F103RE").first().length, 2048u);
    QVERIFY(JLink::flashSectors("STM32F429ZI").isEmpty());
    QVERIFY(JLink::flashSectors("GD32F103C8").isEmpty());
  }
  void gd450SectorLayouts() {
    QVERIFY(JLink::configurableFlashBanks("gd32f450zgt6"));
    QVERIFY(!JLink::configurableFlashBanks("GD32F450ZK"));
    QVERIFY(JLink::flashSectors("GD32F450ZG").isEmpty());
    QVERIFY(JLink::flashSectors("GD32F450ZG", "invalid").isEmpty());
    const auto small = JLink::flashSectors("GD32F450ZE");
    QCOMPARE(small.size(), 8);
    QCOMPARE(small.last().address + small.last().length, 0x08080000u);
    const auto single = JLink::flashSectors("GD32F450ZG", "single");
    QCOMPARE(single.size(), 12);
    QCOMPARE(single[8].address, 0x08080000u);
    QCOMPARE(single[8].length, 128u * 1024);
    QCOMPARE(single[8].sector, 8);
    QCOMPARE(single[8].bank, 0);
    const auto dual = JLink::flashSectors("GD32F450ZGT6", "dual");
    QCOMPARE(dual.size(), 16);
    QCOMPARE(dual[8].address, 0x08080000u);
    QCOMPARE(dual[8].length, 16u * 1024);
    QCOMPARE(dual[8].sector, 12);
    QCOMPARE(dual[8].bank, 1);
    QCOMPARE(dual.last().sector, 19);
    QCOMPARE(dual.last().address + dual.last().length, 0x08100000u);
    const auto two = JLink::flashSectors("GD32F450ZI");
    QCOMPARE(two.size(), 24);
    QCOMPARE(two[12].address, 0x08100000u);
    QCOMPARE(two[12].length, 16u * 1024);
    QCOMPARE(two[12].sector, 12);
    QCOMPARE(two.last().address + two.last().length, 0x08200000u);
    const auto three = JLink::flashSectors("GD32F450ZK");
    QCOMPARE(three.size(), 28);
    QCOMPARE(three[24].address, 0x08200000u);
    QCOMPARE(three[24].length, 256u * 1024);
    QCOMPARE(three.last().sector, 27);
    QCOMPARE(three.last().address + three.last().length, 0x08300000u);
  }
  void readMultipleRanges() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "STM32F407VG";
    QSignalSpy done(&link, &JLink::finished);
    Firmware result;
    connect(&link, &JLink::readDone, &link, [&](const Firmware &f) { result = f; });
    link.execute(c, Operation::Read, {}, 0, 0, {{0x08000020, 8}, {0x08000000, 16}, {0x08000010, 16}});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(result.segments.size(), 1);
    QCOMPARE(result.segments.first().data.size(), 40);
    link.execute(c, Operation::Read, {}, 0, 0, {{0x08000000, 16}, {0x08000020, 8}});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(result.segments.size(), 2);
    QCOMPARE(result.segments.last().address, 0x08000020u);
    QSignalSpy read(&link, &JLink::readDone);
    qputenv("TAILOR_FAKE_INCOMPLETE_READ", "1");
    link.execute(c, Operation::Read, {}, 0, 0, {{0x08000000, 16}, {0x08000020, 8}});
    QVERIFY(done.wait());
    QVERIFY(!done.takeFirst().at(0).toBool());
    QVERIFY(read.isEmpty());
    qunsetenv("TAILOR_FAKE_INCOMPLETE_READ");
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
        link.execute(c, Operation::Read, {}, 0, 0, {{0x08000000, 16}, {0x08000008, 8}}));
  }
  void haltAndResume() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "STM32F407VG";
    QSignalSpy done(&link, &JLink::finished), logs(&link, &JLink::log);
    qputenv("TAILOR_FAKE_ECHO_SCRIPT", "1");
    link.execute(c, Operation::Halt, {});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QStringList lines;
    for (const auto &entry : logs)
      lines << entry.first().toString();
    QVERIFY(lines.contains("exec SetRestartOnClose = 0"));
    QVERIFY(lines.contains("halt"));
    QVERIFY(!lines.contains("r"));
    QVERIFY(!lines.contains("g"));
    logs.clear();
    link.execute(c, Operation::Resume, {});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    lines.clear();
    for (const auto &entry : logs)
      lines << entry.first().toString();
    QVERIFY(lines.contains("g"));
    QVERIFY(!lines.contains("r"));
    QVERIFY(!lines.contains("halt"));
    qunsetenv("TAILOR_FAKE_ECHO_SCRIPT");
  }
  void deviceIdentifier() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "STM32F407ZG";
    QSignalSpy done(&link, &JLink::finished), ids(&link, &JLink::deviceId);
    link.execute(c, Operation::Connect, {});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(ids.last().at(0).toString(), QString("0x413"));
    qputenv("TAILOR_FAKE_ID", "fail");
    link.execute(c, Operation::Connect, {});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QVERIFY(ids.last().at(0).toString().isEmpty());
    qunsetenv("TAILOR_FAKE_ID");
    ids.clear();
    c.device = "GD32F407VG";
    link.execute(c, Operation::Connect, {});
    QVERIFY(done.wait());
    QVERIFY(done.takeFirst().at(0).toBool());
    QCOMPARE(ids.size(), 1);
    QVERIFY(ids.last().at(0).toString().isEmpty());
  }
  void cliErrorWithZeroExit() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "FAIL";
    QSignalSpy done(&link, &JLink::finished);
    link.execute(c, Operation::Connect, {});
    QVERIFY(done.wait());
    QCOMPARE(done.first().first().toBool(), false);
  }
  void cliProgressLog() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "PROGRESS";
    QSignalSpy done(&link, &JLink::finished), logs(&link, &JLink::log),
        updates(&link, &JLink::progressChanged), stages(&link, &JLink::stage);
    link.execute(c, Operation::Connect, {});
    QTRY_VERIFY_WITH_TIMEOUT(updates.size() >= 2, 3000);
    QVERIFY(done.isEmpty());
    QCOMPARE(updates.at(0).at(0).toInt(), 0);
    QCOMPARE(updates.at(1).at(0).toInt(), 50);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QVERIFY(done.first().first().toBool());
    QStringList lines;
    for (const auto &message : logs)
      lines << message.first().toString();
    QVERIFY(lines.contains("Programming flash [100%] Done."));
    QVERIFY(lines.contains("Erasing flash     [100%] Done."));
    QList<int> values;
    for (const auto &event : updates)
      values << event.first().toInt();
    QCOMPARE(values, (QList<int>{0, 50, 100, -1, 0, 100, -1}));
    QStringList phaseNames;
    for (const auto &event : stages)
      phaseNames << event.first().toString();
    QVERIFY(phaseNames.contains("Programming flash"));
    QVERIFY(phaseNames.contains("Erasing flash"));
    for (const auto &line : lines) {
      QVERIFY(!line.contains('\b'));
      QVERIFY(!line.contains('\r'));
    }
  }
  void cliCancel() {
    JLink link;
    LinkConfig c;
    c.executable = QString::fromUtf8(FAKE_JLINK_PATH);
    c.device = "SLOW";
    QSignalSpy done(&link, &JLink::finished);
    link.execute(c, Operation::Connect, {});
    QTimer::singleShot(50, &link, &JLink::cancel);
    QVERIFY(done.wait());
    QCOMPARE(done.first().first().toBool(), false);
    QVERIFY(!link.busy());
  }
  void cliCatalog() {
    JLink link;
    QSignalSpy done(&link, &JLink::finished), devices(&link, &JLink::devices), ranges(&link, &JLink::flashRanges);
    link.catalog(QString::fromUtf8(FAKE_JLINK_PATH));
    QVERIFY(done.wait());
    QCOMPARE(done.first().first().toBool(), true);
    QCOMPARE(devices.first().first().toStringList().size(), 2);
    QVERIFY(devices.first().first().toStringList().contains("STM32F407VG"));
    const auto flash = ranges.first().first().toMap().value("STM32F407VG").toMap();
    QCOMPARE(flash.value("address").toUInt(), 0x08000000u);
    QCOMPARE(flash.value("length").toUInt(), 0x00100000u);
    QCOMPARE(JLink::deviceName("\"ST\", \"STM32F407VG\", \"Cortex-M4\""),
             QString("STM32F407VG"));
  }
  void flashCatalogRanges() {
    const auto prefix = QStringLiteral("\"ST\", \"Example\", \"Cortex-M4\", ");
    auto range = JLink::flashRange(prefix + "{{0x08000000, 0x10000}, {0x08010000, 0x20000}}, {0x20000000, 0x10000}");
    QCOMPARE(range.value("address").toUInt(), 0x08000000u);
    QCOMPARE(range.value("length").toUInt(), 0x30000u);
    QVERIFY(JLink::flashRange(prefix + "{{0x08000000, 0x10000}, {0x08100000, 0x10000}}, {0x20000000, 0x10000}").isEmpty());
    QVERIFY(JLink::flashRange(prefix + "{0x0, 0x0}, {0x20000000, 0x10000}").isEmpty());
    QVERIFY(JLink::flashRange(prefix + "{0xffffffffffffffff, 0x10}, {0x0, 0x0}").isEmpty());
  }
  void realProbeConnection() {
    const auto executable = qEnvironmentVariable("TAILOR_TEST_JLINK");
    if (executable.isEmpty())
      QSKIP("Set TAILOR_TEST_JLINK to test with a connected target");
    JLink link;
    LinkConfig c;
    c.executable = executable;
    c.device = JLink::deviceName("\"ST\", \"STM32F407VG\", \"Cortex-M4\"");
    c.serial = qEnvironmentVariable("TAILOR_TEST_SERIAL");
    QSignalSpy done(&link, &JLink::finished);
    connect(&link, &JLink::log, this, [](const QString &line) { qInfo().noquote() << line; });
    link.execute(c, Operation::Connect, {});
    QVERIFY(done.wait(30000));
    QVERIFY2(done.first().first().toBool(), qPrintable(done.first().at(1).toString()));
  }
  void realCatalog() {
    const auto executable = qEnvironmentVariable("TAILOR_TEST_JLINK");
    if (executable.isEmpty())
      QSKIP("Set TAILOR_TEST_JLINK to test the installed catalog");
    JLink link;
    QSignalSpy done(&link, &JLink::finished), ranges(&link, &JLink::flashRanges);
    link.catalog(executable);
    QVERIFY(done.wait(30000));
    QVERIFY(done.first().first().toBool());
    const auto flash = ranges.first().first().toMap().value("STM32F407VG").toMap();
    QCOMPARE(flash.value("address").toUInt(), 0x08000000u);
    QCOMPARE(flash.value("length").toUInt(), 0x00100000u);
  }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "CoreTest.moc"
