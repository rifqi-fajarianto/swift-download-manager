#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTcpSocket>
#include <QThread>

#include <cstdio>

#include "ui/browser_integration.h"

namespace {

int failures = 0;

template <typename Predicate>
bool pump_until(Predicate predicate, int timeout_ms = 1000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return predicate();
}

void check(bool condition, const char* description) {
    if (!condition) {
        std::printf("FAIL %s\n", description);
        ++failures;
    }
}

QByteArray masked_frame(const QByteArray& payload) {
    const QByteArray mask = QByteArray::fromHex("12345678");
    QByteArray frame;
    frame.append(static_cast<char>(0x81));
    if (payload.size() < 126) {
        frame.append(static_cast<char>(0x80 | payload.size()));
    } else {
        frame.append(static_cast<char>(0xfe));
        frame.append(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(payload.size() & 0xff));
    }
    frame.append(mask);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        frame.append(static_cast<char>(payload.at(i) ^ mask.at(i % 4)));
    }
    return frame;
}

QByteArray parse_server_frame(QByteArray& bytes) {
    if (bytes.size() < 2) return {};
    qsizetype header = 2;
    quint64 length = static_cast<quint8>(bytes.at(1)) & 0x7f;
    if (length == 126) {
        if (bytes.size() < 4) return {};
        length = (static_cast<quint64>(static_cast<quint8>(bytes.at(2))) << 8) |
                 static_cast<quint8>(bytes.at(3));
        header = 4;
    } else if (length == 127) {
        if (bytes.size() < 10) return {};
        length = 0;
        for (qsizetype i = 2; i < 10; ++i) {
            length = (length << 8) | static_cast<quint8>(bytes.at(i));
        }
        header = 10;
    }
    if (bytes.size() < header + static_cast<qsizetype>(length)) return {};
    QByteArray payload = bytes.mid(header, static_cast<qsizetype>(length));
    bytes.remove(0, header + static_cast<qsizetype>(length));
    return payload;
}

bool connect_with_origin(QTcpSocket& socket, quint16 port, const QByteArray& origin) {
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(1000)) return false;
    const QByteArray key = "dGhlIHNhbXBsZSBub25jZQ==";
    const QByteArray request =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: swiftdm.v1\r\n"
        "Origin: " + origin + "\r\n\r\n";
    socket.write(request);
    if (!pump_until([&] { return socket.bytesToWrite() == 0; })) return false;
    QByteArray response = socket.readAll();
    if (!response.contains("\r\n\r\n")) {
        pump_until([&] {
            response.append(socket.readAll());
            return response.contains("\r\n\r\n");
        });
    }
    return response.startsWith("HTTP/1.1 101 Switching Protocols") &&
           response.contains("Sec-WebSocket-Protocol: swiftdm.v1");
}

bool connect_extension(QTcpSocket& socket, quint16 port) {
    return connect_with_origin(socket, port,
                               "chrome-extension://hagednkleljjijmjonpeiokknhngknbb");
}

QJsonObject command(QTcpSocket& socket, const QString& cmd, const QJsonObject& data,
                    const QString& id) {
    const QJsonObject request{{QStringLiteral("v"), 1},
                              {QStringLiteral("cmd"), cmd},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("data"), data}};
    socket.write(masked_frame(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    if (!pump_until([&] { return socket.bytesToWrite() == 0; })) return {};
    QByteArray buffer;
    for (int i = 0; i < 5; ++i) {
        if (!pump_until([&] { return socket.bytesAvailable() > 0; })) return {};
        buffer.append(socket.readAll());
        const QByteArray frame = parse_server_frame(buffer);
        if (!frame.isEmpty()) {
            return QJsonDocument::fromJson(frame).object();
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    swiftdm::ui::BrowserIntegration server(nullptr, 0);
    check(server.isListening(), "loopback WebSocket server starts on ephemeral port");

    QTcpSocket socket;
    check(connect_extension(socket, server.port()), "Chrome extension WebSocket handshake succeeds");
    if (socket.state() == QAbstractSocket::ConnectedState) {
        int downloads_received = 0;
        int panel_requests = 0;
        QObject::connect(&server, &swiftdm::ui::BrowserIntegration::downloadRequested,
                         &app, [&](const QJsonObject&) { ++downloads_received; });
        QObject::connect(&server, &swiftdm::ui::BrowserIntegration::showPanelRequested,
                         &app, [&](bool visible) { if (visible) ++panel_requests; });
        const QJsonObject ping =
            command(socket, QStringLiteral("ping"), {}, QStringLiteral("p1"));
        check(ping.value(QStringLiteral("ok")).toBool(), "ping is accepted");
        check(ping.value(QStringLiteral("id")).toString() == QStringLiteral("p1"),
              "response echoes request id");
        const QJsonObject version = ping.value(QStringLiteral("data")).toObject();
        check(version.value(QStringLiteral("pong")).toBool(), "ping returns pong");

        const QJsonObject get_version = command(
            socket, QStringLiteral("getVersion"), {}, QStringLiteral("version-1"));
        check(get_version.value(QStringLiteral("ok")).toBool(), "getVersion is accepted");
        check(!get_version.value(QStringLiteral("data")).toObject()
                   .value(QStringLiteral("version")).toString().isEmpty(),
              "getVersion returns app version");

        const QJsonObject show_panel = command(
            socket, QStringLiteral("showPanel"),
            {{QStringLiteral("visible"), true}}, QStringLiteral("panel-1"));
        check(show_panel.value(QStringLiteral("ok")).toBool(), "showPanel is accepted");
        check(panel_requests == 1, "showPanel signal is emitted");

        const QJsonObject bad_url = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("file:///etc/passwd")}},
            QStringLiteral("bad-url"));
        check(!bad_url.value(QStringLiteral("ok")).toBool(), "non-http download URL rejected");

        const QJsonObject bad_cookie = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/file")},
             {QStringLiteral("cookies"), QJsonArray{
                  QJsonObject{{QStringLiteral("domain"), QStringLiteral("evil.example")},
                              {QStringLiteral("path"), QStringLiteral("/")},
                              {QStringLiteral("name"), QStringLiteral("sid")},
                              {QStringLiteral("value"), QStringLiteral("secret")},
                              {QStringLiteral("secure"), true},
                              {QStringLiteral("expires"), 0}}}}},
            QStringLiteral("bad-cookie"));
        check(!bad_cookie.value(QStringLiteral("ok")).toBool(),
              "cookie scoped to another host rejected");

        const QJsonObject ok_download = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/file")}},
            QStringLiteral("download-1"));
        check(ok_download.value(QStringLiteral("ok")).toBool(), "valid HTTP download accepted");
        check(downloads_received == 1, "valid download signal is emitted");

        const QJsonObject legacy_cookies = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/file")},
             {QStringLiteral("cookies"), QString()}},
            QStringLiteral("download-empty-cookies"));
        check(legacy_cookies.value(QStringLiteral("ok")).toBool(),
              "empty legacy cookie string is treated as no cookies");
        check(downloads_received == 2, "empty-cookie download signal is emitted");

        const QJsonObject raw_cookie = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/file")},
             {QStringLiteral("cookies"), QStringLiteral("sid=secret")}},
            QStringLiteral("download-raw-cookie"));
        check(!raw_cookie.value(QStringLiteral("ok")).toBool(),
              "unscoped raw cookie string is rejected");

        const QJsonObject post_download = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/submit")},
             {QStringLiteral("method"), QStringLiteral("POST")},
             {QStringLiteral("postData"), QStringLiteral("q=swiftdm&page=1")},
             {QStringLiteral("postDataEncoding"), QStringLiteral("utf8")},
             {QStringLiteral("headers"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("name"), QStringLiteral("x-api-key")},
                  {QStringLiteral("value"), QStringLiteral("secret")}}}}},
            QStringLiteral("download-post"));
        check(post_download.value(QStringLiteral("ok")).toBool(),
              "POST download with an x-* header is accepted");
        check(downloads_received == 3, "POST download signal is emitted");

        const QJsonObject bad_header = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/submit")},
             {QStringLiteral("headers"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("name"), QStringLiteral("Cookie")},
                  {QStringLiteral("value"), QStringLiteral("sid=secret")}}}}},
            QStringLiteral("download-bad-header"));
        check(!bad_header.value(QStringLiteral("ok")).toBool(),
              "non-x-* request headers are rejected");

        const QJsonObject paired_download = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/video.mp4")},
             {QStringLiteral("group"), QStringLiteral("pair-1f2e3d4c")}},
            QStringLiteral("download-paired"));
        check(paired_download.value(QStringLiteral("ok")).toBool(),
              "adaptive pair token is accepted");
        check(downloads_received == 4, "paired download signal is emitted");

        const QJsonObject bad_group = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/video.mp4")},
             {QStringLiteral("group"), QStringLiteral("pair token with spaces")}},
            QStringLiteral("download-bad-group"));
        check(!bad_group.value(QStringLiteral("ok")).toBool(),
              "malformed pair token is rejected");

        const QJsonObject ftp_download = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("ftp://example.com/file.bin")}},
            QStringLiteral("download-ftp"));
        check(ftp_download.value(QStringLiteral("ok")).toBool(), "FTP download URL accepted");
        check(downloads_received == 5, "FTP download signal is emitted");

        const QJsonObject bad_post = command(
            socket, QStringLiteral("addDownload"),
            {{QStringLiteral("url"), QStringLiteral("https://example.com/submit")},
             {QStringLiteral("method"), QStringLiteral("POST")}},
            QStringLiteral("download-post-empty"));
        check(!bad_post.value(QStringLiteral("ok")).toBool(), "POST without a body is rejected");

        const QJsonObject subscribe =
            command(socket, QStringLiteral("subscribe"), {}, QStringLiteral("subscribe-1"));
        check(subscribe.value(QStringLiteral("ok")).toBool(), "subscribe is accepted");
        const QJsonObject subscribed = subscribe.value(QStringLiteral("data")).toObject();
        check(subscribed.value(QStringLiteral("subscribed")).toBool() &&
                  subscribed.value(QStringLiteral("status"))
                      .toObject()
                      .value(QStringLiteral("waiting"))
                      .toBool(),
              "subscribe returns the current app status");

        server.set_status(false, 3);
        QByteArray push_buffer;
        QByteArray pushed;
        for (int i = 0; i < 20 && pushed.isEmpty(); ++i) {
            if (!pump_until([&] {
                    push_buffer.append(socket.readAll());
                    return !push_buffer.isEmpty();
                })) {
                break;
            }
            pushed = parse_server_frame(push_buffer);
        }
        const QJsonObject status_event = QJsonDocument::fromJson(pushed).object();
        check(status_event.value(QStringLiteral("event")).toString() == QStringLiteral("status") &&
                  status_event.value(QStringLiteral("data"))
                          .toObject()
                          .value(QStringLiteral("active"))
                          .toInt() == 3,
              "subscribed sockets receive status events");
        server.set_status(true, 0);
    }

    // Reloading an unpacked extension from another path can change its ID. A
    // different well-formed Chromium extension origin must still connect.
    QTcpSocket other_extension;
    check(connect_with_origin(other_extension, server.port(),
                              "chrome-extension://abcdefghijklmnopabcdefghijklmnop"),
          "a different valid extension ID is accepted");
    if (other_extension.state() == QAbstractSocket::ConnectedState) {
        const QJsonObject ping =
            command(other_extension, QStringLiteral("ping"), {}, QStringLiteral("p2"));
        check(ping.value(QStringLiteral("ok")).toBool(),
              "ping works from a different extension ID");
    }

    // Edge displays extension pages as extensions:// in DevTools and can use
    // that scheme in the WebSocket Origin header.
    for (const QByteArray& origin : {QByteArray("extensions://abcdefghijklmnopabcdefghijklmnop"),
                                     QByteArray("extension://abcdefghijklmnopabcdefghijklmnop")}) {
        QTcpSocket edge;
        check(connect_with_origin(edge, server.port(), origin),
              "Edge extension origin is accepted");
        if (edge.state() == QAbstractSocket::ConnectedState) {
            const QJsonObject ping =
                command(edge, QStringLiteral("ping"), {}, QStringLiteral("edge-ping"));
            check(ping.value(QStringLiteral("ok")).toBool(),
                  "ping works from an Edge extension origin");
        }
    }

    // Each extension command opens a short-lived socket and closes it. Repeat
    // the cycle to catch server-side lifecycle/cleanup regressions.
    for (int i = 0; i < 5; ++i) {
        QTcpSocket repeated;
        const bool connected = connect_extension(repeated, server.port());
        check(connected, "repeated short-lived connection is accepted");
        if (!connected) break;
        const QJsonObject ping =
            command(repeated, QStringLiteral("ping"), {}, QStringLiteral("repeat-%1").arg(i));
        check(ping.value(QStringLiteral("ok")).toBool(), "repeated ping succeeds");
    }

    QString rejected_reason;
    QObject::connect(&server, &swiftdm::ui::BrowserIntegration::handshakeRejected,
                     &app, [&](const QString& reason) { rejected_reason = reason; });

    QTcpSocket malformed;
    malformed.connectToHost(QHostAddress::LocalHost, server.port());
    if (malformed.waitForConnected(1000)) {
        const QByteArray key = "dGhlIHNhbXBsZSBub25jZQ==";
        malformed.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: " + key +
                        "\r\nSec-WebSocket-Version: 13\r\n"
                        "Sec-WebSocket-Protocol: swiftdm.v1\r\n"
                        "Origin: chrome-extension://not-a-valid-extension-id\r\n\r\n");
        pump_until([&] { return malformed.bytesToWrite() == 0; });
        if (pump_until([&] {
                return malformed.bytesAvailable() > 0 ||
                       malformed.state() != QAbstractSocket::ConnectedState;
            })) {
            const QByteArray response = malformed.readAll();
            check(!response.startsWith("HTTP/1.1 101"),
                  "malformed extension origin is rejected");
            check(rejected_reason.contains(QStringLiteral("not-a-valid-extension-id")),
                  "rejection reason includes the origin");
            check(!server.lastHandshakeError().isEmpty(),
                  "last handshake error is exposed to the UI");
        } else {
            check(false, "malformed extension origin is rejected");
        }
    } else {
        check(false, "malformed-origin client can connect for handshake test");
    }

    QTcpSocket untrusted;
    untrusted.connectToHost(QHostAddress::LocalHost, server.port());
    if (untrusted.waitForConnected(1000)) {
        const QByteArray key = "dGhlIHNhbXBsZSBub25jZQ==";
        untrusted.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: " + key +
                        "\r\nSec-WebSocket-Version: 13\r\n"
                        "Sec-WebSocket-Protocol: swiftdm.v1\r\n"
                        "Origin: https://example.com\r\n\r\n");
        pump_until([&] { return untrusted.bytesToWrite() == 0; });
        if (pump_until([&] {
                return untrusted.bytesAvailable() > 0 ||
                       untrusted.state() != QAbstractSocket::ConnectedState;
            })) {
            const QByteArray response = untrusted.readAll();
            check(!response.startsWith("HTTP/1.1 101"), "non-extension browser origin rejected");
        } else {
            check(false, "non-extension browser origin rejected");
        }
    } else {
        check(false, "untrusted client can connect to loopback for handshake test");
    }

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("Browser protocol tests passed\n");
    return failures == 0 ? 0 : 1;
}
