// Headless check of the single-instance guard: the lock is exclusive, the
// owner record names the running role, a stale lock is taken over, and the
// GUI socket forwards a "show the main window" request. No window is shown.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "core/instance_lock.h"
#include "ui/single_instance.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Processes events until the predicate holds or the timeout expires; the socket
// request is answered from the guard's event loop.
template <typename Predicate>
bool pump_until(Predicate predicate, int timeout_ms = 1000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return predicate();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const std::string config = "/tmp/swiftdm-test-single-instance/swiftdm.ini";
    const std::string name = swiftdm::InstanceLock::name_for_config(config);
    const QString guard_name = swiftdm::ui::SingleInstanceGuard::name_for_config(config);

    check(name.rfind("swiftdm-", 0) == 0 && name.size() == 8 + 16,
          "the lock name is 'swiftdm-' plus a 64-bit hash");
    check(name != swiftdm::InstanceLock::name_for_config("/tmp/other/swiftdm.ini"),
          "a different config path gives a different lock name");

    // 1. The first process takes the lock and records its role.
    swiftdm::InstanceLock first;
    std::string error;
    check(first.acquire(swiftdm::InstanceRole::Gui, name, &error),
          "the first acquire() succeeds");
    check(first.held(), "the first lock reports itself as held");

    const swiftdm::InstanceOwner owner = first.owner();
    check(owner.known, "the lock file is readable by its owner");
    check(owner.role == swiftdm::InstanceRole::Gui, "the owner record stores the GUI role");
    check(owner.pid > 0, "the owner record stores a process id");

    // 2. A second process is refused and sees which run holds the lock.
    swiftdm::InstanceLock second;
    check(!second.acquire(swiftdm::InstanceRole::Cli, name, &error),
          "a second acquire() on the same name fails");
    check(error.empty(), "a live owner is reported as contention, not as an I/O error");
    check(!second.held(), "the refused lock does not report itself as held");
    check(second.owner().known && second.owner().role == swiftdm::InstanceRole::Gui,
          "the refused start can name the role of the running instance");

    // 3. A CLI owner is recorded as such, so the GUI can explain the refusal.
    first.release();
    check(!first.held(), "release() drops the lock");
    check(first.acquire(swiftdm::InstanceRole::Cli, name, &error),
          "the lock can be taken again after a release");
    check(first.owner().role == swiftdm::InstanceRole::Cli,
          "a CLI run records the CLI role");
    first.release();

    // 4. A stale lock file (written by a dead run, no lock held) is taken over
    //    instead of blocking the start.
    {
        swiftdm::InstanceLock crashed;
        check(crashed.acquire(swiftdm::InstanceRole::Gui, name, &error),
              "the lock is free after the previous release");
        // Simulate the crash: the file stays on disk without a live lock.
        crashed.release();
        // release() removes the file, so write a leftover record to model the
        // file a killed process leaves behind.
        std::FILE* stale = std::fopen(swiftdm::InstanceLock::lock_path(name).c_str(), "wb");
        check(stale != nullptr, "a stale lock file can be created");
        if (stale) {
            std::fputs("gui 999999\n", stale);
            std::fclose(stale);
        }
        swiftdm::InstanceLock restarted;
        check(restarted.acquire(swiftdm::InstanceRole::Gui, name, &error),
              "a stale lock file does not block the next start");
        check(restarted.owner().pid != 999999,
              "taking over a stale lock rewrites the owner record");
        restarted.release();
        check(!QFile::exists(QString::fromStdString(swiftdm::InstanceLock::lock_path(name))),
              "release() removes the lock file");
    }

    // 5. The GUI guard listens and a second GUI start makes it show its window.
    {
        swiftdm::ui::SingleInstanceGuard gui(true, guard_name);
        check(gui.acquire(&error), "the GUI guard takes the lock");
        check(gui.held(), "the GUI guard reports itself as the owner");

        int show_requests = 0;
        QObject::connect(&gui, &swiftdm::ui::SingleInstanceGuard::showRequested,
                         [&show_requests] { ++show_requests; });

        swiftdm::ui::SingleInstanceGuard shortcut(true, guard_name);
        check(!shortcut.acquire(&error), "a second GUI start is refused");
        check(shortcut.has_owner() && shortcut.owner() == swiftdm::InstanceRole::Gui,
              "the second GUI start sees a GUI owner");
        check(pump_until([&show_requests] { return show_requests > 0; }),
              "the second GUI start asks the running instance to show its window");

        // A login start (--autostart) must never pop the window the user hid.
        show_requests = 0;
        swiftdm::ui::SingleInstanceGuard autostart(false, guard_name);
        check(!autostart.acquire(&error), "a second autostart start is refused");
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 200) QCoreApplication::processEvents();
        check(show_requests == 0,
              "an autostart start does not bring the window forward");
    }

#ifndef _WIN32
    // 6. A socket file left behind by a crashed GUI (bound, then closed without
    //    unlinking - what a killed process leaves) does not stop the next GUI
    //    start from owning the instance.
    {
        const std::string endpoint = swiftdm::InstanceLock::endpoint_path(name);
        ::unlink(endpoint.c_str());
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(fd >= 0, "a raw unix socket can be created");
        if (fd >= 0) {
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", endpoint.c_str());
            check(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0,
                  "a stale socket file can be bound");
            ::close(fd);  // leaves the file behind, like a killed process
            check(QFile::exists(QString::fromStdString(endpoint)),
                  "the stale socket file is on disk");

            swiftdm::ui::SingleInstanceGuard revived(true, guard_name);
            check(revived.acquire(&error),
                  "a stale socket does not block the next start");
        }
    }
#endif

    // 7. Once the owner is gone the lock is free again and the socket is
    //    cleaned up, so a later start is not blocked by either one.
    {
        swiftdm::InstanceLock after;
        check(after.acquire(swiftdm::InstanceRole::Gui, name, &error),
              "the lock is free after the GUI guard is destroyed");
        after.release();
        const QString endpoint = QString::fromStdString(
            swiftdm::InstanceLock::endpoint_path(name));
        check(!QFile::exists(endpoint), "the GUI guard removed its socket on exit");
    }

    if (failures == 0) {
        std::printf("single_instance: all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "single_instance: %d failure(s)\n", failures);
    return 1;
}
