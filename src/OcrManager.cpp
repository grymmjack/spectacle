/*
 *  SPDX-FileCopyrightText: 2025 Jhair Paris <dev@jhairparis.com>
 *  SPDX-FileCopyrightText: 2026 Kai Uwe Broulik <kde@broulik.de>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "OcrManager.h"
#include "settings.h"
#include "spectacle_debug.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QMetaObject>
#include <QRect>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>

#include <KLocalizedString>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <functional>
#include <future>

using namespace Qt::StringLiterals;

OcrManager *OcrManager::s_instance = nullptr;

OcrManager::OcrManager(QObject *parent)
    : QObject(parent)
    , m_workerThread(std::make_unique<QThread>())
    , m_timeoutTimer(new QTimer(this))
{
    m_workerThread->setObjectName(u"OcrThread"_s);

    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(30000);

    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        qCWarning(SPECTACLE_LOG) << "OCR recognition timed out";
        setStatus(OcrStatus::Error);
        Q_EMIT textRecognized({}, {}, false);
    });

    m_worker = new OcrWorker();
    m_worker->moveToThread(m_workerThread.get());

    connect(m_worker, &OcrWorker::initFinished, this, &OcrManager::handleInitializationComplete);
    connect(m_worker, &OcrWorker::languagesChanged, this, &OcrManager::handleLanguagesChanged);
    connect(m_worker, &OcrWorker::imageProcessed, this, &OcrManager::handleRecognitionComplete);

    m_workerThread->start();

    connect(Settings::self(), &Settings::ocrLanguagesChanged, this, [this]() {
        if (m_configSyncSuspended) {
            return;
        }

        const QStringList languages = Settings::ocrLanguages();

        if (languages != m_configuredLanguages) {
            setLanguagesByCode(languages);
        }
    });

    QMetaObject::invokeMethod(m_worker, &OcrWorker::init, Qt::QueuedConnection, Settings::ocrLanguages());
}

OcrManager::~OcrManager()
{
    m_workerThread->quit();
    if (!m_workerThread->wait(3000)) {
        qCWarning(SPECTACLE_LOG) << "Worker thread did not stop in time, terminating";
        m_workerThread->terminate();
        m_workerThread->wait();
    }

    delete m_worker;
    m_worker = nullptr;
}

OcrManager *OcrManager::instance()
{
    if (!s_instance) {
        s_instance = new OcrManager(qApp);
    }
    return s_instance;
}

void OcrManager::handleInitializationComplete(bool success, const QStringList &availableLanguages, const QStringList &activeLanguages)
{
    m_available = success;

    for (const QString &language : availableLanguages) {
        if (language == u"osd"_s) {
            m_languageNames.insert(language, i18nc("@item:inlistbox", "Orientation and Script Detection"));
        } else {
            m_languageNames.insert(language, tesseractLangName(language));
        }
    }
    m_activeLanguages = activeLanguages;
    m_availableLanguages = availableLanguages;

    if (!success) {
        qCWarning(SPECTACLE_LOG) << "OCR worker initialization failed";
        setStatus(OcrStatus::Error);
        return;
    }

    m_configuredLanguages = m_activeLanguages;

    // Persist fallback languages if the saved configuration
    // could not be used during initialization.
    if (Settings::ocrLanguages() != m_configuredLanguages) {
        Settings::setOcrLanguages(m_configuredLanguages);
        Settings::self()->save();
    }

    if (m_status == OcrStatus::Ready) {
        // isAvailable() just became true while the status stayed Ready: announce it anyway, or
        // bindings on it (the Extract Text button's visibility) never update.
        Q_EMIT statusChanged(m_status);
    } else {
        setStatus(OcrStatus::Ready);
    }

    qCDebug(SPECTACLE_LOG) << "OCR worker initialized with:" << m_activeLanguages;
}

bool OcrManager::isAvailable() const
{
    return m_available;
}

OcrManager::OcrStatus OcrManager::status() const
{
    return m_status;
}

QMap<QString, QString> OcrManager::availableLanguagesWithNames() const
{
    return m_languageNames;
}

QString OcrManager::currentLanguageCode() const
{
    return m_activeLanguages.join(u"+"_s);
}

void OcrManager::setLanguagesByCode(const QStringList &languageCodes)
{
    if (languageCodes.isEmpty()) {
        qCWarning(SPECTACLE_LOG) << "No OCR languages specified";
        return;
    }

    if (!isAvailable()) {
        qCWarning(SPECTACLE_LOG) << "Cannot change OCR languages: engine unavailable";
        return;
    }

    m_pendingConfiguredLanguages = languageCodes;
    QMetaObject::invokeMethod(m_worker, &OcrWorker::setLanguages, Qt::QueuedConnection, languageCodes);
}

void OcrManager::handleLanguagesChanged(const QStringList &activeLanguages, bool success)
{
    if (!success) {
        qCWarning(SPECTACLE_LOG) << "Failed to change OCR languages";

        m_pendingConfiguredLanguages.clear();
        return;
    }

    m_activeLanguages = activeLanguages;

    if (!m_pendingConfiguredLanguages.isEmpty()) {
        m_configuredLanguages = activeLanguages;
        m_pendingConfiguredLanguages.clear();

        Settings::setOcrLanguages(m_configuredLanguages);
        Settings::self()->save();

        qCDebug(SPECTACLE_LOG) << "OCR languages changed to:" << currentLanguageCode();
    }
}

void OcrManager::setConfigSyncSuspended(bool suspended)
{
    if (m_configSyncSuspended == suspended) {
        return;
    }

    m_configSyncSuspended = suspended;

    // On resume, apply any changes made to Settings
    if (!m_configSyncSuspended) {
        const QStringList settingsLanguages = Settings::ocrLanguages();
        if (settingsLanguages != m_configuredLanguages) {
            setLanguagesByCode(settingsLanguages);
        }
    }
}

bool OcrManager::isConfigSyncSuspended() const
{
    return m_configSyncSuspended;
}

void OcrManager::recognizeText(const QImage &image)
{
    if (!isAvailable()) {
        qCWarning(SPECTACLE_LOG) << "Cannot start OCR: engine is not available";
        Q_EMIT textRecognized(QString(), QStringList(), false);
        return;
    }

    if (m_status == OcrStatus::Processing) {
        qCWarning(SPECTACLE_LOG) << "Cannot start OCR: text extraction already running";
        Q_EMIT textRecognized(QString(), QStringList(), false);
        return;
    }

    if (image.isNull() || image.size().isEmpty()) {
        qCWarning(SPECTACLE_LOG) << "Cannot start OCR: invalid image provided";
        Q_EMIT textRecognized(QString(), QStringList(), false);
        return;
    }

    m_shouldRestoreToConfigured = false;

    beginRecognition(image, m_configuredLanguages);
}

void OcrManager::recognizeTextWithLanguage(const QImage &image, const QString &languageCode)
{
    if (languageCode.isEmpty()) {
        recognizeText(image);
        return;
    }

    if (!isAvailable()) {
        qCWarning(SPECTACLE_LOG) << "Cannot start OCR with language" << languageCode << ": engine is not available";
        Q_EMIT textRecognized(QString(), QStringList(), false);
        return;
    }

    if (m_status == OcrStatus::Processing) {
        qCWarning(SPECTACLE_LOG) << "Cannot start OCR with language" << languageCode << ": text extraction already running";
        Q_EMIT textRecognized(QString(), QStringList(), false);
        return;
    }

    if (image.isNull() || image.size().isEmpty()) {
        qCWarning(SPECTACLE_LOG) << "Cannot start OCR with language" << languageCode << ": invalid image provided";
        Q_EMIT textRecognized(QString(), QStringList(), false);
        return;
    }

    const QStringList tempLanguages{languageCode};

    // Store that we need to restore after recognition
    m_shouldRestoreToConfigured = (m_activeLanguages != m_configuredLanguages);

    beginRecognition(image, tempLanguages);
}

void OcrManager::beginRecognition(const QImage &image, const QStringList &languages)
{
    setStatus(OcrStatus::Processing);
    m_timeoutTimer->start();

    QMetaObject::invokeMethod(m_worker, &OcrWorker::processImage, Qt::QueuedConnection, image, languages);
}

void OcrManager::handleRecognitionComplete(const QString &text, const QStringList &activeLanguages, bool success)
{
    m_timeoutTimer->stop();

    m_activeLanguages = activeLanguages;

    if (success) {
        setStatus(OcrStatus::Ready);

        if (!text.isEmpty()) {
            QApplication::clipboard()->setText(text);
        }

        Q_EMIT textRecognized(text, m_activeLanguages, true);
        qCDebug(SPECTACLE_LOG) << "OCR recognition completed successfully";
    } else {
        setStatus(OcrStatus::Error);
        Q_EMIT textRecognized(QString(), QStringList(), false);
        qCWarning(SPECTACLE_LOG) << "OCR recognition failed";
    }

    // Restore configured languages if we used temporary ones
    if (m_shouldRestoreToConfigured && !m_configuredLanguages.isEmpty()) {
        const QStringList configuredLanguages = m_configuredLanguages;

        QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, configuredLanguages]() {
                worker->setLanguages(configuredLanguages);
            },
            Qt::QueuedConnection);

        m_shouldRestoreToConfigured = false;
    }
}

void OcrManager::setStatus(OcrStatus status)
{
    if (m_status == status) {
        return;
    }

    m_status = status;
    Q_EMIT statusChanged(status);
}

QString OcrManager::tesseractLangName(const QString &tesseractCode) const
{
    static const QMap<QString, QString> tesseractToIsoMap = {
        {u"afr"_s, u"af"_s},        {u"ara"_s, u"ar"_s},      {u"aze"_s, u"az"_s},     {u"aze_cyrl"_s, u"az"_s}, {u"bel"_s, u"be"_s},
        {u"ben"_s, u"bn"_s},        {u"bul"_s, u"bg"_s},      {u"cat"_s, u"ca"_s},     {u"ces"_s, u"cs"_s},      {u"chi_sim"_s, u"zh_CN"_s},
        {u"chi_tra"_s, u"zh_TW"_s}, {u"cym"_s, u"cy"_s},      {u"dan"_s, u"da"_s},     {u"dan_frak"_s, u"da"_s}, {u"deu"_s, u"de"_s},
        {u"deu_frak"_s, u"de"_s},   {u"deu_latf"_s, u"de"_s}, {u"ell"_s, u"el"_s},     {u"eng"_s, u"en"_s},      {u"epo"_s, u"eo"_s},
        {u"est"_s, u"et"_s},        {u"eus"_s, u"eu"_s},      {u"fas"_s, u"fa"_s},     {u"fin"_s, u"fi"_s},      {u"fra"_s, u"fr"_s},
        {u"frk"_s, u"de"_s},        {u"gla"_s, u"gd"_s},      {u"gle"_s, u"ga"_s},     {u"glg"_s, u"gl"_s},      {u"heb"_s, u"he"_s},
        {u"hin"_s, u"hi"_s},        {u"hrv"_s, u"hr"_s},      {u"hun"_s, u"hu"_s},     {u"ind"_s, u"id"_s},      {u"isl"_s, u"is"_s},
        {u"ita"_s, u"it"_s},        {u"ita_old"_s, u"it"_s},  {u"jpn"_s, u"ja"_s},     {u"kor"_s, u"ko"_s},      {u"kor_vert"_s, u"ko"_s},
        {u"lav"_s, u"lv"_s},        {u"lit"_s, u"lt"_s},      {u"nld"_s, u"nl"_s},     {u"nor"_s, u"no"_s},      {u"pol"_s, u"pl"_s},
        {u"por"_s, u"pt"_s},        {u"ron"_s, u"ro"_s},      {u"rus"_s, u"ru"_s},     {u"slk"_s, u"sk"_s},      {u"slk_frak"_s, u"sk"_s},
        {u"slv"_s, u"sl"_s},        {u"spa"_s, u"es"_s},      {u"spa_old"_s, u"es"_s}, {u"srp"_s, u"sr"_s},      {u"srp_latn"_s, u"sr"_s},
        {u"swe"_s, u"sv"_s},        {u"tur"_s, u"tr"_s},      {u"ukr"_s, u"uk"_s},     {u"vie"_s, u"vi"_s},      {u"amh"_s, u"am"_s},
        {u"asm"_s, u"as"_s},        {u"bod"_s, u"bo"_s},      {u"dzo"_s, u"dz"_s},     {u"guj"_s, u"gu"_s},      {u"kan"_s, u"kn"_s},
        {u"kat"_s, u"ka"_s},        {u"kat_old"_s, u"ka"_s},  {u"kaz"_s, u"kk"_s},     {u"khm"_s, u"km"_s},      {u"kir"_s, u"ky"_s},
        {u"lao"_s, u"lo"_s},        {u"mal"_s, u"ml"_s},      {u"mar"_s, u"mr"_s},     {u"mya"_s, u"my"_s},      {u"nep"_s, u"ne"_s},
        {u"ori"_s, u"or"_s},        {u"pan"_s, u"pa"_s},      {u"sin"_s, u"si"_s},     {u"tam"_s, u"ta"_s},      {u"tel"_s, u"te"_s},
        {u"tha"_s, u"th"_s},        {u"urd"_s, u"ur"_s},      {u"bos"_s, u"bs"_s},     {u"bre"_s, u"br"_s},      {u"cos"_s, u"co"_s},
        {u"fao"_s, u"fo"_s},        {u"fil"_s, u"tl"_s},      {u"fry"_s, u"fy"_s},     {u"hat"_s, u"ht"_s},      {u"hye"_s, u"hy"_s},
        {u"iku"_s, u"iu"_s},        {u"jav"_s, u"jv"_s},      {u"kmr"_s, u"ku"_s},     {u"kur"_s, u"ku"_s},      {u"lat"_s, u"la"_s},
        {u"ltz"_s, u"lb"_s},        {u"mkd"_s, u"mk"_s},      {u"mlt"_s, u"mt"_s},     {u"mon"_s, u"mn"_s},      {u"mri"_s, u"mi"_s},
        {u"msa"_s, u"ms"_s},        {u"oci"_s, u"oc"_s},      {u"pus"_s, u"ps"_s},     {u"que"_s, u"qu"_s},      {u"san"_s, u"sa"_s},
        {u"snd"_s, u"sd"_s},        {u"sqi"_s, u"sq"_s},      {u"sun"_s, u"su"_s},     {u"swa"_s, u"sw"_s},      {u"tat"_s, u"tt"_s},
        {u"tgk"_s, u"tg"_s},        {u"tgl"_s, u"tl"_s},      {u"tir"_s, u"ti"_s},     {u"ton"_s, u"to"_s},      {u"uig"_s, u"ug"_s},
        {u"uzb"_s, u"uz"_s},        {u"uzb_cyrl"_s, u"uz"_s}, {u"yid"_s, u"yi"_s},     {u"yor"_s, u"yo"_s},
    };

    if (tesseractCode == u"equ"_s) {
        return i18n("Math/Equation Detection");
    }
    if (tesseractCode == u"osd"_s) {
        return i18n("Orientation and Script Detection");
    }

    const QString isoCode = tesseractToIsoMap.value(tesseractCode);
    if (!isoCode.isEmpty()) {
        QLocale locale(isoCode);
        QString name = locale.nativeLanguageName();

        if (!name.isEmpty()) {
            name[0] = name[0].toUpper();
            return name;
        }

        QString languageName = QLocale::languageToString(locale.language());
        if (!languageName.isEmpty()) {
            languageName[0] = languageName[0].toUpper();
            return languageName;
        }
    }

    return tesseractCode;
}

OcrWorker::OcrWorker(QObject *parent)
    : QObject(parent)
{
}

OcrWorker::~OcrWorker()
{
    if (m_tesseract) {
        m_tesseract->End();
    }
}

void OcrWorker::init(const QStringList &configuredLanguages)
{
    auto cleanup = qScopeGuard([this] {
        if (m_tesseract) {
            m_tesseract->End();
            m_tesseract.reset();
        }
        Q_EMIT initFinished(false, {}, {});
    });

    try {
        m_tesseract = std::make_unique<tesseract::TessBaseAPI>();

        if (m_tesseract->Init(nullptr, nullptr) != 0) {
            qCWarning(SPECTACLE_LOG) << "Failed to initialize Tesseract";
            m_tesseract.reset();
        } else if (setupAvailableLanguages()) {
            QStringList initialLanguages;

            for (const QString &language : configuredLanguages) {
                if (language != u"osd"_s && m_availableLanguages.contains(language) && !initialLanguages.contains(language)) {
                    initialLanguages.append(language);
                }
            }

            if (initialLanguages.isEmpty()) {
                initialLanguages = fallbackLanguages();
            }

            if (initialLanguages.isEmpty()) {
                qCWarning(SPECTACLE_LOG) << "No initial languages";
                return;
            }

            if (!applyLanguages(initialLanguages)) {
                qCWarning(SPECTACLE_LOG) << "Failed to initialize Tesseract with available languages";
                return;
            }
        }

        cleanup.dismiss();
        Q_EMIT initFinished(true, m_availableLanguages, m_activeLanguages);
    } catch (const std::exception &e) {
        qCWarning(SPECTACLE_LOG) << "Exception during Tesseract initialization:" << e.what();
    }
}

bool OcrWorker::setupAvailableLanguages()
{
    m_availableLanguages.clear();

    if (!m_tesseract) {
        return false;
    }

    std::vector<std::string> languages;
    m_tesseract->GetAvailableLanguagesAsVector(&languages);

    for (const auto &language : languages) {
        const QString code = QString::fromUtf8(language);

        if (!code.isEmpty() && !m_availableLanguages.contains(code)) {
            m_availableLanguages.append(code);
        }
    }

    std::sort(m_availableLanguages.begin(), m_availableLanguages.end());

    qCDebug(SPECTACLE_LOG) << "Detected OCR languages:" << m_availableLanguages;

    return !m_availableLanguages.isEmpty();
}

QStringList OcrWorker::fallbackLanguages() const
{
    const auto it = std::find_if(m_availableLanguages.cbegin(), m_availableLanguages.cend(), [](const QString &language) {
        return language != u"osd"_s;
    });

    if (it == m_availableLanguages.cend()) {
        return {};
    }

    return {*it};
}

bool OcrWorker::applyLanguages(const QStringList &languages)
{
    if (!m_tesseract || languages.isEmpty()) {
        return false;
    }

    QStringList validLanguages;

    for (const QString &language : languages) {
        if (language == u"osd"_s) {
            continue;
        }

        if (!m_availableLanguages.contains(language)) {
            qCWarning(SPECTACLE_LOG) << "OCR language not available:" << language;
            continue;
        }

        if (!validLanguages.contains(language)) {
            validLanguages.append(language);
        }
    }

    if (validLanguages.size() > OcrManager::MAX_OCR_LANGUAGES) {
        validLanguages = validLanguages.mid(0, OcrManager::MAX_OCR_LANGUAGES);
    }

    if (validLanguages.isEmpty()) {
        return false;
    }

    if (validLanguages == m_activeLanguages) {
        return true;
    }

    // Verify all requested traineddata files exist.
    const char *datapath = m_tesseract->GetDatapath();
    const QString tessdataPath = datapath ? QString::fromUtf8(datapath) : QString();

    if (tessdataPath.isEmpty()) {
        qCWarning(SPECTACLE_LOG) << "Tessdata path is empty";
        return false;
    }

    QDir tessdataDir{tessdataPath};
    for (const QString &language : std::as_const(validLanguages)) {
        const QString path = tessdataDir.filePath(language + u".traineddata"_s);

        if (!QFile::exists(path)) {
            qCWarning(SPECTACLE_LOG) << "Language file not found:" << path;
            return false;
        }
    }

    m_tesseract->End();

    const QByteArray languageData = validLanguages.join(u"+"_s).toUtf8();

    if (m_tesseract->Init(nullptr, languageData.constData()) != 0) {
        qCWarning(SPECTACLE_LOG) << "Failed to initialize Tesseract with:" << validLanguages;

        m_activeLanguages.clear();
        return false;
    }

    m_tesseract->SetPageSegMode(tesseract::PSM_AUTO);
    m_activeLanguages = validLanguages;

    qCDebug(SPECTACLE_LOG) << "Applied OCR languages:" << m_activeLanguages;

    return true;
}

void OcrWorker::setLanguages(const QStringList &languages)
{
    const bool success = applyLanguages(languages);

    Q_EMIT languagesChanged(m_activeLanguages, success);
}

namespace
{
// A line of text found by one OCR pass, in image pixels.
struct OcrLine {
    QString text;
    QRect box;
    double confidence = 0; // mean word confidence, 0-100
};

// Below this mean word confidence a "line" is usually noise read from icons or edges.
constexpr double minLineConfidence = 60;
// Words further apart than this many line heights are separate texts that Tesseract joined
// because they sit on the same row, e.g. a sidebar item and the page beside it.
constexpr double maxWordGapInLineHeights = 2.5;
// Readings of the same text by different passes overlap at least this much.
constexpr double sameLineOverlap = 0.3;

// Reads the image with one thresholding method and returns its confident lines.
QList<OcrLine> readLines(tesseract::TessBaseAPI *api, const QImage &image, const char *thresholdingMethod)
{
    QList<OcrLine> lines;
    api->SetVariable("thresholding_method", thresholdingMethod);
    api->SetImage(image.constBits(), image.width(), image.height(), 1, image.bytesPerLine());
    if (api->Recognize(nullptr) != 0) {
        return lines;
    }
    std::unique_ptr<TessResultIterator> iterator{api->GetIterator()};
    if (!iterator) {
        return lines;
    }

    struct Word {
        QString text;
        QRect box;
        float confidence;
    };
    QList<Word> words;
    const auto endLine = [&] {
        if (words.isEmpty()) {
            return;
        }
        std::ranges::sort(words, {}, [](const Word &word) {
            return word.box.left();
        });
        const int lineHeight = std::ranges::max(words, {}, [](const Word &word) {
                                   return word.box.height();
                               }).box.height();
        qsizetype segmentStart = 0;
        for (qsizetype i = 1; i <= words.size(); ++i) {
            if (i < words.size() && words[i].box.left() - words[i - 1].box.right() <= maxWordGapInLineHeights * lineHeight) {
                continue;
            }
            OcrLine line;
            QStringList texts;
            for (qsizetype j = segmentStart; j < i; ++j) {
                texts.append(words[j].text);
                line.box |= words[j].box;
                line.confidence += words[j].confidence;
            }
            line.text = texts.join(QLatin1Char(' '));
            line.confidence /= texts.size();
            if (line.confidence >= minLineConfidence) {
                lines.append(line);
            }
            segmentStart = i;
        }
        words.clear();
    };
    do {
        if (iterator->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) {
            endLine();
        }
        std::unique_ptr<char[]> text{iterator->GetUTF8Text(tesseract::RIL_WORD)};
        const float confidence = iterator->Confidence(tesseract::RIL_WORD);
        const QString word = text ? QString::fromUtf8(text.get()).trimmed() : QString();
        int left, top, right, bottom;
        if (word.isEmpty() || confidence < 0 || !iterator->BoundingBox(tesseract::RIL_WORD, &left, &top, &right, &bottom)) {
            continue;
        }
        words.append({word, QRect(QPoint(left, top), QPoint(right - 1, bottom - 1)), confidence});
    } while (iterator->Next(tesseract::RIL_WORD));
    endLine();
    return lines;
}

// Recursive XY-cut: split the lines at the widest empty gap, across or down, and read the
// parts in order (left then right, top then bottom). Whole regions separate first, so a
// sidebar or a window beside another reads as one block instead of interleaving by row.
void appendInReadingOrder(QList<OcrLine> lines, QStringList &out)
{
    if (lines.size() <= 1) {
        for (const OcrLine &line : std::as_const(lines)) {
            out.append(line.text);
        }
        return;
    }
    int bestGap = 0;
    QList<OcrLine> bestSorted;
    qsizetype bestSplit = -1;
    for (const bool across : {true, false}) { // across: columns side by side; else rows
        const auto start = [across](const OcrLine &line) {
            return across ? line.box.left() : line.box.top();
        };
        const auto end = [across](const OcrLine &line) {
            return across ? line.box.right() : line.box.bottom();
        };
        QList<OcrLine> sorted = lines;
        std::ranges::sort(sorted, {}, start);
        int furthest = end(sorted.first());
        for (qsizetype i = 1; i < sorted.size(); ++i) {
            const int gap = start(sorted[i]) - furthest;
            if (gap > bestGap) {
                bestGap = gap;
                bestSorted = sorted;
                bestSplit = i;
            }
            furthest = std::max(furthest, end(sorted[i]));
        }
    }
    if (bestSplit < 0) { // overlapping lines: no gap to cut at
        std::ranges::sort(lines, [](const OcrLine &a, const OcrLine &b) {
            return std::pair(a.box.top(), a.box.left()) < std::pair(b.box.top(), b.box.left());
        });
        for (const OcrLine &line : std::as_const(lines)) {
            out.append(line.text);
        }
        return;
    }
    appendInReadingOrder(bestSorted.mid(0, bestSplit), out);
    appendInReadingOrder(bestSorted.mid(bestSplit), out);
}

// Merges the lines found by all passes: each text once, in reading order.
QStringList mergeLines(QList<OcrLine> lines)
{
    // Most confident and longest readings first; a line overlapping one already kept is the
    // same text read by another pass.
    std::ranges::sort(lines, std::greater{}, [](const OcrLine &line) {
        return line.confidence * line.text.size();
    });
    const auto overlap = [](const QRect &a, const QRect &b) {
        const QRect common = a & b;
        const qint64 smallerArea = std::min(qint64(a.width()) * a.height(), qint64(b.width()) * b.height());
        return smallerArea > 0 ? double(qint64(common.width()) * common.height()) / smallerArea : 0.0;
    };
    QList<OcrLine> kept;
    for (const OcrLine &line : std::as_const(lines)) {
        if (std::ranges::none_of(kept, [&](const OcrLine &other) {
                return overlap(line.box, other.box) >= sameLineOverlap;
            })) {
            kept.append(line);
        }
    }
    QStringList out;
    appendInReadingOrder(kept, out);
    return out;
}
}

tesseract::TessBaseAPI *OcrWorker::secondEngine()
{
    const QByteArray languages = m_activeLanguages.join(u'+').toUtf8();
    if (languages.isEmpty()) {
        return nullptr;
    }
    if (!m_secondTesseract || m_secondTesseractLanguages != languages) {
        m_secondTesseract = std::make_unique<tesseract::TessBaseAPI>();
        if (m_secondTesseract->Init(nullptr, languages.constData()) != 0) {
            qCWarning(SPECTACLE_LOG) << "Failed to start a second OCR engine; reading passes in sequence";
            m_secondTesseract.reset();
            m_secondTesseractLanguages.clear();
            return nullptr;
        }
        m_secondTesseract->SetPageSegMode(tesseract::PSM_AUTO);
        m_secondTesseractLanguages = languages;
    }
    return m_secondTesseract.get();
}

void OcrWorker::processImage(const QImage &image, const QStringList &languages)
{
    if (!m_tesseract || image.isNull()) {
        Q_EMIT imageProcessed({}, m_activeLanguages, false);
        return;
    }

    try {
        if (!applyLanguages(languages)) {
            Q_EMIT imageProcessed({}, m_activeLanguages, false);
            return;
        }

        // Screen text is small (10-12 px) and often light on a coloured bar. Tesseract reads it
        // far better as grayscale at twice the size: at 1x in colour it dropped white-on-blue
        // lines and misread small glyphs. The upscale must be bicubic: bilinear (Qt's smooth
        // scaling) and Lanczos still split lines and read "0" as "O" in testing.
        // Images from HiDPI screens are already large enough. Upscaling is skipped for huge
        // images to bound memory and time (1080p x2 took ~0.4 s).
        constexpr qint64 maxUpscaledPixels = 40'000'000;
        QImage prepared = image.convertToFormat(QImage::Format_Grayscale8);
        const qint64 upscaledPixels = qint64(prepared.width()) * prepared.height() * 4;
        if (image.devicePixelRatio() < 1.5 && upscaledPixels <= maxUpscaledPixels) {
            QImage upscaled(prepared.size() * 2, QImage::Format_Grayscale8);
            const cv::Mat src(prepared.height(), prepared.width(), CV_8UC1, prepared.bits(), prepared.bytesPerLine());
            cv::Mat dst(upscaled.height(), upscaled.width(), CV_8UC1, upscaled.bits(), upscaled.bytesPerLine());
            cv::resize(src, dst, dst.size(), 0, 0, cv::INTER_CUBIC);
            prepared = upscaled;
        }

        // Tesseract binarizes the image before reading it, and on screenshots no one way works:
        // the default global (Otsu) threshold read nothing from light text on a dark panel
        // beside a black area, the adaptive and Sauvola ones each found lines the others
        // missed, and green-on-black terminal text was only read right with the image
        // inverted. Every line a pass did find was read with high confidence. So the image is
        // read six ways (three thresholds, plain and inverted) and the confident lines merged.
        // On 7 test screenshots (light/dark themes, coloured bars, sidebar, terminal, mixed
        // backgrounds, a full 1080p desktop) this reads 48 of 48 lines exactly; the best single
        // way read 37. The inverted passes run on a second engine in parallel, so a 1080p
        // screenshot takes about as long as three passes.
        const auto readAll = [](tesseract::TessBaseAPI *api, const QImage &image) {
            QList<OcrLine> lines;
            for (const char *method : {"0", "1", "2"}) { // global Otsu, adaptive Otsu, Sauvola
                lines += readLines(api, image, method);
            }
            api->SetVariable("thresholding_method", "0");
            return lines;
        };
        QImage inverted = prepared;
        inverted.invertPixels();
        tesseract::TessBaseAPI *second = secondEngine();
        std::future<QList<OcrLine>> invertedLines;
        if (second) {
            invertedLines = std::async(std::launch::async, readAll, second, std::cref(inverted));
        }
        QList<OcrLine> lines = readAll(m_tesseract.get(), prepared);
        lines += second ? invertedLines.get() : readAll(m_tesseract.get(), inverted);

        const QStringList resultLines = mergeLines(lines);
        qCDebug(SPECTACLE_LOG) << "OCR:" << lines.size() << "lines from 6 passes," << resultLines.size() << "kept";
        QString result = resultLines.join(QLatin1Char('\n'));
        // Tesseract's models often read straight quotes as typographic ones. Screen text is
        // mostly pasted into terminals, code and chat, where curly quotes break things.
        for (const auto &[curly, straight] : {std::pair{u'\u201C', u'"'}, {u'\u201D', u'"'}, {u'\u201E', u'"'},
                                               {u'\u2018', u'\''}, {u'\u2019', u'\''}, {u'\u201A', u'\''}}) {
            result.replace(QChar(curly), QChar(straight));
        }
        Q_EMIT imageProcessed(result, m_activeLanguages, true);
    } catch (const std::exception &e) {
        qCWarning(SPECTACLE_LOG) << "Exception in OCR worker:" << e.what();
        Q_EMIT imageProcessed({}, m_activeLanguages, false);
    }
}

#include "moc_OcrManager.cpp"
