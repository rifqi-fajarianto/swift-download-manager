#include "ui/browser_integration.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <utility>

#include "swiftdm/version.h"

namespace swiftdm::ui {
namespace {

constexpr qsizetype kMaxFrameBytes = 1024 * 1024;
constexpr int kMaxClients = 32;
constexpr int kHandshakeTimeoutMs = 5000;
constexpr char kExtensionId[] = "hagednkleljjijmjonpeiokknhngknbb";
const QByteArray kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const QString kSubprotocol = QStringLiteral("swiftdm.v1");

static_assert(sizeof(kExtensionId) - 1 == 32,
              "extension ID must match Chromium's 32-character ID format");

// The checked-in manifest has a fixed public key, so its extension ID is stable
// across load locations. Only the app's own ID is allowed; a shape-only check
// would accept any extension installed in the browser. Edge may report the
// same ID with either of its extension schemes.
bool is_chromium_extension_origin(const QByteArray& origin) {
    static const QRegularExpression pattern(
        QStringLiteral("^(chrome-extension|extensions|extension)://([a-p]{32})$"));
    const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(origin));
    if (!match.hasMatch()) return false;
    return match.captured(2) == QLatin1String(kExtensionId);
}

QByteArray websocket_frame(const QByteArray& payload, quint8 opcode = 0x1) {
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.append(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.append(static_cast<char>(127));
        const quint64 length = static_cast<quint64>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.append(static_cast<char>((length >> shift) & 0xff));
        }
    }
    frame.append(payload);
    return frame;
}

QByteArray json_frame(const QJsonObject& object) {
    return websocket_frame(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool is_http_url(const QString& text) {
    const QUrl url(text);
    return url.isValid() && (url.scheme() == QStringLiteral("http") ||
                             url.scheme() == QStringLiteral("https")) &&
           !url.host().isEmpty() && url.userName().isEmpty() && url.password().isEmpty();
}

// Download targets may also be FTP links (the extension forwards them when a
// page still exposes one; modern Chromium handles ftp:// internally).
bool is_download_url(const QString& text) {
    if (is_http_url(text)) return true;
    const QUrl url(text);
    return url.isValid() && url.scheme() == QStringLiteral("ftp") && !url.host().isEmpty() &&
           url.userName().isEmpty() && url.password().isEmpty();
}

}  // namespace

BrowserIntegration::BrowserIntegration(QObject* parent, quint16 port) : QObject(parent) {
    connect(&server_, &QTcpServer::newConnection, this,
            &BrowserIntegration::accept_connections);
    if (!server_.listen(QHostAddress::LocalHost, port)) {
        error_ = server_.errorString();
        qWarning() << "browser integration: cannot listen on 127.0.0.1" << port << "-"
                   << error_;
        // Emit after construction so MainWindow can connect to this signal.
        const QString error = error_;
        QMetaObject::invokeMethod(
            this, [this, error] { emit serverError(error); }, Qt::QueuedConnection);
    }
}

BrowserIntegration::~BrowserIntegration() {
    server_.close();
    const auto sockets = buffers_.keys();
    buffers_.clear();
    handshaken_.clear();
    handshake_timers_.clear();
    subscribed_.clear();
    for (QTcpSocket* socket : sockets) {
        if (!socket) continue;
        socket->disconnect(this);
        socket->abort();
        delete socket;
    }
}

void BrowserIntegration::accept_connections() {
    while (server_.hasPendingConnections()) {
        QTcpSocket* socket = server_.nextPendingConnection();
        if (!socket) continue;
        if (buffers_.size() >= kMaxClients) {
            qWarning() << "browser integration: too many clients; rejecting connection";
            socket->abort();
            socket->deleteLater();
            continue;
        }
        buffers_.insert(socket, {});
        handshaken_.insert(socket, false);
        auto* handshake_timer = new QTimer(socket);
        handshake_timer->setSingleShot(true);
        handshake_timers_.insert(socket, handshake_timer);
        connect(handshake_timer, &QTimer::timeout, this, [this, socket] {
            if (!buffers_.contains(socket) || handshaken_.value(socket)) return;
            qWarning() << "browser integration: WebSocket handshake timed out";
            buffers_.remove(socket);
            socket->disconnectFromHost();
        });
        handshake_timer->start(kHandshakeTimeoutMs);
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { read_socket(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
            handshaken_.remove(socket);
            handshake_timers_.remove(socket);
            subscribed_.remove(socket);
            socket->deleteLater();
        });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [socket](QAbstractSocket::SocketError) {
                    if (socket->state() != QAbstractSocket::ConnectedState) {
                        socket->deleteLater();
                    }
                });
    }
}

void BrowserIntegration::read_socket(QTcpSocket* socket) {
    if (!buffers_.contains(socket)) return;
    QByteArray buffer = buffers_.value(socket);
    buffer.append(socket->readAll());
    if (buffer.size() > kMaxFrameBytes + 16384) {
        qWarning() << "browser integration: oversized handshake/frame; closing connection";
        buffers_.remove(socket);
        socket->disconnectFromHost();
        return;
    }

    if (!handshaken_.value(socket)) {
        const qsizetype end = buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            buffers_[socket] = buffer;
            return;
        }
        if (end > 16384) {
            buffers_.remove(socket);
            socket->disconnectFromHost();
            return;
        }
        const QByteArray request = buffer.left(end);
        buffer.remove(0, end + 4);
        const QList<QByteArray> lines = request.split('\n');
        auto reject = [this, socket](const QString& reason) {
            if (QTimer* timer = handshake_timers_.value(socket, nullptr)) timer->stop();
            handshake_error_ = reason;
            qWarning() << "browser integration:" << reason;
            emit handshakeRejected(reason);
            buffers_.remove(socket);
            socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n");
            socket->disconnectFromHost();
        };
        if (lines.isEmpty() || lines.first().trimmed() != "GET / HTTP/1.1") {
            reject(QStringLiteral("unsupported request line '%1'").arg(
                QString::fromLatin1(lines.isEmpty() ? QByteArray()
                                                    : lines.first().trimmed())));
            return;
        }
        QHash<QByteArray, QByteArray> headers;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                headers.insert(line.left(colon).trimmed().toLower(),
                               line.mid(colon + 1).trimmed());
            }
        }
        const QByteArray origin = headers.value("origin");
        const QByteArray key = headers.value("sec-websocket-key");
        const QByteArray protocols = headers.value("sec-websocket-protocol");
        const QByteArray decoded_key = QByteArray::fromBase64(key);
        bool connection_upgrade = false;
        for (QByteArray token : headers.value("connection").toLower().split(',')) {
            if (token.trimmed() == "upgrade") connection_upgrade = true;
        }
        const QByteArray needed_protocol = kSubprotocol.toUtf8();
        bool protocol_ok = false;
        for (QByteArray token : protocols.split(',')) {
            if (token.trimmed() == needed_protocol) protocol_ok = true;
        }
        if (!is_chromium_extension_origin(origin) || decoded_key.size() != 16 ||
            headers.value("upgrade").toLower() != "websocket" || !connection_upgrade ||
            headers.value("sec-websocket-version") != "13" || !protocol_ok) {
            reject(QStringLiteral("rejected handshake: origin='%1' protocol='%2'")
                       .arg(QString::fromLatin1(origin),
                            QString::fromLatin1(protocols)));
            return;
        }
        const QByteArray accept = QCryptographicHash::hash(
                                      key + kWebSocketGuid, QCryptographicHash::Sha1)
                                      .toBase64();
        QByteArray response = "HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: " + accept + "\r\n"
                              "Sec-WebSocket-Protocol: " + needed_protocol +
                              "\r\n\r\n";
        socket->write(response);
        handshaken_[socket] = true;
        if (QTimer* timer = handshake_timers_.value(socket, nullptr)) timer->stop();
        handshake_error_.clear();
        qInfo() << "browser integration: extension connected; origin=" << origin.constData();
    }

    while (handshaken_.value(socket) &&
           socket->state() == QAbstractSocket::ConnectedState) {
        const qsizetype before = buffer.size();
        process_frame(socket, buffer);
        if (!buffers_.contains(socket)) return;
        if (buffer.size() == before) break;
    }
    if (buffers_.contains(socket)) buffers_[socket] = buffer;
}

void BrowserIntegration::process_frame(QTcpSocket* socket, QByteArray& buffer) {
    if (buffer.size() < 2) return;
    const quint8 first = static_cast<quint8>(buffer.at(0));
    const quint8 second = static_cast<quint8>(buffer.at(1));
    const bool final = (first & 0x80) != 0;
    const quint8 opcode = first & 0x0f;
    const bool masked = (second & 0x80) != 0;
    quint64 length = second & 0x7f;
    qsizetype header_size = 2;
    if (!final || (first & 0x70) != 0 || !masked) {
        socket->disconnectFromHost();
        buffer.clear();
        return;
    }
    if (length == 126) {
        if (buffer.size() < 4) return;
        length = (static_cast<quint64>(static_cast<quint8>(buffer.at(2))) << 8) |
                 static_cast<quint8>(buffer.at(3));
        header_size = 4;
    } else if (length == 127) {
        if (buffer.size() < 10) return;
        length = 0;
        for (qsizetype i = 2; i < 10; ++i) {
            length = (length << 8) | static_cast<quint8>(buffer.at(i));
        }
        header_size = 10;
    }
    if (length > static_cast<quint64>(kMaxFrameBytes) ||
        (opcode >= 0x8 && length > 125)) {
        socket->disconnectFromHost();
        buffer.clear();
        return;
    }
    const qsizetype frame_size = header_size + 4 + static_cast<qsizetype>(length);
    if (buffer.size() < frame_size) return;
    const QByteArray mask = buffer.mid(header_size, 4);
    QByteArray payload = buffer.mid(header_size + 4, static_cast<qsizetype>(length));
    buffer.remove(0, frame_size);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(payload.at(i) ^ mask.at(i % 4));
    }

    if (opcode == 0x8) {
        socket->write(websocket_frame(payload, 0x8));
        socket->disconnectFromHost();
    } else if (opcode == 0x9) {
        socket->write(websocket_frame(payload, 0xA));
    } else if (opcode == 0x1) {
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            send_json(socket, {{QStringLiteral("v"), 1}, {QStringLiteral("id"), QString()},
                              {QStringLiteral("ok"), false},
                              {QStringLiteral("error"), QStringLiteral("invalid JSON")}});
            return;
        }
        send_json(socket, handle_message(socket, document.object()));
    } else {
        socket->disconnectFromHost();
    }
}

void BrowserIntegration::send_json(QTcpSocket* socket, const QJsonObject& response) {
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        socket->write(json_frame(response));
    }
}

void BrowserIntegration::set_status(bool waiting, int active_downloads) {
    if (waiting_ == waiting && active_downloads_ == active_downloads) return;
    waiting_ = waiting;
    active_downloads_ = active_downloads;
    if (subscribed_.isEmpty()) return;
    QJsonObject status;
    status.insert(QStringLiteral("waiting"), waiting_);
    status.insert(QStringLiteral("active"), active_downloads_);
    const QJsonObject event{{QStringLiteral("v"), 1},
                            {QStringLiteral("event"), QStringLiteral("status")},
                            {QStringLiteral("data"), status}};
    for (QTcpSocket* socket : std::as_const(subscribed_)) {
        send_json(socket, event);
    }
}

QJsonObject BrowserIntegration::handle_message(QTcpSocket* socket, const QJsonObject& message) {
    const QJsonValue id_value = message.value(QStringLiteral("id"));
    const QString id = id_value.isString() ? id_value.toString() : QString();
    const QString command = message.value(QStringLiteral("cmd")).toString();
    const QJsonObject data = message.value(QStringLiteral("data")).toObject();
    bool ok = message.value(QStringLiteral("v")).toInt() == 1 &&
              id_value.isString() && !id.isEmpty() && id.size() <= 128 &&
              message.value(QStringLiteral("data")).isObject();
    QString error;
    QJsonObject result;

    if (!ok) {
        error = QStringLiteral("invalid message envelope");
    } else if (command == QStringLiteral("ping")) {
        result.insert(QStringLiteral("pong"), true);
        result.insert(QStringLiteral("version"), QString::fromLatin1(swiftdm::kVersionString));
    } else if (command == QStringLiteral("getVersion")) {
        result.insert(QStringLiteral("version"), QString::fromLatin1(swiftdm::kVersionString));
    } else if (command == QStringLiteral("subscribe")) {
        // The extension keeps this socket open to receive push events without
        // polling. Older builds never send subscribe and are unaffected.
        subscribed_.insert(socket);
        result.insert(QStringLiteral("subscribed"), true);
        result.insert(QStringLiteral("version"), QString::fromLatin1(swiftdm::kVersionString));
        QJsonObject status;
        status.insert(QStringLiteral("waiting"), waiting_);
        status.insert(QStringLiteral("active"), active_downloads_);
        result.insert(QStringLiteral("status"), status);
    } else if (command == QStringLiteral("showPanel")) {
        if (!data.value(QStringLiteral("visible")).isBool()) {
            ok = false;
            error = QStringLiteral("visible must be boolean");
        } else {
            emit showPanelRequested(data.value(QStringLiteral("visible")).toBool());
        }
    } else if (command == QStringLiteral("addDownload")) {
        const QString url = data.value(QStringLiteral("url")).toString();
        const QJsonValue cookies = data.value(QStringLiteral("cookies"));
        const QString referer = data.value(QStringLiteral("referer")).toString();
        const QString user_agent = data.value(QStringLiteral("userAgent")).toString();
        const QString filename = data.value(QStringLiteral("filename")).toString();
        // Older extension builds sent an empty string when no cookie permission
        // was granted; treat that as "no cookies" while rejecting raw Cookie
        // headers (which would bypass domain scoping).
        bool cookies_valid =
            cookies.isUndefined() || cookies.isArray() ||
            (cookies.isString() && cookies.toString().isEmpty());
        const QUrl download_url(url);
        if (cookies.isArray()) {
            const QJsonArray entries = cookies.toArray();
            cookies_valid = entries.size() <= 256;
            static const QRegularExpression cookie_token(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$"));
            static const QRegularExpression cookie_domain(
                QStringLiteral("^\\.?[A-Za-z0-9.-]+$"));
            static const QRegularExpression cookie_controls(
                QStringLiteral("[\\x00-\\x20\\x7f]"));
            for (const QJsonValue& entry : entries) {
                if (!entry.isObject()) {
                    cookies_valid = false;
                    break;
                }
                const QJsonObject cookie = entry.toObject();
                const QString domain = cookie.value(QStringLiteral("domain")).toString();
                const QString path = cookie.value(QStringLiteral("path")).toString();
                const QString name = cookie.value(QStringLiteral("name")).toString();
                const QString value = cookie.value(QStringLiteral("value")).toString();
                QString cookie_host = domain.toLower();
                if (cookie_host.startsWith(QLatin1Char('.'))) cookie_host.remove(0, 1);
                const QString download_host = download_url.host().toLower();
                const bool domain_matches = download_host == cookie_host ||
                    download_host.endsWith(QLatin1Char('.') + cookie_host);
                if (!cookie_domain.match(domain).hasMatch() || domain.size() > 255 ||
                    !domain_matches || path.isEmpty() || path.size() > 2048 ||
                    !path.startsWith(QLatin1Char('/')) || cookie_controls.match(path).hasMatch() ||
                    !cookie_token.match(name).hasMatch() || value.size() > 4096 ||
                    cookie_controls.match(value).hasMatch() ||
                    !cookie.value(QStringLiteral("secure")).isBool() ||
                    !cookie.value(QStringLiteral("expires")).isDouble() ||
                    cookie.value(QStringLiteral("expires")).toDouble() < 0 ||
                    cookie.value(QStringLiteral("expires")).toDouble() > 253402300799.0) {
                    cookies_valid = false;
                    break;
                }
            }
        }
        const bool referer_valid = referer.isEmpty() || is_http_url(referer);
        const QUrl referer_url(referer);
        const bool referer_is_safe = referer.isEmpty() ||
                                     (referer_url.scheme() == download_url.scheme() &&
                                      referer_url.host().compare(download_url.host(),
                                                                 Qt::CaseInsensitive) == 0 &&
                                      referer_url.port(0) == download_url.port(0));
        const bool user_agent_valid = !user_agent.contains(
            QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));

        // POST handoffs carry a bounded body plus optional custom x-* request
        // headers, mirroring the original extension's model. Raw Cookie or
        // Authorization headers are never accepted here: cookies stay
        // structured and domain-scoped.
        const QString method = data.value(QStringLiteral("method")).toString().toUpper();
        const QString post_data = data.value(QStringLiteral("postData")).toString();
        const QString post_encoding = data.value(QStringLiteral("postDataEncoding")).toString();
        constexpr qsizetype kMaxPostBytes = 256 * 1024;
        constexpr int kMaxExtraHeaders = 16;
        bool method_valid = method.isEmpty() || method == QStringLiteral("GET");
        if (method == QStringLiteral("POST")) method_valid = !post_data.isEmpty();
        bool post_valid = post_data.isEmpty() && post_encoding.isEmpty();
        if (!post_data.isEmpty()) {
            const bool encoding_ok = post_encoding.isEmpty() ||
                                     post_encoding == QStringLiteral("utf8") ||
                                     post_encoding == QStringLiteral("base64");
            bool body_ok = post_data.size() <= kMaxPostBytes;
            if (encoding_ok && post_encoding == QStringLiteral("base64")) {
                static const QRegularExpression base64_pattern(
                    QStringLiteral("^[A-Za-z0-9+/]*={0,2}$"));
                body_ok = body_ok && base64_pattern.match(post_data).hasMatch() &&
                          QByteArray::fromBase64(post_data.toLatin1()).size() <= kMaxPostBytes;
            }
            post_valid = encoding_ok && body_ok &&
                         (method.isEmpty() || method == QStringLiteral("POST"));
        }
        const QJsonValue header_value = data.value(QStringLiteral("headers"));
        bool headers_valid = header_value.isUndefined();
        if (header_value.isArray()) {
            const QJsonArray entries = header_value.toArray();
            headers_valid = entries.size() <= kMaxExtraHeaders;
            static const QRegularExpression header_name(
                QStringLiteral("^[Xx]-[A-Za-z0-9!#$%&'*+.^_`|~-]{0,80}$"));
            static const QRegularExpression header_controls(
                QStringLiteral("[\\x00-\\x1f\\x7f]"));
            qsizetype total = 0;
            for (const QJsonValue& entry : entries) {
                if (!headers_valid) break;
                if (!entry.isObject()) {
                    headers_valid = false;
                    break;
                }
                const QJsonObject header = entry.toObject();
                const QString name = header.value(QStringLiteral("name")).toString();
                const QString value = header.value(QStringLiteral("value")).toString();
                total += name.size() + value.size();
                if (!header_name.match(name).hasMatch() || value.size() > 2048 ||
                    header_controls.match(value).hasMatch() || total > 16384) {
                    headers_valid = false;
                }
            }
        }
        // Adaptive (video + audio) pairs share a short opaque token so the app
        // can recognize both halves later (muxing is a later release).
        const QJsonValue group_value = data.value(QStringLiteral("group"));
        bool group_valid = group_value.isUndefined() || group_value.isNull();
        if (!group_valid && group_value.isString()) {
            static const QRegularExpression group_pattern(
                QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
            group_valid = group_pattern.match(group_value.toString()).hasMatch();
        }
        if (!is_download_url(url) || !cookies_valid || !referer_valid || !referer_is_safe ||
            referer.size() > 8192 || !user_agent_valid || user_agent.size() > 2048 ||
            filename.size() > 255 || !method_valid || !post_valid || !headers_valid ||
            !group_valid) {
            ok = false;
            error = QStringLiteral("invalid download request");
        } else {
            emit downloadRequested(data);
        }
    } else {
        ok = false;
        error = QStringLiteral("unknown command");
    }

    if (ok) {
        qInfo() << "browser integration: command" << command << "accepted";
    } else {
        qWarning() << "browser integration: command" << command << "rejected:" << error;
    }

    QJsonObject response{{QStringLiteral("v"), 1},
                         {QStringLiteral("id"), id},
                         {QStringLiteral("ok"), ok}};
    if (ok && !result.isEmpty()) response.insert(QStringLiteral("data"), result);
    if (!ok) response.insert(QStringLiteral("error"), error);
    return response;
}

}  // namespace swiftdm::ui
