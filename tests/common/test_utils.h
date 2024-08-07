/*
  This file is part of Knut.

  SPDX-FileCopyrightText: 2024 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>

  SPDX-License-Identifier: GPL-3.0-only

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#pragma once

#include "utils/log.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTest>
#include <iostream>
#include <memory>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/callback_sink.h>
#include <vector>

namespace Test {

inline QString examplesPath()
{
    QString path;
#if defined(EXAMPLES_PATH)
    path = EXAMPLES_PATH;
#endif
    if (path.isEmpty() || !QDir(path).exists()) {
        path = QCoreApplication::applicationDirPath() + "/examples";
    }
    return path;
}

inline QString testDataPath()
{
    QString path;
#if defined(TEST_DATA_PATH)
    path = TEST_DATA_PATH;
#endif
    if (path.isEmpty() || !QDir(path).exists()) {
        path = QCoreApplication::applicationDirPath() + "/test_data";
    }
    return path;
}

inline bool compareFiles(const QString &file, const QString &expected, bool eolLF = true)
{
    QFile file1(file);
    if (!file1.open(QIODevice::ReadOnly)) {
        spdlog::warn("Cannot open {} for comparison!", file);
        return false;
    }
    QFile file2(expected);
    if (!file2.open(QIODevice::ReadOnly)) {
        spdlog::warn("Cannot open {} for comparison!", expected);
        return false;
    }

    auto data1 = file1.readAll();
    auto data2 = file2.readAll();
    if (eolLF) {
        data1.replace("\r\n", "\n");
        data2.replace("\r\n", "\n");
    }
    auto result = data1 == data2;
    if (!result) {
        spdlog::warn("Comparison of {} and {} failed!", file, expected);
        spdlog::warn("Actual: {}", data1);
        spdlog::warn("Expected: {}", data2);
    }
    return result;
}

inline bool compareDirectories(const QString &current, const QString &expected)
{
    QDir currentDir(current);
    if (!currentDir.exists()) {
        spdlog::warn("Cannot open directory {} for comparison!", current);
        return false;
    }

    bool result = true;

    QDirIterator it(currentDir, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();

        const auto fileInfo = it.fileInfo();

        if (fileInfo.isDir())
            continue;

        const QString subPath = fileInfo.absoluteFilePath().mid(currentDir.absolutePath().length());

        result &= compareFiles(fileInfo.absoluteFilePath(), expected + subPath);
    }

    return result;
}

/**
 * @brief The FileTester class to handle expected/original files
 * Create a temporary file based on an original one, and also compare to an expected one.
 * Delete the created file on destruction.
 */
class FileTester
{
public:
    FileTester(const QString &fileName, bool removeTempFile = true)
        : m_original(fileName)
        , m_removeTempFile(removeTempFile)
    {
        m_file = fileName;
        m_original.append(".original");
        QVERIFY(QFile::exists(m_original));
        QFile::copy(m_original, m_file);
    }
    ~FileTester()
    {
        if (m_removeTempFile) {
            QFile::remove(m_file);
        }
    }

    QString fileName() const { return m_file; }

    bool compare() const
    {
        QString expected = m_file;
        expected.append(".expected");
        return compareFiles(m_file, expected);
    }

private:
    QString m_original;
    QString m_file;
    bool m_removeTempFile = true;
};

// *****************************************************************************

class LogCounter
{
public:
    LogCounter(spdlog::level::level_enum level = spdlog::level::err, const std::string &name = "")
    {
        m_logger = name.empty() ? spdlog::default_logger() : spdlog::get(name);

        if (m_logger) {
            m_sink = std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg &msg) {
                Q_UNUSED(msg)
                std::cout << "############### LogCounter - Counting message ##############\n";
                std::cout << std::string_view(msg.payload.data(), msg.payload.size()) << '\n';
                std::cout << "############################################################" << std::endl;

                ++m_count;
            });
            m_sink->set_level(level);

            m_logger->sinks().push_back(m_sink);
        }
    }

    ~LogCounter()
    {
        if (m_logger && m_sink) {
            auto iter = std::ranges::find(m_logger->sinks(), std::dynamic_pointer_cast<spdlog::sinks::sink>(m_sink));
            if (iter != m_logger->sinks().end()) {
                m_logger->sinks().erase(iter);
            }
        }
    }

    int count() const { return m_count.load(); }

private:
    std::atomic<int> m_count;
    std::shared_ptr<spdlog::logger> m_logger;
    std::shared_ptr<spdlog::sinks::callback_sink_mt> m_sink;
};

// *****************************************************************************

constexpr inline bool noClangd()
{
#if defined(NO_CLANGD)
    return true;
#else
    return false;
#endif
}

constexpr inline bool obsoleteClangd()
{
#if defined(OBSOLETE_CLANGD)
    return true;
#else
    return false;
#endif
}
}

// Check if clangd is available, needed for some tests
#define CHECK_CLANGD                                                                                                   \
    do {                                                                                                               \
        if constexpr (Test::noClangd())                                                                                \
            QSKIP("clangd is not available to run the test");                                                          \
    } while (false)

// Check if clangd is available, and if the version is high enough
#define CHECK_CLANGD_VERSION                                                                                           \
    do {                                                                                                               \
        if constexpr (Test::noClangd())                                                                                \
            QSKIP("clangd is not available to run the test");                                                          \
        else if constexpr (Test::obsoleteClangd())                                                                     \
            QSKIP("clangd version is too old to run the test");                                                        \
    } while (false)

// Qt6 prior to 6.3 uses QVERIFY_EXCEPTION_THROWN, which doesn't support expressions that include commas
// so let's define the Qt6.3 version here, if it doesn't already exist:
#ifndef QVERIFY_THROWS_EXCEPTION

// QTest::qCaught also doesn't exist in Qt version prior to 6.3, so modify the macro to use QTest::qFail instead.
#define QVERIFY_THROWS_EXCEPTION(exceptiontype, ...)                                                                   \
    do {                                                                                                               \
        QT_TRY                                                                                                         \
        {                                                                                                              \
            QT_TRY                                                                                                     \
            {                                                                                                          \
                __VA_ARGS__;                                                                                           \
                QTest::qFail("Expected exception of type " #exceptiontype " to be thrown"                              \
                             " but no exception caught",                                                               \
                             __FILE__, __LINE__);                                                                      \
                return;                                                                                                \
            }                                                                                                          \
            QT_CATCH(const exceptiontype &)                                                                            \
            { /* success */                                                                                            \
            }                                                                                                          \
        }                                                                                                              \
        QT_CATCH(const std::exception &e)                                                                              \
        {                                                                                                              \
            QByteArray msg = QByteArray()                                                                              \
                + "Expected exception of type " #exceptiontype " to be thrown"                                         \
                  " but std::exception caught with message: "                                                          \
                + e.what();                                                                                            \
            QTest::qFail(msg.constData(), __FILE__, __LINE__);                                                         \
            return;                                                                                                    \
        }                                                                                                              \
        QT_CATCH(...)                                                                                                  \
        {                                                                                                              \
            QTest::qFail("Expected exception of type " #exceptiontype " to be thrown"                                  \
                         " but unknown exception caught",                                                              \
                         __FILE__, __LINE__);                                                                          \
            QT_RETHROW;                                                                                                \
        }                                                                                                              \
    } while (false)

#endif
