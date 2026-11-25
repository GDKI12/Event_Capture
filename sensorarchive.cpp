#include "sensorarchive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes, QString& error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("센서 파일 저장 실패: %1 (%2)").arg(path, file.errorString());
        return false;
    }
    return true;
}
QString stem(double timestamp) { return QString::number(timestamp, 'f', 0); }
}

void SensorArchive::clear()
{
    root.clear();
    images.clear();
    lidars.clear();
    imagesFinished = false;
}

void SensorArchive::begin(const QString& directory, int toleranceMs)
{
    clear();
    root = directory;
    tolerance = toleranceMs;
}

bool SensorArchive::addImage(const QByteArray& bgr, int width, int height, double timestamp, QString& error)
{
    if (root.isEmpty() || width <= 0 || height <= 0 || quint64(width) * height * 3 != quint64(bgr.size())
        || !std::isfinite(timestamp) || timestamp <= 0 || timestamp > 9e15) {
        error = QStringLiteral("BGR8 RAW 프레임 크기 또는 타임스탬프가 잘못되었습니다.");
        return false;
    }
    const auto folder = QDir(root).filePath("image");
    const auto filename = stem(timestamp) + ".raw";
    if (!QDir().mkpath(folder) || QFileInfo::exists(QDir(folder).filePath(filename))) {
        error = QStringLiteral("RAW 이미지 폴더 생성 실패 또는 타임스탬프 중복");
        return false;
    }
    if (!writeFile(QDir(folder).filePath(filename), bgr, error))
        return false;
    images.append({timestamp, filename, width, height});
    return true;
}

bool SensorArchive::addLidar(const QByteArray& xyzi, double timestamp, QString& error)
{
    if (root.isEmpty() || images.isEmpty())
        return true;
    if (timestamp < images.first().timestamp - tolerance
        || (imagesFinished && timestamp > images.last().timestamp + tolerance))
        return true;
    if (xyzi.isEmpty() || xyzi.size() % 16 != 0 || !std::isfinite(timestamp) || timestamp <= 0 || timestamp > 9e15) {
        error = QStringLiteral("라이다 BIN 데이터 또는 타임스탬프가 잘못되었습니다.");
        return false;
    }
    if (lidars.contains(timestamp))
        return true;
    const auto folder = QDir(root).filePath("lidar-candidates");
    const auto filename = stem(timestamp) + ".bin";
    if (!QDir().mkpath(folder) || QFileInfo::exists(QDir(folder).filePath(filename))) {
        error = QStringLiteral("라이다 임시 폴더 생성 실패 또는 타임스탬프 중복");
        return false;
    }
    if (!writeFile(QDir(folder).filePath(filename), xyzi, error))
        return false;
    lidars.insert(timestamp, filename);
    return true;
}

bool SensorArchive::match(QJsonArray& pairs, QString& error) const
{
    pairs = {};
    if (!imagesFinished || images.isEmpty() || lidars.isEmpty()) {
        error = QStringLiteral("클립과 같은 시간대의 라이다 데이터 수신 대기");
        return false;
    }
    for (const auto& image : images) {
        auto nearest = lidars.lowerBound(image.timestamp);
        if (nearest == lidars.cend())
            --nearest;
        else if (nearest != lidars.cbegin()) {
            auto before = nearest;
            --before;
            if (std::abs(before.key() - image.timestamp) <= std::abs(nearest.key() - image.timestamp))
                nearest = before;
        }
        const double delta = std::abs(nearest.key() - image.timestamp);
        if (delta > tolerance) {
            error = QStringLiteral("이미지 %1에 대응하는 라이다 없음 (차이 %2ms, 허용 %3ms)")
                        .arg(image.file).arg(delta, 0, 'f', 3).arg(tolerance);
            return false;
        }
        pairs.append(QJsonObject{{"image", "image/" + image.file}, {"imageTimestampMs", image.timestamp},
                                  {"lidar", "lidar/" + nearest.value()}, {"lidarTimestampMs", nearest.key()},
                                  {"differenceMs", delta}});
    }
    return true;
}

bool SensorArchive::stage(const QString& destination, const QJsonArray& pairs, QString& error)
{
    // Both directories are under save_path; move the packed BGR8 RAW bytes intact.
    if (!QDir().rename(QDir(root).filePath("image"), QDir(destination).filePath("image"))
        || !QDir().mkpath(QDir(destination).filePath("lidar"))) {
        error = QStringLiteral("RAW/BIN 저장 폴더 준비 실패");
        return false;
    }
    QSet<QString> selected;
    for (const auto& value : pairs) {
        const auto relative = value.toObject().value("lidar").toString();
        if (selected.contains(relative))
            continue;
        const auto source = QDir(root).filePath("lidar-candidates/" + QFileInfo(relative).fileName());
        if (!QFile::rename(source, QDir(destination).filePath(relative))) {
            error = QStringLiteral("라이다 BIN 파일 저장 실패: %1").arg(relative);
            return false;
        }
        selected.insert(relative);
    }
    return true;
}

bool SensorArchive::emptyScene(const QString& path)
{
    if (!QFileInfo::exists(path))
        return true;
    if (!QFileInfo(path).isDir())
        return false;
    const auto entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (!entry.isDir() || entry.isSymLink() || (entry.fileName() != "image" && entry.fileName() != "lidar")
            || !QDir(entry.absoluteFilePath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
            return false;
    }
    return true;
}

bool SensorArchive::commitScene(const QString& staging, const QString& destination, QString& error)
{
    if (!emptyScene(destination)) {
        error = QStringLiteral("장면 폴더에 이미 파일이 있습니다: %1").arg(destination);
        return false;
    }
    if (!QFileInfo::exists(destination)) {
        if (QDir().rename(staging, destination))
            return true;
        error = QStringLiteral("장면 폴더 확정 실패: %1").arg(destination);
        return false;
    }
    // The server may have pre-created empty scene/image and scene/lidar folders.
    QStringList moved;
    QDirIterator iterator(staging, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source = iterator.next();
        const auto target = QDir(destination).filePath(QDir(staging).relativeFilePath(source));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::rename(source, target)) {
            for (const auto& created : moved)
                QFile::remove(created);
            error = QStringLiteral("장면 파일 확정 실패: %1").arg(target);
            return false;
        }
        moved.append(target);
    }
    return true;
}
