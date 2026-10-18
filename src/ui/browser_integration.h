#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

namespace swiftdm::ui {

// Loopback-only WebSocket endpoint for the SwiftDM Chromium extension.
class BrowserIntegration : public QObject {
    Q_OBJECT
  public:
    explicit BrowserIntegration(QObject* parent = nullptr, quint16 port = 10007);
    ~BrowserIntegration() override;

    bool isListening() const { return server_.isListening(); }
    quint16 port() const { return server_.serverPort(); }

  signals:
    void downloadRequested(const QJsonObject& data);
    void showPanelRequested(bool visible);
    void serverError(const QString& error);

  private:
    void accept_connections();
    void read_socket(QTcpSocket* socket);
    void process_frame(QTcpSocket* socket, QByteArray& buffer);
    void send_json(QTcpSocket* socket, const QJsonObject& response);
    QJsonObject handle_message(const QJsonObject& message);

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QHash<QTcpSocket*, bool> handshaken_;
};

}  // namespace swiftdm::ui
