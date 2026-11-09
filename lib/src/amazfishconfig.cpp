#include "amazfishconfig.h"

#include <QCoreApplication>

#ifdef MER_EDITION_SAILFISH
#include <MDConfGroup>
#endif

#include <functional>

#include <QDateTime>

// VO2max reference bands, lower bound (inclusive) of each band. One row per
// age group (under 30, 30-39, 40-49, 50-59, 60-69, 70 and over), one column
// per band: superior, excellent, good, fair. A value under the last column is
// poor. Numbers taken from the Gadgetbridge VO2MaxRanges table.
static const double VO2MAX_BANDS_MALE[6][4] = {
    { 55.4, 51.1, 45.4, 41.7 },
    { 54.0, 48.3, 44.0, 40.5 },
    { 52.5, 46.4, 42.4, 38.5 },
    { 48.9, 43.4, 39.2, 35.6 },
    { 45.7, 39.5, 35.5, 32.3 },
    { 42.1, 36.7, 32.3, 29.4 },
};

static const double VO2MAX_BANDS_FEMALE[6][4] = {
    { 49.6, 43.9, 39.5, 36.1 },
    { 47.4, 42.4, 37.8, 34.4 },
    { 45.3, 39.7, 36.3, 33.0 },
    { 41.1, 36.7, 33.0, 30.1 },
    { 37.8, 33.0, 30.0, 27.5 },
    { 36.7, 30.9, 28.1, 25.9 },
};

static int vo2maxAgeRow(int age)
{
    if (age < 30) {
        return 0;
    }
    if (age < 40) {
        return 1;
    }
    if (age < 50) {
        return 2;
    }
    if (age < 60) {
        return 3;
    }
    if (age < 70) {
        return 4;
    }
    return 5;
}

AmazfishConfig::AmazfishConfig(QObject *parent)
    : QObject(parent)
#ifdef MER_EDITION_SAILFISH
    , m_group(new MDConfGroup(QStringLiteral("/uk/co/piggz/amazfish"), this))
#endif
{

}

AmazfishConfig *AmazfishConfig::instance()
{
    static AmazfishConfig *inst = nullptr;
    if (!inst) {
        inst = new AmazfishConfig(qApp);
    }
    return inst;
}

QVariant AmazfishConfig::value(const QString &key, const QVariant &def) const
{
#ifdef MER_EDITION_SAILFISH
    return m_group->value(key, def);
#else
    QSettings settings;
    return settings.value(key, def);
#endif
}

void AmazfishConfig::setValue(const QString &key, const QVariant &value)
{
#ifdef MER_EDITION_SAILFISH
    m_group->setValue(key, value);
#else
    QSettings settings;
    settings.setValue(key, value);
#endif
}

void AmazfishConfig::setValue(const QString &key, const QVariant &value, signal_ptr signal)
{
#ifdef MER_EDITION_SAILFISH
    auto prev = m_group->value(key);
#else
    QSettings settings;
    auto prev = settings.value(key);
#endif

    if (value != prev) {
#ifdef MER_EDITION_SAILFISH
        m_group->setValue(key, value);
#else
        settings.setValue(key, value);
#endif
        emit std::bind(signal, this)();
    }
}

QVariantList AmazfishConfig::vo2maxBands() const
{
    const QDateTime dob = profileDOB();
    if (!dob.isValid()) {
        return QVariantList();
    }
    const int age = dob.date().daysTo(QDate::currentDate()) / 365;
    if (age < 0) {
        // A birthday in the future. Better no bands than wrong ones.
        return QVariantList();
    }

    const double(*bands)[4] = profileGender() == ProfileGenderFemale ? VO2MAX_BANDS_FEMALE : VO2MAX_BANDS_MALE;
    const int row = vo2maxAgeRow(age);

    QVariantList out;
    for (int i = 0; i < 4; ++i) {
        out << bands[row][i];
    }
    return out;
}
