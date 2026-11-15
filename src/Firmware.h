#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
struct Segment {
  quint32 address{};
  QByteArray data;
};
enum class ChecksumAlgorithm { Md5, Crc32Ethernet, Sha256 };
struct Firmware {
  QVector<Segment> segments;
  static Firmware load(const QString &path, quint32 base);
  static Firmware parseHex(const QByteArray &text);
  QByteArray toHex() const;
  qsizetype size() const;
  QString checksum(ChecksumAlgorithm algorithm) const;
};
quint32 addressValue(const QString &text);
quint32 lengthValue(const QString &text);
