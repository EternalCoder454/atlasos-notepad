// Stands in for a running atlas-notepad: a real KDBusService::Unique under the
// old app ID (net.eterneon.atlas.notepad), so it serves org.kde.KDBusService
// at the path KDE gives it. Writes the arguments of the launch it is handed to
// the file named by argv[1], then quits.
//   legacy_peer <file>
#include <KDBusService>

#include <QCoreApplication>
#include <QFile>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationDomain(QStringLiteral("atlas.eterneon.net"));
    QCoreApplication::setApplicationName(QStringLiteral("notepad"));
    if (argc < 2) {
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    KDBusService service(KDBusService::Unique);
    QObject::connect(&service, &KDBusService::activateRequested, &app, [out](const QStringList &arguments, const QString &) {
        QFile f(out);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(arguments.join(QLatin1Char('\n')).toUtf8());
        }
        QCoreApplication::quit();
    });
    return app.exec();
}
