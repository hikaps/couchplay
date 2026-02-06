#include "AppImageHelper.h"
#include <QtGlobal>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDebug>

bool AppImageHelper::isRunningAsAppImage()
{
    return !qgetenv("APPIMAGE").isEmpty();
}

bool AppImageHelper::isHelperInstalled()
{
    // Check if helper exists in system path
    // We check /usr/libexec/couchplay-helper as primary location
    // And /usr/local/libexec/couchplay-helper as secondary
    return QFile::exists(QStringLiteral("/usr/libexec/couchplay-helper")) || 
           QFile::exists(QStringLiteral("/usr/local/libexec/couchplay-helper"));
}

QString AppImageHelper::getAppDir()
{
    // When running as AppImage, APPDIR env var points to the mount point
    QString appDir = qgetenv("APPDIR");
    if (appDir.isEmpty()) {
        return QCoreApplication::applicationDirPath();
    }
    return appDir;
}

bool AppImageHelper::installHelper()
{
    // 1. Locate bundled helper files
    // In AppImage, they should be relative to APPDIR
    QString appDir = getAppDir();
    QString binaryPath = appDir + "/usr/libexec/couchplay-helper";
    QString scriptPath = ":/helper/install-helper.sh"; // From QRC
    QString policyPath = ":/helper/io.github.hikaps.couchplay.policy"; // From QRC
    
    if (!QFile::exists(binaryPath)) {
        qWarning() << "AppImageHelper: Helper binary not found at" << binaryPath;
        // Fallback: try relative to executable if not in standard AppDir structure
        binaryPath = QCoreApplication::applicationDirPath() + "/../libexec/couchplay-helper";
        if (!QFile::exists(binaryPath)) {
             qWarning() << "AppImageHelper: Helper binary not found at fallback" << binaryPath;
             return false;
        }
    }

    // 2. Extract to /tmp
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/couchplay-install";
    QDir().mkpath(tmpDir);
    
    QString tmpBinary = tmpDir + "/couchplay-helper";
    QString tmpScript = tmpDir + "/install-helper.sh";
    QString tmpPolicy = tmpDir + "/io.github.hikaps.couchplay.policy";
    
    // Remove existing
    QFile::remove(tmpBinary);
    QFile::remove(tmpScript);
    QFile::remove(tmpPolicy);
    
    // Copy files
    if (!QFile::copy(binaryPath, tmpBinary)) {
        qWarning() << "AppImageHelper: Failed to copy binary to" << tmpBinary;
        return false;
    }
    
    if (!QFile::copy(scriptPath, tmpScript)) {
        qWarning() << "AppImageHelper: Failed to copy script to" << tmpScript;
        return false;
    }
    
    if (!QFile::copy(policyPath, tmpPolicy)) {
        qWarning() << "AppImageHelper: Failed to copy policy to" << tmpPolicy;
        return false;
    }
    
    // Make executable
    QFile::setPermissions(tmpBinary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ReadOther);
    QFile::setPermissions(tmpScript, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ReadOther);
    
    // 3. Run pkexec
    // We pass the tmp dir as argument to the script so it knows where to find files
    QStringList args;
    args << tmpScript << "install_from_dir" << tmpDir;
    
    // Use pkexec to run the script as root
    int ret = QProcess::execute("pkexec", args);
    
    // Cleanup
    QDir(tmpDir).removeRecursively();
    
    return ret == 0;
}
