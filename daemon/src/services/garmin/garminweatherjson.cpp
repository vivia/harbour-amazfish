#include "garminweatherjson.h"

#include "garminweathermessage.h"
#include "weather/currentweather.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace {

// Unit strings the watch asks for in the tempUnit and speedUnit query.
const char kUnitCelsius[] = "CELSIUS";
const char kUnitFahrenheit[] = "FAHRENHEIT";
const char kUnitKelvin[] = "KELVIN";
const char kUnitMetersPerSecond[] = "METERS_PER_SECOND";
const char kUnitKilometersPerHour[] = "KILOMETERS_PER_HOUR";

// Same guard as GarminWeatherMessage::encodeWeather: CurrentWeather is zeroed
// before the first API reply has been fetched.
bool haveWeather(const CurrentWeather* weather)
{
    return weather && weather->city() && weather->dateTime() > 0;
}

// The value/units pair Gadgetbridge calls WeatherValue.
QJsonObject weatherValue(double value, const QString& units)
{
    QJsonObject valueObject;
    valueObject.insert(QStringLiteral("value"), value);
    valueObject.insert(QStringLiteral("units"), units);
    return valueObject;
}

// CurrentWeather keeps Kelvin. For Celsius this copies Gadgetbridge's
// deliberate kelvin - 273 instead of kelvin - 273.15 (Gadgetbridge issue 4313:
// the plain subtraction matches what the watches expect).
QJsonObject temperatureValue(qreal kelvin, const QString& unit)
{
    if (unit == QLatin1String(kUnitFahrenheit)) {
        return weatherValue(qRound((kelvin - 273.15) * 1.8 + 32.0), QLatin1String(kUnitFahrenheit));
    }
    if (unit == QLatin1String(kUnitKelvin)) {
        return weatherValue(qRound(kelvin), QLatin1String(kUnitKelvin));
    }
    if (unit != QLatin1String(kUnitCelsius)) {
        qWarning().noquote()
            << QStringLiteral("Garmin: Unknown temperature unit %1, answering CELSIUS").arg(unit);
    }
    return weatherValue(qRound(kelvin) - 273, QLatin1String(kUnitCelsius));
}

// CurrentWeather keeps wind speed in m/s, so only km/h needs a conversion.
QJsonObject windValue(qreal speedMs, qreal directionDeg, const QString& unit)
{
    double speed = speedMs;
    QString units = QLatin1String(kUnitMetersPerSecond);
    if (unit != QLatin1String(kUnitMetersPerSecond)) {
        if (unit != QLatin1String(kUnitKilometersPerHour)) {
            qWarning().noquote()
                << QStringLiteral("Garmin: Unknown speed unit %1, answering KILOMETERS_PER_HOUR").arg(unit);
        }
        speed = speedMs * 3.6;
        units = QLatin1String(kUnitKilometersPerHour);
    }
    // QJsonDocument writes doubles with up to 17 significant digits, so a
    // value like 34.87 goes on the wire as 34.869999999999997. Round to a
    // whole number to keep the value short; a weather display cannot use
    // the extra digits.
    speed = qRound(speed);
    const int direction = (qRound(directionDeg) % 360 + 360) % 360;
    static const char* const kCompass[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    QJsonObject wind;
    wind.insert(QStringLiteral("speed"), weatherValue(speed, units));
    wind.insert(QStringLiteral("directionString"), QLatin1String(kCompass[qRound(direction / 45.0) % 8]));
    wind.insert(QStringLiteral("direction"), direction);
    return wind;
}

// OpenWeather condition id to the Garmin icon number. Same grouping as
// Gadgetbridge's WeatherInterceptor.mapToGarminCondition, a Venu 3 icon map.
int garminIcon(int owmCode)
{
    switch (owmCode) {
    case 200: // thunderstorm with light rain
    case 201: // thunderstorm with rain
    case 202: // thunderstorm with heavy rain
    case 210: // light thunderstorm
    case 211: // thunderstorm
    case 212: // heavy thunderstorm
    case 221: // ragged thunderstorm
    case 230: // thunderstorm with light drizzle
    case 231: // thunderstorm with drizzle
    case 232: // thunderstorm with heavy drizzle
        return 27; // thunder with rain
    case 771: // squalls
    case 781: // tornado
    case 900: // tornado
    case 901: // tropical storm
    case 902: // hurricane
    case 960: // storm
    case 961: // violent storm
    case 962: // hurricane
    case 905: // windy
    case 951: // calm
    case 952: // light breeze
    case 953: // gentle breeze
    case 954: // moderate breeze
    case 955: // fresh breeze
    case 956: // strong breeze
    case 957: // high wind, near gale
    case 958: // gale
    case 959: // severe gale
        return 46; // wind
    case 300: // light intensity drizzle
    case 301: // drizzle
    case 302: // heavy intensity drizzle
    case 310: // light intensity drizzle rain
    case 311: // drizzle rain
    case 312: // heavy intensity drizzle rain
    case 313: // shower rain and drizzle
    case 314: // heavy shower rain and drizzle
    case 321: // shower drizzle
    case 500: // light rain
    case 501: // moderate rain
    case 502: // heavy intensity rain
    case 503: // very heavy rain
    case 504: // extreme rain
    case 520: // light intensity shower rain
    case 521: // shower rain
    case 522: // heavy intensity shower rain
    case 531: // ragged shower rain
        return 17; // rain
    case 511: // freezing rain
    case 615: // light rain and snow
    case 616: // rain and snow
    case 906: // hail
        return 40; // snow with rain
    case 600: // light snow
    case 601: // snow
    case 602: // heavy snow
    case 611: // sleet
    case 612: // light shower sleet
    case 620: // light shower snow
    case 621: // shower snow
    case 622: // heavy shower snow
        return 38; // snow with clouds, big flake
    case 701: // mist
    case 711: // smoke
    case 721: // haze
    case 731: // sand/dust whirls
    case 741: // fog
    case 751: // sand
    case 761: // dust
    case 762: // volcanic ash
        return 47; // fog
    case 800: // clear sky
    case 904: // hot
        return 5; // sunny
    case 801: // few clouds
    case 802: // scattered clouds
        return 8; // sun cloudy
    case 803: // broken clouds
    case 804: // overcast clouds
        return 15; // clouds
    case 903: // cold
    default:
        return 35; // snowflake, Gadgetbridge's fallback
    }
}

// The name an OpenWeather condition id has in the API's own text. Hour
// entries have no free-form description, and Gadgetbridge takes the localized
// condition name here; English only for now.
QString conditionName(int owmCode)
{
    switch (owmCode) {
    case 200:
        return QStringLiteral("thunderstorm with light rain");
    case 201:
        return QStringLiteral("thunderstorm with rain");
    case 202:
        return QStringLiteral("thunderstorm with heavy rain");
    case 210:
        return QStringLiteral("light thunderstorm");
    case 211:
        return QStringLiteral("thunderstorm");
    case 212:
        return QStringLiteral("heavy thunderstorm");
    case 221:
        return QStringLiteral("ragged thunderstorm");
    case 230:
        return QStringLiteral("thunderstorm with light drizzle");
    case 231:
        return QStringLiteral("thunderstorm with drizzle");
    case 232:
        return QStringLiteral("thunderstorm with heavy drizzle");
    case 300:
        return QStringLiteral("light intensity drizzle");
    case 301:
        return QStringLiteral("drizzle");
    case 302:
        return QStringLiteral("heavy intensity drizzle");
    case 310:
        return QStringLiteral("light intensity drizzle rain");
    case 311:
        return QStringLiteral("drizzle rain");
    case 312:
        return QStringLiteral("heavy intensity drizzle rain");
    case 313:
        return QStringLiteral("shower rain and drizzle");
    case 314:
        return QStringLiteral("heavy shower rain and drizzle");
    case 321:
        return QStringLiteral("shower drizzle");
    case 500:
        return QStringLiteral("light rain");
    case 501:
        return QStringLiteral("moderate rain");
    case 502:
        return QStringLiteral("heavy intensity rain");
    case 503:
        return QStringLiteral("very heavy rain");
    case 504:
        return QStringLiteral("extreme rain");
    case 511:
        return QStringLiteral("freezing rain");
    case 520:
        return QStringLiteral("light intensity shower rain");
    case 521:
        return QStringLiteral("shower rain");
    case 522:
        return QStringLiteral("heavy intensity shower rain");
    case 531:
        return QStringLiteral("ragged shower rain");
    case 600:
        return QStringLiteral("light snow");
    case 601:
        return QStringLiteral("snow");
    case 602:
        return QStringLiteral("heavy snow");
    case 611:
        return QStringLiteral("sleet");
    case 612:
        return QStringLiteral("light shower sleet");
    case 613:
        return QStringLiteral("shower sleet");
    case 615:
        return QStringLiteral("light rain and snow");
    case 616:
        return QStringLiteral("rain and snow");
    case 620:
        return QStringLiteral("light shower snow");
    case 621:
        return QStringLiteral("shower snow");
    case 622:
        return QStringLiteral("heavy shower snow");
    case 701:
        return QStringLiteral("mist");
    case 711:
        return QStringLiteral("smoke");
    case 721:
        return QStringLiteral("haze");
    case 731:
        return QStringLiteral("sand/dust whirls");
    case 741:
        return QStringLiteral("fog");
    case 751:
        return QStringLiteral("sand");
    case 761:
        return QStringLiteral("dust");
    case 762:
        return QStringLiteral("volcanic ash");
    case 771:
        return QStringLiteral("squalls");
    case 781:
        return QStringLiteral("tornado");
    case 800:
        return QStringLiteral("clear sky");
    case 801:
        return QStringLiteral("few clouds");
    case 802:
        return QStringLiteral("scattered clouds");
    case 803:
        return QStringLiteral("broken clouds");
    case 804:
        return QStringLiteral("overcast clouds");
    case 900:
        return QStringLiteral("tornado");
    case 901:
        return QStringLiteral("tropical storm");
    case 902:
        return QStringLiteral("hurricane");
    case 903:
        return QStringLiteral("cold");
    case 904:
        return QStringLiteral("hot");
    case 905:
        return QStringLiteral("windy");
    case 906:
        return QStringLiteral("hail");
    case 951:
        return QStringLiteral("calm");
    case 952:
        return QStringLiteral("light breeze");
    case 953:
        return QStringLiteral("gentle breeze");
    case 954:
        return QStringLiteral("moderate breeze");
    case 955:
        return QStringLiteral("fresh breeze");
    case 956:
        return QStringLiteral("strong breeze");
    case 957:
        return QStringLiteral("high wind, near gale");
    case 958:
        return QStringLiteral("gale");
    case 959:
        return QStringLiteral("severe gale");
    case 960:
        return QStringLiteral("storm");
    case 961:
        return QStringLiteral("violent storm");
    case 962:
        return QStringLiteral("hurricane");
    default:
        return QStringLiteral("unknown");
    }
}

// One day record. sunrise and sunset are 0 when unknown and left out, as
// Gadgetbridge leaves its null fields out. The API has no sun times per day,
// so only today carries them; Gadgetbridge computes the rest with SolarEvents.
// No wind record at all: CurrentWeather keeps the daily wind extremes in
// uint8 fields that overflow (windMin stays 255, see the FIT weather path).
QJsonObject dayEntry(const QDate& date, const QString& description, qreal minKelvin,
    qreal maxKelvin, int precipProb, int humidity, int conditionCode,
    qlonglong sunrise, qlonglong sunset, const QString& tempUnit, bool version2)
{
    QJsonObject entry;
    // v1 counts 1 = Sunday .. 7 = Saturday; v2 counts 1 = Monday .. 7 =
    // Sunday, the numbering QDate already uses.
    entry.insert(QStringLiteral("dayOfWeek"), version2 ? date.dayOfWeek() : date.dayOfWeek() % 7 + 1);
    entry.insert(QStringLiteral("description"), description);
    entry.insert(QStringLiteral("summary"), description);
    entry.insert(QStringLiteral("high"), temperatureValue(maxKelvin, tempUnit));
    entry.insert(QStringLiteral("low"), temperatureValue(minKelvin, tempUnit));
    if (precipProb >= 0) {
        entry.insert(QStringLiteral("precipProb"), precipProb);
    }
    entry.insert(QStringLiteral("icon"), garminIcon(conditionCode));
    if (sunrise > 0 && sunset > 0) {
        entry.insert(QStringLiteral("epochSunrise"), double(sunrise));
        entry.insert(QStringLiteral("epochSunset"), double(sunset));
    }
    entry.insert(QStringLiteral("humidity"), humidity);
    return entry;
}

// OpenWeather reports no precipitation probability for current conditions, so
// today takes the highest probability among its hourly entries. Returns -1
// when the API had no entry for today.
int todayPrecipProbability(const CurrentWeather* weather, const QDate& day)
{
    int best = -1;
    for (int i = 0; i < weather->hourCount(); ++i) {
        const CurrentWeather::Hour& hour = weather->hour(i);
        if (QDateTime::fromMSecsSinceEpoch(hour.dateTime() * 1000).date() != day) {
            continue;
        }
        best = qMax(best, qRound(hour.precipProbability() * 100.0));
    }
    return best;
}
} // namespace

QByteArray GarminWeatherJson::dayForecast(const CurrentWeather* weather, int duration,
    const QString& tempUnit, bool version2)
{
    if (!haveWeather(weather) || duration <= 0) {
        return QByteArray();
    }

    // The forecast list starts tomorrow: handleForecast() folds today away.
    // Build today from the current-conditions reply, like Gadgetbridge does,
    // and date the series from the data's own timestamp.
    const QDate base = QDateTime::fromMSecsSinceEpoch(weather->dateTime() * 1000).date();
    QJsonArray days;
    days.append(dayEntry(base, weather->description(), weather->minTemperature(),
        weather->maxTemperature(), todayPrecipProbability(weather, base),
        weather->humidity(), weather->weatherCode(),
        weather->sunrise(), weather->sunset(), tempUnit, version2));

    // Gadgetbridge answers duration entries in total: today plus duration-1
    // forecast days.
    const int forecastDays = qMin(duration, weather->forecastCount()) - 1;
    for (int i = 0; i < forecastDays; ++i) {
        const CurrentWeather::Forecast& f = weather->forecast(i);
        // CurrentWeather fills the day details only when the API had an entry
        // around midday; without one dateTime() is still 0.
        int precipProb = -1;
        if (f.dateTime() > 0) {
            precipProb = qRound(f.precipProbability() * 100.0);
        }
        QString description = f.description();
        if (description.isEmpty()) {
            description = conditionName(f.weatherCode());
        }
        days.append(dayEntry(base.addDays(i + 1), description, f.minTemperature(),
            f.maxTemperature(), precipProb, int(f.humidity()),
            f.weatherCode(), 0, 0, tempUnit, version2));
    }
    return QJsonDocument(days).toJson(QJsonDocument::Compact);
}

QByteArray GarminWeatherJson::hourForecast(const CurrentWeather* weather, int duration,
    const QString& tempUnit, const QString& speedUnit)
{
    if (!haveWeather(weather) || duration <= 0 || weather->hourCount() <= 0) {
        return QByteArray();
    }
    QJsonArray hours;
    const qlonglong first = weather->hour(0).dateTime();
    for (int i = 0; i < duration; ++i) {
        const qlonglong at = first + qlonglong(i) * 3600;
        const CurrentWeather::Hour hour = GarminWeatherMessage::interpolatedHour(weather, at);
        QJsonObject entry;
        entry.insert(QStringLiteral("epochSeconds"), double(at));
        entry.insert(QStringLiteral("description"), conditionName(hour.weatherCode()));
        entry.insert(QStringLiteral("temp"), temperatureValue(hour.temperature(), tempUnit));
        entry.insert(QStringLiteral("precipProb"), qRound(hour.precipProbability() * 100.0));
        entry.insert(QStringLiteral("wind"), windValue(hour.windSpeed(), hour.windDirection(), speedUnit));
        entry.insert(QStringLiteral("icon"), garminIcon(hour.weatherCode()));
        entry.insert(QStringLiteral("relativeHumidity"), hour.humidity());
        // No dewPoint, uvIndex, feelsLikeTemperature, visibility, pressure or
        // cloudCover entries: CurrentWeather has no values for them, and
        // Gadgetbridge leaves most of those out as TODOs as well.
        hours.append(entry);
    }
    return QJsonDocument(hours).toJson(QJsonDocument::Compact);
}

QByteArray GarminWeatherJson::current(const CurrentWeather* weather,
    const QString& tempUnit, const QString& speedUnit)
{
    if (!haveWeather(weather)) {
        return QByteArray();
    }
    QJsonObject entry;
    entry.insert(QStringLiteral("epochSeconds"), double(weather->dateTime()));
    entry.insert(QStringLiteral("temperature"), temperatureValue(weather->temperature(), tempUnit));
    entry.insert(QStringLiteral("description"), weather->description());
    entry.insert(QStringLiteral("icon"), garminIcon(weather->weatherCode()));
    entry.insert(QStringLiteral("relativeHumidity"), weather->humidity());
    entry.insert(QStringLiteral("wind"), windValue(weather->windSpeed(), weather->windDeg(), speedUnit));
    entry.insert(QStringLiteral("locationName"), weather->city()->name());
    // No feelsLikeTemperature, dewPoint, visibility or pressure entries: the
    // OpenWeather reply carries pressure but CurrentWeather does not parse it.
    entry.insert(QStringLiteral("cloudCoverage"), weather->clouds());
    return QJsonDocument(entry).toJson(QJsonDocument::Compact);
}
