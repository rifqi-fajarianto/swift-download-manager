#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <string>

#include "core/instance_lock.h"

class QLocalServer;
class QLocalSocket;

namespace swiftdm::ui {

// GUI side of the single-instance guard: it holds the shared InstanceLock and
// listens on the local socket that carries the "show the main window" request,
// so a second shortcut or terminal start brings the running window forward
// instead of opening another one.
class SingleInstanceGuard : public QObject {
    Q_OBJECT
  public:
    // `request_show` marks a start that should bring the running GUI forward
    // (shortcut, `swiftdm` from a terminal, `--gui`). The login entry passes
    // false so a second autostart never pops the window the user hid.
    explicit SingleInstanceGuard(bool request_show, QObject* parent = nullptr);
    // `name` overrides the default lock name; tests use it to stay independent
    // of the running application.
    SingleInstanceGuard(bool request_show, const QString& name, QObject* parent = nullptr);
    ~SingleInstanceGuard() override;

    // Default lock name, derived from the default config path.
    static QString default_name();
    static QString name_for_config(const std::string& config_path);

    // True when this process now owns the instance. Otherwise owner() describes
    // the run that is in the way and, when both runs are the GUI and this start
    // asked for it, that run has already been told to show its window.
    bool acquire(std::string* error = nullptr);
    bool held() const { return lock_.held(); }
    bool has_owner() const { return owner_known_; }
    InstanceRole owner() const { return owner_role_; }

  signals:
    // A second GUI start asked for the main window.
    void showRequested();

  private:
    // Socket path on POSIX, pipe name on Windows.
    QString endpoint() const;
    void start_listening();
    void accept_connections();
    void read_request(QLocalSocket* socket);
    void finish_request(QLocalSocket* socket, const QByteArray& line);
    bool send_show_request() const;

    bool request_show_ = false;
    QString name_;
    swiftdm::InstanceLock lock_;
    QLocalServer* server_ = nullptr;
    QLocalSocket* pending_ = nullptr;
    QByteArray pending_buffer_;
    InstanceRole owner_role_ = InstanceRole::Gui;
    bool owner_known_ = false;
};

}  // namespace swiftdm::ui
