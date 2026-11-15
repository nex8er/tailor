#include "Firmware.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
namespace {
void fail(const QString &s) { throw std::runtime_error(s.toStdString()); }
QByteArray record(quint16 address, int type, const QByteArray &data) {
  QByteArray b;
  b.append(static_cast<char>(data.size()));
  b.append(static_cast<char>(address >> 8));
  b.append(static_cast<char>(address));
  b.append(static_cast<char>(type));
  b += data;
  unsigned sum = 0;
  for (auto c : b)
    sum += static_cast<unsigned char>(c);
  b.append(static_cast<char>(0u - sum));
  return ":" + b.toHex().toUpper() + "\n";
}
} // namespace
quint32 addressValue(const QString &text) {
  bool ok = false;
  const auto n = text.trimmed().toULongLong(&ok, 0);
  if (!ok || n > std::numeric_limits<quint32>::max())
    fail("Invalid 32-bit address/length: " + text);
  return static_cast<quint32>(n);
}
quint32 lengthValue(const QString &text) {
  static const QRegularExpression syntax(
      "^(0[xX][0-9a-fA-F]+|[0-9]+)\\s*([kKmM]?)$");
  const auto match = syntax.match(text.trimmed());
  if (!match.hasMatch())
    fail("Invalid length: " + text);
  const auto number = match.captured(1);
  bool ok = false;
  const auto value = number.toULongLong(&ok, number.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
  const auto suffix = match.captured(2).toLower();
  const quint64 multiplier = suffix == "k" ? 1024 : suffix == "m" ? 1024 * 1024 : 1;
  if (!ok || value > std::numeric_limits<quint32>::max() / multiplier)
    fail("Invalid 32-bit length: " + text);
  return static_cast<quint32>(value * multiplier);
}
qsizetype Firmware::size() const {
  qsizetype n = 0;
  for (const auto &s : segments)
    n += s.data.size();
  return n;
}
QString Firmware::checksum(ChecksumAlgorithm algorithm) const {
  if (algorithm == ChecksumAlgorithm::Crc32Ethernet) {
    static constexpr auto table = [] {
      std::array<quint32, 256> values{};
      for (quint32 i = 0; i < values.size(); ++i) {
        auto value = i;
        for (int bit = 0; bit < 8; ++bit)
          value = (value >> 1) ^ ((value & 1u) ? 0xedb88320u : 0u);
        values[i] = value;
      }
      return values;
    }();
    quint32 crc = 0xffffffffu;
    for (const auto &segment : segments)
      for (const auto byte : segment.data)
        crc = (crc >> 8) ^ table[(crc ^ static_cast<quint8>(byte)) & 0xffu];
    return QString("%1").arg(crc ^ 0xffffffffu, 8, 16, QChar('0'));
  }
  QCryptographicHash hash(algorithm == ChecksumAlgorithm::Sha256
                              ? QCryptographicHash::Sha256
                              : QCryptographicHash::Md5);
  for (const auto &segment : segments)
    hash.addData(segment.data);
  return QString::fromLatin1(hash.result().toHex());
}
Firmware Firmware::load(const QString &path, quint32 base) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    fail(f.errorString());
  if (f.size() > 256 * 1024 * 1024)
    fail("File exceeds 256 MiB limit");
  const auto data = f.readAll();
  if (data.isEmpty())
    fail("Empty firmware");
  if (QFileInfo(path).suffix().compare("hex", Qt::CaseInsensitive) == 0)
    return parseHex(data);
  if (QFileInfo(path).suffix().compare("bin", Qt::CaseInsensitive) != 0)
    fail("Expected .hex or .bin");
  if (quint64(base) + quint64(data.size()) > 0x100000000ULL)
    fail("Address overflow");
  return {{{base, data}}};
}
Firmware Firmware::parseHex(const QByteArray &text) {
  QMap<quint32, QByteArray> blocks;
  quint64 upper = 0;
  bool eof = false;
  int lineNo = 0;
  static const QRegularExpression syntax("^[0-9A-Fa-f]+$");
  for (auto line : text.split('\n')) {
    ++lineNo;
    line = line.trimmed();
    if (line.isEmpty())
      continue;
    auto bad = [&] {
      fail(QString("Invalid Intel HEX record at line %1").arg(lineNo));
    };
    if (eof || !line.startsWith(':') || line.size() < 11 ||
        (line.size() % 2) != 1 ||
        !syntax.match(QString::fromLatin1(line.mid(1))).hasMatch())
      bad();
    const auto b = QByteArray::fromHex(line.mid(1));
    const auto byte = [&](int i) {
      return static_cast<unsigned char>(b.at(i));
    };
    const int count = byte(0);
    if (b.size() != count + 5)
      bad();
    unsigned sum = 0;
    for (auto c : b)
      sum += static_cast<unsigned char>(c);
    if ((sum & 255) != 0)
      bad();
    const quint32 low = quint32(byte(1)) * 256 + byte(2);
    const int type = byte(3);
    if (type == 0) {
      const quint64 start = upper + low, end = start + quint64(count);
      if (end > 0x100000000ULL)
        bad();
      if (count == 0)
        continue;
      auto it = blocks.lowerBound(static_cast<quint32>(start));
      if (it != blocks.end() && quint64(it.key()) < end)
        bad();
      if (it != blocks.begin()) {
        auto prev = std::prev(it);
        if (quint64(prev.key()) + quint64(prev.value().size()) > start)
          bad();
      }
      blocks.insert(static_cast<quint32>(start), b.mid(4, count));
    } else if (type == 1) {
      if (count != 0 || low != 0)
        bad();
      eof = true;
    } else if (type == 2 || type == 4) {
      if (count != 2 || low != 0)
        bad();
      upper = quint64(byte(4) * 256 + byte(5)) << (type == 2 ? 4 : 16);
    } else if (type == 3 || type == 5) {
      if (count != 4 || low != 0)
        bad();
    } else
      bad();
  }
  if (!eof || blocks.isEmpty())
    fail("Intel HEX has no EOF or data");
  Firmware result;
  for (auto it = blocks.begin(); it != blocks.end(); ++it) {
    if (!result.segments.isEmpty() &&
        quint64(result.segments.last().address) +
                quint64(result.segments.last().data.size()) ==
            it.key())
      result.segments.last().data += it.value();
    else
      result.segments.append({it.key(), it.value()});
  }
  return result;
}
QByteArray Firmware::toHex() const {
  QByteArray out;
  quint32 last = 0xffffffff;
  for (const auto &s : segments)
    for (qsizetype i = 0; i < s.data.size();) {
      const auto a = static_cast<quint32>(quint64(s.address) + quint64(i));
      const auto hi = a >> 16;
      if (hi != last) {
        QByteArray b;
        b.append(static_cast<char>(hi >> 8));
        b.append(static_cast<char>(hi));
        out += record(0, 4, b);
        last = hi;
      }
      const auto n =
          std::min<qsizetype>({16, s.data.size() - i, 65536 - (a & 65535)});
      out += record(static_cast<quint16>(a), 0, s.data.mid(i, n));
      i += n;
    }
  return out + record(0, 1, {});
}
