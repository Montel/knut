#pragma once

#include "string_utils.h"

#include <QAbstractItemModel>
#include <QMetaEnum>
#include <QString>
#include <QVariantList>

#include <spdlog/spdlog.h>

#include <concepts>
#include <vector>

/**
 * Create a return value, the name will depend on the type returned.
 */
#define LOG_ARG(name, value) Core::LoggerArg(name, value)

/**
 * Log a method, with all its parameters.
 */
#define LOG(name, ...) Core::LoggerObject __loggerObject(name, false, ##__VA_ARGS__)

/**
 * Log a method, with all its parameters. If the previous log is also the same method, it will be merged into one
 * operation
 */
#define LOG_AND_MERGE(name, ...) Core::LoggerObject __loggerObject(name, true, ##__VA_ARGS__)

/**
 * Macro to save the returned value in the historymodel
 */
#define LOG_RETURN(name, value)                                                                                        \
    do {                                                                                                               \
        const auto &__value = value;                                                                                   \
        __loggerObject.setReturnValue(name, __value);                                                                  \
        return __value;                                                                                                \
    } while (false)

namespace Core {

/**
 * @brief The LoggerDisabler class is a RAII class to temporary disable logging
 */
class LoggerDisabler
{
public:
    LoggerDisabler(bool silenceAll = false);
    ~LoggerDisabler();

private:
    bool m_originalCanLog = true;
    bool m_silenceAll = false;
    spdlog::level::level_enum m_level = spdlog::level::off;
};

///////////////////////////////////////////////////////////////////////////////
// Internal structs and classes
///////////////////////////////////////////////////////////////////////////////
template <typename T>
concept HasToString = requires(const T &t)
{
    t.toString();
};

template <typename T>
concept HasPointerToString = requires(const T &t)
{
    t->toString();
};

/**
 * @brief toString
 * Returns a string for any kind of data you can pass as a parameter.
 */
template <class T>
QString valueToString(const T &data)
{
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, QString>) {
        QString text = data;
        text.replace('\n', "\\n");
        text.replace('\t', "\\t");
        return text;
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, bool>)
        return data ? "true" : "false";
    else if constexpr (std::is_floating_point_v<T> || std::is_integral_v<T>)
        return QString::number(data);
    else if constexpr (std::is_same_v<std::remove_cvref_t<T>, QStringList>)
        return '{' + data.join(", ") + '}';
    else if constexpr (std::is_enum_v<T>)
        return QMetaEnum::fromType<T>().valueToKey(data);
    else if constexpr (HasToString<T>)
        return data.toString();
    else if constexpr (HasPointerToString<T>)
        return data->toString();
    else
        Q_UNREACHABLE();
    return {};
}

struct LoggerArgBase
{
};
/**
 * @brief Argument for a call
 * The argName will be matched to an existing returned value from a previous method, when recording a script.
 * If empty, or not set by a previous method, the value will be used.
 */
template <typename T>
struct LoggerArg : public LoggerArgBase
{
    LoggerArg(QString &&name, T v)
        : argName(name)
        , value(v)
    {
    }
    QString argName;
    T value;
    QString toString() const { return valueToString(value); }
};

class HistoryModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Columns { NameCol = 0, ParamCol, ColumnCount };

    explicit HistoryModel(QObject *parent = nullptr);
    ~HistoryModel();

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void clear();

    /**
     * @brief Create a script from 2 points in the history
     * The script is created using 2 rows in the history model. It will create a javascript script.
     */
    QString createScript(int start, int end);
    QString createScript(const QModelIndex &startIndex, const QModelIndex &endIndex);

private:
    friend class LoggerObject;

    struct Arg
    {
        QString name;
        QVariant value;
        bool isEmpty() const { return name.isEmpty(); }
    };
    struct LogData
    {
        QString name;
        std::vector<Arg> params;
        Arg returnArg;
    };

    void logData(const QString &name);
    template <typename... Ts>
    void logData(const QString &name, bool merge, Ts... params)
    {
        LogData data;
        data.name = name;
        fillLogData(data, params...);
        addData(std::move(data), merge);
    }

    template <typename T>
    void setReturnValue(QString &&name, const T &value)
    {
        m_data.back().returnArg.name = std::move(name);
        m_data.back().returnArg.value = QVariant::fromValue(value);
    }

    void fillLogData(LogData &) {};

    template <typename T, typename... Ts>
    void fillLogData(LogData &data, T param, Ts... params)
    {
        if constexpr (std::derived_from<T, LoggerArgBase>)
            data.params.push_back({param.argName, QVariant::fromValue(param.value)});
        else
            data.params.push_back({"", QVariant::fromValue(param)});

        fillLogData(data, params...);
    }

    void addData(LogData &&data, bool merge);

    std::vector<LogData> m_data;
};

/**
 * @brief The LoggerObject class is a utility class to help logging API calls
 *
 * This class ensure that only the first API call is logged, subsequent calls done by the first one won't.
 * Do not use this class directly, but use the macros LOG and LOG_AND_MERGE
 */
class LoggerObject
{
public:
    explicit LoggerObject(QString name, bool /*unused*/)
        : LoggerObject()
    {
        if (!m_canLog)
            return;
        if (m_model)
            m_model->logData(name);
        log(std::move(name));
    }

    template <typename... Ts>
    explicit LoggerObject(QString name, bool merge, Ts... params)
        : LoggerObject()
    {
        if (!m_canLog)
            return;
        if (m_model)
            m_model->logData(name, merge, params...);

        QStringList paramList;
        (paramList.push_back(valueToString(params)), ...);
        QString result = name + " - " + paramList.join(", ");
        log(std::move(result));
    }

    ~LoggerObject();

    template <typename T>
    void setReturnValue(QString &&name, const T &value)
    {
        if (m_firstLogger && m_model)
            m_model->setReturnValue(std::move(name), value);
    }

private:
    friend HistoryModel;
    friend LoggerDisabler;

    LoggerObject();
    void log(QString &&string);

    inline static bool m_canLog = true;
    bool m_firstLogger = false;

    inline static HistoryModel *m_model = nullptr;
};

} // namespace Core