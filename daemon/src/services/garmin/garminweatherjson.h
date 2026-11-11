#ifndef GARMINWEATHERJSON_H
#define GARMINWEATHERJSON_H

#include <QByteArray>
#include <QString>

class CurrentWeather;

// JSON answers for the weather endpoints the watch fetches through the
// HttpService tunnel. The field names follow Gadgetbridge's WeatherInterceptor
// so the watch parser sees what it expects. Each builder returns compact JSON
// bytes, or an empty byte array when the phone has no usable data.
class GarminWeatherJson {
public:
    // duration comes from the request query; version2 selects the v2 field
    // numbering (dayOfWeek 1 = Monday, as QDate already counts it). The day
    // query usually has no speedUnit, so the caller passes Gadgetbridge's
    // KILOMETERS_PER_HOUR default for this endpoint.
    static QByteArray dayForecast(const CurrentWeather* weather, int duration,
        const QString& tempUnit, const QString& speedUnit, bool version2);
    // The entries are one hour apart, interpolated from the 3-hour API list,
    // because the watch rejects a hourly series with gaps.
    static QByteArray hourForecast(const CurrentWeather* weather, int duration,
        const QString& tempUnit, const QString& speedUnit);
    static QByteArray current(const CurrentWeather* weather,
        const QString& tempUnit, const QString& speedUnit);
};

#endif // GARMINWEATHERJSON_H
