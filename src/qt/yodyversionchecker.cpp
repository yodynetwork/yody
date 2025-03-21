#include <qt/yodyversionchecker.h>
#include <clientversion.h>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QTimer>

#define paternVersion "yody-([0-9]+\\.)?([0-9]+\\.)?([0-9]+)-"

YodyVersionChecker::YodyVersionChecker(QObject *parent) : QObject(parent)
{
    currentVersion = Version(CLIENT_VERSION_MAJOR, CLIENT_VERSION_MINOR, 0);
}

YodyVersionChecker::~YodyVersionChecker()
{

}

bool YodyVersionChecker::newVersionAvailable()
{
    Version maxReleaseVersion = getMaxReleaseVersion();
    return maxReleaseVersion > currentVersion;
}

QList<Version> YodyVersionChecker::getVersions()
{
    QNetworkAccessManager manager;
    QNetworkReply *response = manager.get(QNetworkRequest(QUrl(YODY_RELEASES)));
    QTimer timer;
    timer.setSingleShot(true);
    QEventLoop event;
    connect(&timer, &QTimer::timeout, &event, &QEventLoop::quit);
    connect(response, &QNetworkReply::finished, &event, &QEventLoop::quit);
    timer.start(30000); // 30 seconds
    event.exec();

    QList<Version> versions;
    if(timer.isActive())
    {
        timer.stop();
        if(response->error() == QNetworkReply::NoError)
        {
            QString html = response->readAll();

            QRegularExpression regEx(paternVersion);
            QRegularExpressionMatchIterator regExIt = regEx.globalMatch(html);

            while (regExIt.hasNext())
            {
                QRegularExpressionMatch match = regExIt.next();
                QString versionString = match.captured().mid(5, match.captured().length() - 6); // get version string in format XX.XX.XX
                Version version(versionString);
                if(!versions.contains(version))
                {
                    versions.append(version);
                }
            }
        }
    } else
    {
        // timeout
        disconnect(response, &QNetworkReply::finished, &event, &QEventLoop::quit);
        response->abort();
    }

    return versions;
}

Version YodyVersionChecker::getMaxReleaseVersion()
{
    QList<Version> versions = getVersions();
    Version maxVersion;

    if(!versions.isEmpty())
    {
        maxVersion = *std::max_element(versions.begin(), versions.end());
    }
    return maxVersion;
}
