#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QPointer>

#include <functional>

class QTcpSocket;
class QTimer;

namespace swiftdm::ui {

// Loopback-only WebSocket endpoint for the SwiftDM browser extensions
// (Chromium and Firefox).
//
// Besides the link/page handoff (`addDownload`) this endpoint accepts a media
// handoff (`addMedia`, docs/spec/08-media-download.md §5): the extension sends
// metadata for a stream that is playing in the browser and the application
// re-issues the request itself.
//
// The socket is bound to 127.0.0.1, which keeps it off the network, but any
// local process could still claim to be the extension. A pairing token closes
// that gap: the first connection from an unknown client asks the user, and the
// answer is remembered in `$XDG_CONFIG_HOME/swiftdm/browser-token`.
class BrowserIntegration : public QObject {
    Q_OBJECT
  public:
    explicit BrowserIntegration(QObject* parent = nullptr, quint16 port = 10007);
    ~BrowserIntegration() override;

    bool isListening() const { return server_.isListening(); }
    quint16 port() const { return server_.serverPort(); }
    QString lastError() const { return error_; }
    QString lastHandshakeError() const { return handshake_error_; }

    // Broadcasts an app-status event to every subscribed extension socket.
    // `waiting` is true while the application has no active downloads.
    void set_status(bool waiting, int active_downloads);

    // Pushes the media-overlay switch to the extensions so the in-page button
    // appears or disappears without reloading a page.
    void set_media_config(bool overlay_enabled);

    // Pairing prompt: called when an unpaired client connects. Returning true
    // lets the connection continue and stores a new token; false closes it.
    // The default refuses, so a headless run never pairs silently.
    using PairingPrompt = std::function<bool(const QString& origin)>;
    void set_pairing_prompt(PairingPrompt prompt) { pairing_prompt_ = std::move(prompt); }
    // Token file location. Defaults to BrowserToken::default_path().
    void set_token_path(const QString& path) { token_path_ = path; }

  signals:
    void downloadRequested(const QJsonObject& data);
    void mediaRequested(const QJsonObject& data);
    void showPanelRequested(bool visible);
    void serverError(const QString& error);
    void handshakeRejected(const QString& reason);
    // Emitted after the user approved a new client; carries the origin shown in
    // the prompt so the window can report it.
    void clientPaired(const QString& origin);

  private:
    void accept_connections();
    void read_socket(QTcpSocket* socket);
    void process_frame(QTcpSocket* socket, QByteArray& buffer);
    void send_json(QTcpSocket* socket, const QJsonObject& response);
    void send_close(QTcpSocket* socket, quint16 code);
    QJsonObject handle_message(QTcpSocket* socket, const QJsonObject& message);
    // Returns true when the socket may continue (valid token, or the user
    // approved pairing). Closes the socket and returns false otherwise.
    bool authorize(QTcpSocket* socket, const QString& origin, const QString& token);
    bool verify_token(const QString& candidate) const;
    // Per-socket flood limit for `addMedia`.
    bool allow_media_command(QTcpSocket* socket);

    QTcpServer server_;
    QString error_;
    QString handshake_error_;
    QString token_path_;
    PairingPrompt pairing_prompt_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QHash<QTcpSocket*, bool> handshaken_;
    QHash<QTcpSocket*, QPointer<QTimer>> handshake_timers_;
    QSet<QTcpSocket*> subscribed_;
    // Per-socket count of accepted addMedia commands, for the flood limit.
    QHash<QTcpSocket*, int> media_counts_;
    bool waiting_ = true;
    int active_downloads_ = 0;
    bool media_overlay_ = true;
};

}  // namespace swiftdm::ui
