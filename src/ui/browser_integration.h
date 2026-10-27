#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QPointer>

class QTcpSocket;
class QTimer;

namespace swiftdm::ui {

// Loopback-only WebSocket endpoint for the SwiftDM Chromium extension.
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

  signals:
    void downloadRequested(const QJsonObject& data);
    void showPanelRequested(bool visible);
    void serverError(const QString& error);
    void handshakeRejected(const QString& reason);

  private:
    void accept_connections();
    void read_socket(QTcpSocket* socket);
    void process_frame(QTcpSocket* socket, QByteArray& buffer);
    void send_json(QTcpSocket* socket, const QJsonObject& response);
    QJsonObject handle_message(QTcpSocket* socket, const QJsonObject& message);

    QTcpServer server_;
    QString error_;
    QString handshake_error_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QHash<QTcpSocket*, bool> handshaken_;
    QHash<QTcpSocket*, QPointer<QTimer>> handshake_timers_;
    QSet<QTcpSocket*> subscribed_;
    bool waiting_ = true;
    int active_downloads_ = 0;
};

}  // namespace swiftdm::ui
