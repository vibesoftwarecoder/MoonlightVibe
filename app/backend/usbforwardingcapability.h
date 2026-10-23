#pragma once

// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <optional>

namespace UsbForwarding {

// Largest capability response or tunnel handshake line we will buffer.
constexpr qsizetype kMaxHandshakeBytes = 4 * 1024;

// A USB/IP bus id as the host accepts it: [0-9A-Za-z.:-], 1 to 31 bytes.
// Every place that forwards a bus id (tunnel handshake, usbipd command
// line) must pass it through this check first.
inline bool isValidBusId(const QByteArray& busId) noexcept {
    constexpr qsizetype kMaxBusIdBytes = 31;
    if (busId.isEmpty() || busId.size() > kMaxBusIdBytes) return false;
    for (const char c : busId) {
        const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == ':';
        if (!allowed) return false;
    }
    return true;
}

// Credentials stay in memory; never log or persist this object.
struct Capability {
    bool available = false;
    QString reason;
    quint16 port = 0;
    QByteArray token;

    static std::optional<Capability> parse(const QByteArray& bytes) {
        if (bytes.size() > kMaxHandshakeBytes) return std::nullopt;
        const auto document = QJsonDocument::fromJson(bytes);
        if (!document.isObject()) return std::nullopt;
        const auto json = document.object();
        if (json.value("version").toDouble(-1) != 1 ||
            !json.value("enabled").isBool() || !json.value("available").isBool()) return std::nullopt;
        Capability result;
        result.available = json.value("enabled").toBool() && json.value("available").toBool();
        result.reason = json.value("reason").toString();
        if (result.available) {
            const double port = json.value("port").toDouble(-1);
            const QString token = json.value("token").toString();
            if (port < 1 || port > 65535 || port != static_cast<int>(port) ||
                !QRegularExpression(QStringLiteral("\\A[0-9a-fA-F]{64}\\z")).match(token).hasMatch()) return std::nullopt;
            result.port = static_cast<quint16>(port);
            result.token = token.toLatin1();
        }
        return result;
    }
};
}
