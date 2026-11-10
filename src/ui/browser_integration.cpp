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
#include <QUrlQuery>

#include <cmath>
#include <utility>

#include "core/browser_token.h"
#include "swiftdm/version.h"

namespace swiftdm::ui {
namespace {

constexpr qsizetype kMaxFrameBytes = 1024 * 1024;
constexpr int kMaxClients = 32;
constexpr int kHandshakeTimeoutMs = 5000;
// ID of the checked-in Chromium manifest; `tools/extension_pack.mjs` verifies
// that its public key still hashes to this value. Store installs differ, so it
// is only used to keep that check meaningful and to spot them in the log.
constexpr char kExtensionId[] = "hagednkleljjijmjonpeiokknhngknbb";
const QByteArray kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const QString kSubprotocol = QStringLiteral("swiftdm.v1");

static_assert(sizeof(kExtensionId) - 1 == 32,
              "extension ID must match Chromium's 32-character ID format");

// Both browser families get their origin validated by shape, not by pinned ID.
//
// Chromium: the checked-in manifest's fixed public key only decides the ID of a
// load-unpacked install. Edge Add-ons and the Chrome Web Store re-sign the
// package with their own key, so a published install reports an ID that cannot
// be known before the listing exists (Edge: lpfidepmgdmkbkfnlpomnjjhadefgklg).
// Pinning would mean one app release per store, per listing, so any
// `<a-p>{32}` ID is accepted; a handoff still passes the download confirmation
// dialog (unless the user turned it off) and the origin is logged.
//
// Firefox: `moz-extension://` carries a UUID that Firefox generates per profile
// and per installation, and the `key` manifest field is unsupported there
// (`browser_specific_settings.gecko.id` names the add-on but does not appear in
// the Origin header).
bool is_allowed_extension_origin(const QByteArray& origin) {
    static const QRegularExpression chromium_pattern(
        QStringLiteral("^(chrome-extension|extensions|extension)://([a-p]{32})$"));
    const QRegularExpressionMatch chromium =
        chromium_pattern.match(QString::fromLatin1(origin));
    if (chromium.hasMatch()) {
        const QString id = chromium.captured(2);
        if (id != QLatin1String(kExtensionId)) {
            qInfo() << "browser integration: accepting store-signed extension id" << id;
        }
        return true;
    }

    static const QRegularExpression firefox_pattern(QStringLiteral(
        "^moz-extension://[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return firefox_pattern.match(QString::fromLatin1(origin)).hasMatch();
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

constexpr qsizetype kMaxPostBytes = 256 * 1024;
constexpr int kMaxExtraHeaders = 16;
constexpr qsizetype kMaxUrlLength = 8192;

// Structured, domain-scoped cookies. Older extension builds sent an empty
// string when no cookie permission was granted; that still means "no cookies",
// while a raw Cookie header (which would bypass domain scoping) is rejected.
bool cookies_are_valid(const QJsonValue& cookies, const QUrl& download_url) {
    if (cookies.isUndefined()) return true;
    if (cookies.isString()) return cookies.toString().isEmpty();
    if (!cookies.isArray()) return false;

    const QJsonArray entries = cookies.toArray();
    if (entries.size() > 256) return false;
    static const QRegularExpression cookie_token(
        QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$"));
    static const QRegularExpression cookie_domain(QStringLiteral("^\\.?[A-Za-z0-9.-]+$"));
    static const QRegularExpression cookie_controls(QStringLiteral("[\\x00-\\x20\\x7f]"));
    const QString download_host = download_url.host().toLower();
    for (const QJsonValue& entry : entries) {
        if (!entry.isObject()) return false;
        const QJsonObject cookie = entry.toObject();
        const QString domain = cookie.value(QStringLiteral("domain")).toString();
        const QString path = cookie.value(QStringLiteral("path")).toString();
        const QString name = cookie.value(QStringLiteral("name")).toString();
        const QString value = cookie.value(QStringLiteral("value")).toString();
        QString cookie_host = domain.toLower();
        if (cookie_host.startsWith(QLatin1Char('.'))) cookie_host.remove(0, 1);
        const bool domain_matches =
            download_host == cookie_host ||
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
            return false;
        }
    }
    return true;
}

// Extra request headers are limited to `x-*` names: raw Cookie, Authorization
// or Referer headers would bypass the structured cookie and referer rules.
bool headers_are_valid(const QJsonValue& header_value) {
    if (header_value.isUndefined()) return true;
    if (!header_value.isArray()) return false;
    const QJsonArray entries = header_value.toArray();
    if (entries.size() > kMaxExtraHeaders) return false;
    static const QRegularExpression header_name(
        QStringLiteral("^[Xx]-[A-Za-z0-9!#$%&'*+.^_`|~-]{0,80}$"));
    static const QRegularExpression header_controls(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    qsizetype total = 0;
    for (const QJsonValue& entry : entries) {
        if (!entry.isObject()) return false;
        const QJsonObject header = entry.toObject();
        const QString name = header.value(QStringLiteral("name")).toString();
        const QString value = header.value(QStringLiteral("value")).toString();
        total += name.size() + value.size();
        if (!header_name.match(name).hasMatch() || value.size() > 2048 ||
            header_controls.match(value).hasMatch() || total > 16384) {
            return false;
        }
    }
    return true;
}

// Adaptive (video + audio) pairs share a short opaque token so the application
// can recognize both halves later (muxing is a later release).
bool group_is_valid(const QJsonValue& group_value) {
    if (group_value.isUndefined() || group_value.isNull()) return true;
    if (!group_value.isString()) return false;
    static const QRegularExpression group_pattern(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    return group_pattern.match(group_value.toString()).hasMatch();
}

// A Referer is only replayed while the download stays on the same origin, so a
// page URL is never leaked to a different host.
bool referer_is_safe(const QString& referer, const QUrl& download_url) {
    if (referer.isEmpty()) return true;
    if (!is_http_url(referer) || referer.size() > kMaxUrlLength) return false;
    const QUrl referer_url(referer);
    return referer_url.scheme() == download_url.scheme() &&
           referer_url.host().compare(download_url.host(), Qt::CaseInsensitive) == 0 &&
           referer_url.port(0) == download_url.port(0);
}

bool user_agent_is_valid(const QString& user_agent) {
    static const QRegularExpression controls(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    return user_agent.size() <= 2048 && !controls.match(user_agent).hasMatch();
}

// Rules shared by `addDownload` and `addMedia`: the URL, the structured cookies,
// the same-origin referer and the extra `x-*` headers. Both commands must
// enforce the same policy, so it lives in one place.
bool request_is_valid(const QJsonObject& data) {
    const QString url = data.value(QStringLiteral("url")).toString();
    if (!is_download_url(url) || url.size() > kMaxUrlLength) return false;
    const QUrl download_url(url);

    const QString referer = data.value(QStringLiteral("referer")).toString();
    if (!referer_is_safe(referer, download_url)) return false;
    if (!user_agent_is_valid(data.value(QStringLiteral("userAgent")).toString())) return false;
    if (data.value(QStringLiteral("filename")).toString().size() > 255) return false;
    if (!cookies_are_valid(data.value(QStringLiteral("cookies")), download_url)) return false;
    if (!headers_are_valid(data.value(QStringLiteral("headers")))) return false;
    if (!group_is_valid(data.value(QStringLiteral("group")))) return false;
    return true;
}

// Media-specific validation on top of request_is_valid(): the kind, the
// variant list and the segment list (docs/spec/08-media-download.md §5.1).
// Returns an empty string when the request may be accepted.
QString media_request_error(const QJsonObject& data) {
    if (!request_is_valid(data)) return QStringLiteral("invalid media request");

    const QString kind = data.value(QStringLiteral("kind")).toString();
    if (kind != QStringLiteral("file") && kind != QStringLiteral("hls") &&
        kind != QStringLiteral("dash") && kind != QStringLiteral("mse")) {
        return QStringLiteral("invalid media kind");
    }
    if (data.value(QStringLiteral("protected")).toBool(false)) {
        return QStringLiteral("protected media cannot be downloaded");
    }
    static const QRegularExpression controls(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    const QString quality = data.value(QStringLiteral("quality")).toString();
    if (quality.size() > 64 || controls.match(quality).hasMatch()) {
        return QStringLiteral("invalid media quality label");
    }
    for (const char* field : {"durationSec", "positionSec"}) {
        const QJsonValue value = data.value(QLatin1String(field));
        if (value.isUndefined()) continue;
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0) {
            return QStringLiteral("invalid media timing");
        }
    }

    const QJsonValue candidates = data.value(QStringLiteral("candidates"));
    if (!candidates.isUndefined()) {
        if (!candidates.isArray()) return QStringLiteral("invalid media candidate list");
        const QJsonArray entries = candidates.toArray();
        if (entries.size() > 8) return QStringLiteral("too many media candidates");
        for (const QJsonValue& entry : entries) {
            if (!entry.isObject()) return QStringLiteral("invalid media candidate");
            const QJsonObject candidate = entry.toObject();
            const QString candidate_url = candidate.value(QStringLiteral("url")).toString();
            if (!is_download_url(candidate_url) || candidate_url.size() > kMaxUrlLength) {
                return QStringLiteral("invalid media candidate URL");
            }
            const QString label = candidate.value(QStringLiteral("label")).toString();
            if (label.size() > 64 || controls.match(label).hasMatch()) {
                return QStringLiteral("invalid media candidate label");
            }
            const QJsonValue bandwidth = candidate.value(QStringLiteral("bandwidth"));
            if (!bandwidth.isUndefined() &&
                (!bandwidth.isDouble() || !std::isfinite(bandwidth.toDouble()) ||
                 bandwidth.toDouble() < 0)) {
                return QStringLiteral("invalid media candidate bandwidth");
            }
        }
    }

    const QJsonValue segments = data.value(QStringLiteral("segments"));
    if (!segments.isUndefined()) {
        if (!segments.isArray()) return QStringLiteral("invalid media segment list");
        const QJsonArray entries = segments.toArray();
        if (entries.size() > 2000) return QStringLiteral("too many media segments");
        qsizetype total = 0;
        int last_order = -1;
        for (const QJsonValue& entry : entries) {
            if (!entry.isObject()) return QStringLiteral("invalid media segment");
            const QJsonObject segment = entry.toObject();
            const QString segment_url = segment.value(QStringLiteral("url")).toString();
            if (!is_download_url(segment_url) || segment_url.size() > kMaxUrlLength) {
                return QStringLiteral("invalid media segment URL");
            }
            const QString role = segment.value(QStringLiteral("role")).toString();
            if (role != QStringLiteral("init") && role != QStringLiteral("media")) {
                return QStringLiteral("invalid media segment role");
            }
            const QJsonValue order = segment.value(QStringLiteral("order"));
            if (order.isUndefined() || !order.isDouble() || order.toDouble() < 0) {
                return QStringLiteral("invalid media segment order");
            }
            // The list has to arrive in playback order, which is what the
            // engine relies on when it appends the segments.
            if (order.toInt() < last_order) return QStringLiteral("media segments are out of order");
            last_order = order.toInt();
            total += segment_url.size();
        }
        if (total > 512 * 1024) return QStringLiteral("media segment list is too large");
    }
    return {};
}

}  // namespace

BrowserIntegration::BrowserIntegration(QObject* parent, quint16 port) : QObject(parent) {
    token_path_ = QString::fromStdString(swiftdm::BrowserToken::default_path());
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
    media_counts_.clear();
    for (QTcpSocket* socket : sockets) {
        if (!socket) continue;
        socket->disconnect(this);
        socket->abort();
        delete socket;
    }
}

bool BrowserIntegration::verify_token(const QString& candidate) const {
    const std::string expected = swiftdm::BrowserToken::load(token_path_.toStdString());
    return swiftdm::BrowserToken::verify(expected, candidate.toStdString());
}

bool BrowserIntegration::authorize(QTcpSocket* socket, const QString& origin,
                                   const QString& token) {
    if (verify_token(token)) return true;
    // No stored token yet, or the extension lost it: ask the user once. The
    // prompt is skipped in a headless run (no handler installed), which keeps
    // an unattended app from pairing silently.
    const bool paired = pairing_prompt_ ? pairing_prompt_(origin) : false;
    if (!paired) {
        qWarning() << "browser integration: refused an unpaired connection from" << origin;
        handshake_error_ = QStringLiteral("unpaired client: %1").arg(origin);
        send_close(socket, 1008);
        return false;
    }
    std::string error;
    const std::string created =
        swiftdm::BrowserToken::create(token_path_.toStdString(), &error);
    if (created.empty()) {
        qWarning() << "browser integration: cannot store the pairing token:"
                   << QString::fromStdString(error);
        handshake_error_ = QStringLiteral("cannot store the pairing token");
        send_close(socket, 1011);
        return false;
    }
    emit clientPaired(origin);
    send_json(socket, {{QStringLiteral("v"), 1},
                       {QStringLiteral("event"), QStringLiteral("pair")},
                       {QStringLiteral("data"),
                        QJsonObject{{QStringLiteral("token"),
                                     QString::fromStdString(created)}}}});
    return true;
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
            media_counts_.remove(socket);
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
        const QByteArray request_line = lines.isEmpty() ? QByteArray() : lines.first().trimmed();
        // `GET /` for the plain protocol, `GET /?t=<token>` once the extension
        // holds a pairing token.
        const QByteArray target = request_line.startsWith("GET ")
                                      ? request_line.mid(4, request_line.indexOf(" HTTP/")
                                                                  - 4)
                                      : QByteArray();
        if (!request_line.startsWith("GET ") || !target.startsWith("/") ||
            !request_line.endsWith(" HTTP/1.1")) {
            reject(QStringLiteral("unsupported request line '%1'").arg(
                QString::fromLatin1(request_line)));
            return;
        }
        const QUrl target_url(QStringLiteral("http://127.0.0.1") +
                              QString::fromLatin1(target));
        const QString token =
            QUrlQuery(target_url).queryItemValue(QStringLiteral("t"), QUrl::FullyDecoded);
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
        if (!is_allowed_extension_origin(origin) || decoded_key.size() != 16 ||
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
        // The origin checks above are compatibility checks, not authentication.
        // The pairing token decides whether this client may talk to the app.
        if (!authorize(socket, QString::fromLatin1(origin), token)) return;
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

void BrowserIntegration::send_close(QTcpSocket* socket, quint16 code) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
    // A close frame carries a 2-byte big-endian status code plus a reason.
    QByteArray payload;
    payload.append(static_cast<char>((code >> 8) & 0xff));
    payload.append(static_cast<char>(code & 0xff));
    QByteArray frame;
    frame.append(static_cast<char>(0x88));  // FIN + close
    frame.append(static_cast<char>(payload.size()));
    frame.append(payload);
    socket->write(frame);
    socket->disconnectFromHost();
}

// A page can generate thousands of segment requests, so media commands are
// rate limited per socket: 30 per minute, counted in 10 s windows.
bool BrowserIntegration::allow_media_command(QTcpSocket* socket) {
    int& count = media_counts_[socket];
    if (count >= 30) return false;
    ++count;
    return true;
}

void BrowserIntegration::set_media_config(bool overlay_enabled) {
    if (media_overlay_ == overlay_enabled) return;
    media_overlay_ = overlay_enabled;
    if (subscribed_.isEmpty()) return;
    const QJsonObject event{
        {QStringLiteral("v"), 1},
        {QStringLiteral("event"), QStringLiteral("config")},
        {QStringLiteral("data"), QJsonObject{{QStringLiteral("mediaOverlay"), media_overlay_}}}};
    for (QTcpSocket* socket : std::as_const(subscribed_)) {
        send_json(socket, event);
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
        // A freshly connected worker learns the overlay switch right away, so
        // the button state is correct before the first page loads.
        result.insert(QStringLiteral("config"),
                      QJsonObject{{QStringLiteral("mediaOverlay"), media_overlay_}});
    } else if (command == QStringLiteral("showPanel")) {
        if (!data.value(QStringLiteral("visible")).isBool()) {
            ok = false;
            error = QStringLiteral("visible must be boolean");
        } else {
            emit showPanelRequested(data.value(QStringLiteral("visible")).toBool());
        }
    } else if (command == QStringLiteral("addDownload")) {
        const QString url = data.value(QStringLiteral("url")).toString();
        const QString method = data.value(QStringLiteral("method")).toString().toUpper();
        const QString post_data = data.value(QStringLiteral("postData")).toString();
        const QString post_encoding = data.value(QStringLiteral("postDataEncoding")).toString();

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
        if (!request_is_valid(data) || !method_valid || !post_valid) {
            ok = false;
            error = QStringLiteral("invalid download request");
        } else {
            // The app shows a modal confirmation window while handling this
            // signal, which blocks the event loop. Emitting directly would delay
            // the reply past the extension's 5 s command timeout, and its retry
            // would then deliver the same capture again (duplicate download
            // prompts). Queue the delivery so the reply is written first.
            QMetaObject::invokeMethod(
                this, [this, data] { emit downloadRequested(data); },
                Qt::QueuedConnection);
        }
    } else if (command == QStringLiteral("addMedia")) {
        // Media handoff (docs/spec/08-media-download.md §5.1). The same request
        // rules as addDownload apply, plus the media-specific fields below.
        const QString error_text = media_request_error(data);
        if (!error_text.isEmpty()) {
            ok = false;
            error = error_text;
        } else if (!allow_media_command(socket)) {
            ok = false;
            error = QStringLiteral("too many media requests; try again later");
        } else {
            result.insert(QStringLiteral("accepted"), true);
            // Same reason as addDownload: the reply must be written before the
            // confirmation window blocks the event loop.
            QMetaObject::invokeMethod(
                this, [this, data] { emit mediaRequested(data); },
                Qt::QueuedConnection);
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
