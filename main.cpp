// main.cpp
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <QTextStream>
#include <QMutex>
#include <QFuture>
#include <QAtomicInt>
#include <memory>

struct Config {
    QString inputDir;
    QString fileMask;
    bool removeInputFiles;
    QString outputDir;
    bool overwriteFiles;
    bool periodic;
    int periodSeconds;
    QByteArray xorValue;
};

QMutex consoleMutex;

void log(const QString& message) {
    QMutexLocker locker(&consoleMutex);
    QTextStream(stdout) << message << Qt::endl;
}

void processFile(const QString& filePath, const Config& config, std::shared_ptr<QAtomicInt> processed, int total) {
    QDir().mkpath(config.outputDir);
    QFile inputFile(filePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        log("Failed to open: " + filePath);
        return;
    }

    QString fileName = QFileInfo(filePath).fileName();
    QString outPath = QDir(config.outputDir).filePath(fileName);

    if (QFile::exists(outPath) && !config.overwriteFiles) {
        int counter = 1;
        QString baseName = QFileInfo(fileName).baseName();
        QString extension = QFileInfo(fileName).suffix();
        do {
            outPath = QDir(config.outputDir).filePath(
                QString("%1_%2.%3").arg(baseName).arg(counter++).arg(extension));
        } while (QFile::exists(outPath));
    }

    QFile outputFile(outPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log("Failed to write: " + outPath);
        return;
    }

    const qint64 bufferSize = 1024 * 1024; // 1 MB
    while (!inputFile.atEnd()) {
        QByteArray buffer = inputFile.read(bufferSize);
        for (int i = 0; i < buffer.size(); ++i) {
            buffer[i] ^= config.xorValue[i % config.xorValue.size()];
        }
        outputFile.write(buffer);
    }

    inputFile.close();
    outputFile.close();

    if (config.removeInputFiles) {
        QFile::remove(filePath);
    }

    int done = ++(*processed);
    log(QString("Processed [%1/%2]: %3 -> %4").arg(done).arg(total).arg(filePath, outPath));
}



void scanAndProcess(const Config& config) {
    QDir dir(config.inputDir);
    QStringList files = dir.entryList(QStringList() << config.fileMask, QDir::Files);
    int total = files.size();
    if (total == 0) {
        log("No matching files found.");
        if (!config.periodic) {
            QCoreApplication::quit();
        }
        return;
    }

    auto processed = std::make_shared<QAtomicInt>(0);
    bool isFinalRun = !config.periodic;

    for (int i = 0; i < files.size(); ++i) {
        QString filePath = dir.filePath(files.at(i));
        log("Starting: " + filePath);
        QFuture<void> future = QtConcurrent::run([filePath, config, processed, total, isFinalRun]() {
            processFile(filePath, config, processed, total);
            if (isFinalRun && processed->loadRelaxed() == total) {
                log("All files processed.\n");
                QCoreApplication::quit();
            }
        });
    }
}

#include <QSettings>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    Config config;
    QTextStream in(stdin);
    QSettings settings("config.ini", QSettings::IniFormat);

    config.inputDir = settings.value("inputDir").toString();
    log("Enter input directory path [" + config.inputDir + "]: ");
    QString temp = in.readLine().trimmed();
    if (!temp.isEmpty()) config.inputDir = temp;
    if (config.inputDir.isEmpty() || !QDir(config.inputDir).exists()) {
        log("Input directory is invalid or does not exist.");
        return 1;
    }
    settings.setValue("inputDir", config.inputDir);

    log("Enter file mask (e.g., *.txt): ");
    config.fileMask = in.readLine();
    if (config.fileMask.trimmed().isEmpty()) {
        log("File mask must not be empty.");
        return 1;
    }

    log("Remove input files after processing? (yes/no): ");
    config.removeInputFiles = (in.readLine().trimmed().toLower() == "yes");

    config.outputDir = settings.value("outputDir").toString();
    log("Enter output directory path [" + config.outputDir + "]: ");
    temp = in.readLine().trimmed();
    if (!temp.isEmpty()) config.outputDir = temp;
    if (config.outputDir.trimmed().isEmpty()) {
        log("Output directory must not be empty.");
        return 1;
    }
    settings.setValue("outputDir", config.outputDir);

    log("Overwrite output files? (yes/no): ");
    config.overwriteFiles = (in.readLine().trimmed().toLower() == "yes");

    log("Periodic run? (yes/no): ");
    config.periodic = (in.readLine().trimmed().toLower() == "yes");

    if (config.periodic) {
        log("Enter period in seconds: ");
        config.periodSeconds = in.readLine().toInt();
        if (config.periodSeconds <= 0) {
            log("Period must be greater than 0.");
            return 1;
        }
    }

    log("Enter 8-byte XOR value (hex, e.g., 0102030405060708): ");
    QString xorStr = in.readLine().trimmed();
    config.xorValue = QByteArray::fromHex(xorStr.toUtf8());
    if (config.xorValue.size() != 8) {
        log("XOR value must be exactly 8 bytes!");
        return 1;
    }

    if (config.periodic) {
        QTimer* timer = new QTimer(&app);
        QObject::connect(timer, &QTimer::timeout, [config]() {
            scanAndProcess(config);
        });
        timer->start(config.periodSeconds * 1000);
        scanAndProcess(config); // initial run
        return app.exec();
    } else {
        scanAndProcess(config);
        return app.exec();
    }
}
