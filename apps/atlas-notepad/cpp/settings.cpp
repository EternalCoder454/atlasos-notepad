// ~/.config/atlas-notepadrc (an ini file). Every setter writes through.
#include "app.h"

#include <QFontDatabase>
#include <QStandardPaths>

namespace
{
constexpr auto fontKey = "font";

template<typename T>
bool update(QSettings &rc, const char *key, const T &value, const T &old)
{
    if (old == value) {
        return false;
    }
    rc.setValue(QLatin1String(key), value);
    rc.sync();
    return true;
}
}

QString Settings::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/atlas-notepadrc");
}

Settings::Settings(QObject *parent)
    : QObject(parent)
    , m_rc(filePath(), QSettings::IniFormat)
{
}

bool Settings::readGpuRendering()
{
    QSettings rc(filePath(), QSettings::IniFormat);
    return rc.value(QStringLiteral("gpuRendering"), false).toBool();
}

QSettings &Settings::rc()
{
    return m_rc;
}

QFont Settings::font() const
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QString saved = m_rc.value(QLatin1String(fontKey)).toString();
    if (!saved.isEmpty()) {
        font.fromString(saved);
    }
    return font;
}

void Settings::setFont(const QFont &font)
{
    if (update(m_rc, fontKey, font.toString(), this->font().toString())) {
        Q_EMIT fontChanged();
    }
}

#define BOOL_SETTING(Name, name, fallback) \
    bool Settings::name() const \
    { \
        return m_rc.value(QStringLiteral(#name), fallback).toBool(); \
    } \
    void Settings::set##Name(bool on) \
    { \
        if (update(m_rc, #name, on, name())) { \
            Q_EMIT name##Changed(); \
        } \
    }

BOOL_SETTING(WordWrap, wordWrap, true)
BOOL_SETTING(LineNumbers, lineNumbers, false)
BOOL_SETTING(StatusBar, statusBar, true)
BOOL_SETTING(FormattingToolbar, formattingToolbar, true)
BOOL_SETTING(Formatting, formatting, true)
BOOL_SETTING(OpenMarkdownFormatted, openMarkdownFormatted, true)
BOOL_SETTING(ContinueSession, continueSession, true)
BOOL_SETTING(OpenInNewWindow, openInNewWindow, false)
BOOL_SETTING(SpellCheck, spellCheck, true)
BOOL_SETTING(GpuRendering, gpuRendering, false)

int Settings::zoom() const
{
    return qBound(50, m_rc.value(QStringLiteral("zoom"), 100).toInt(), 400);
}

void Settings::setZoom(int percent)
{
    const int stepped = qBound(50, (percent + 5) / 10 * 10, 400);
    if (update(m_rc, "zoom", stepped, zoom())) {
        Q_EMIT zoomChanged();
    }
}

QRect Settings::windowGeometry() const
{
    const QVariantList v = m_rc.value(QStringLiteral("Window/geometry")).toList();
    if (v.size() != 4) {
        return {};
    }
    return QRect(v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toInt());
}

bool Settings::windowMaximized() const
{
    return m_rc.value(QStringLiteral("Window/maximized"), false).toBool();
}

void Settings::setWindowGeometry(const QRect &rect, bool maximized)
{
    if (rect.isValid()) {
        m_rc.setValue(QStringLiteral("Window/geometry"), QVariantList{rect.x(), rect.y(), rect.width(), rect.height()});
    }
    m_rc.setValue(QStringLiteral("Window/maximized"), maximized);
    m_rc.sync();
}
