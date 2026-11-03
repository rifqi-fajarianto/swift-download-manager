#include "ui/single_instance.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

#include <string>

#include "store/settings.h"

namespace swiftdm::ui {
namespace {

// A second start must not hold a connection open when it never sends a request.
constexpr int kRequestTimeoutMs = 1000;
// The request is one short line.
constexpr char kShowRequest[] = "swiftdm/1 show\n";
constexpr char kShowAck[] = "ok\n";

}  // namespace

SingleInstanceGuard::SingleInstanceGuard(bool request_show, QObject* parent)
    : SingleInstanceGuard(request_show, default_name(), parent) {}

SingleInstanceGuard::SingleInstanceGuard(bool request_show, const QString& name,
                                        QObject* parent)
    : QObject(parent),
      request_show_(request_show),
      name_(name.isEmpty() ? default_name() : name) {}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (pending_) {
        pending_->disconnect(this);
        pending_->abort();
        delete pending_;
        pending_ = nullptr;
    }
    if (server_) {
        server_->close();
        delete server_;
        server_ = nullptr;
        QLocalServer::removeServer(endpoint());
    }
}

QString SingleInstanceGuard::name_for_config(const std::string& config_path) {
    return QString::fromStdString(swiftdm::InstanceLock::name_for_config(config_path));
}

QString SingleInstanceGuard::default_name() {
    return name_for_config(swiftdm::default_config_path());
}

bool SingleInstanceGuard::acquire(std::string* error) {
    if (lock_.held()) return true;

    if (!lock_.acquire(swiftdm::InstanceRole::Gui, name_.toStdString(), error)) {
        if (error && !error->empty()) return false;  // I/O failure, not a live owner
        // Another process holds the lock. The lock file records its role, which
        // is what decides whether this start should bring a window forward.
        const swiftdm::InstanceOwner info = lock_.owner_wait(300);
        owner_known_ = info.known;
        owner_role_ = info.role;
        // A second `swiftdm get` is none of the GUI's business, and the login
        // entry stays quiet so a repeated autostart cannot pop the window.
        if (request_show_ && (!info.known || info.role == swiftdm::InstanceRole::Gui)) {
            send_show_request();
        }
        return false;
    }

    start_listening();
    return true;
}

QString SingleInstanceGuard::endpoint() const {
    return QString::fromStdString(
        swiftdm::InstanceLock::endpoint_path(name_.toStdString()));
}

void SingleInstanceGuard::start_listening() {
    if (server_) return;
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this,
            &SingleInstanceGuard::accept_connections);
    if (server_->listen(endpoint())) return;

    // A socket left behind by a crashed run blocks the name even though the
    // lock was free. Qt only clears a socket it can prove is dead, so remove it
    // once and retry.
    QLocalServer::removeServer(endpoint());
    if (!server_->listen(endpoint())) {
        qWarning() << "single instance: cannot listen on" << endpoint() << "-"
                   << server_->errorString();
    }
}

void SingleInstanceGuard::accept_connections() {
    while (server_ && server_->hasPendingConnections()) {
        QLocalSocket* socket = server_->nextPendingConnection();
        if (!socket) continue;
        if (pending_) {
            // One request at a time is enough; a burst means something else is
            // talking to the socket.
            socket->abort();
            socket->deleteLater();
            continue;
        }
        pending_ = socket;
        pending_buffer_.clear();
        // A client that connects and never sends its line must not block every
        // later start for the rest of the session.
        auto* timer = new QTimer(socket);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, socket] {
            if (pending_ != socket) return;
            socket->disconnect(this);
            pending_ = nullptr;
            pending_buffer_.clear();
            socket->abort();
            socket->deleteLater();
        });
        timer->start(kRequestTimeoutMs);
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { read_request(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (pending_ != socket) return;
            pending_ = nullptr;
            pending_buffer_.clear();
            socket->deleteLater();
        });
        read_request(socket);
    }
}

void SingleInstanceGuard::read_request(QLocalSocket* socket) {
    if (!socket || socket != pending_) return;
    pending_buffer_.append(socket->readAll());
    if (pending_buffer_.size() > 128) {
        socket->disconnect(this);
        pending_ = nullptr;
        pending_buffer_.clear();
        socket->abort();
        socket->deleteLater();
        return;
    }
    if (!pending_buffer_.contains('\n')) return;
    finish_request(socket, pending_buffer_);
}

void SingleInstanceGuard::finish_request(QLocalSocket* socket, const QByteArray& line) {
    // Only the GUI owns the socket, so a valid request always means "come to the
    // front"; the payload stays explicit for future request kinds.
    if (QString::fromUtf8(line).trimmed() == QLatin1String("swiftdm/1 show")) {
        emit showRequested();
    }
    socket->write(kShowAck);
    socket->flush();
    socket->disconnectFromServer();
    socket->disconnect(this);
    if (pending_ == socket) {
        pending_ = nullptr;
        pending_buffer_.clear();
    }
    socket->deleteLater();
}

bool SingleInstanceGuard::send_show_request() const {
    QLocalSocket socket;
    socket.connectToServer(endpoint());
    if (!socket.waitForConnected(kRequestTimeoutMs)) return false;
    socket.write(kShowRequest);
    socket.flush();
    // The return value is deliberately ignored: a Windows named pipe reports
    // false here even though the request was delivered (bytesToWrite() stays
    // non-zero until the peer reads), and aborting on that would drop the
    // request. The acknowledgement below is the real delivery proof.
    socket.waitForBytesWritten(kRequestTimeoutMs);

    // Wait for the acknowledgement so the owner has handled the request before
    // this process exits.
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray response;
    while (elapsed.elapsed() < kRequestTimeoutMs) {
        if (socket.bytesAvailable() > 0) response.append(socket.readAll());
        if (response.contains('\n')) break;
        if (socket.state() != QLocalSocket::ConnectedState) break;
        // The owner answers from its event loop; keep ours responsive so the
        // wait ends as soon as the reply arrives.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    socket.abort();
    return response.startsWith(kShowAck);
}

}  // namespace swiftdm::ui
