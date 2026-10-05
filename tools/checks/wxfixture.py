"""One canned Open-Meteo reply for the Weather checks: the same shape and field list as the
single request kernel.c's weather_fetch makes (current + next 24 hours + seven days), with the
real reply's traps kept: current_units, hourly_units and daily_units repeat every key with
string values before the real objects. 2026-09-20 is a Sunday, so the week reads Sun..Sat and
the hourly strip starts at 16:00 ("Now", then 5 PM, 6 PM ...)."""
import json

DAYS = ["2026-09-20", "2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-26"]
HOURS_T = [14.2, 14.0, 13.4, 12.6, 12.1, 11.8, 11.5, 11.2, 10.9, 10.7, 10.5, 10.4, 10.6, 11.3, 12.8, 14.4, 15.9, 17.1, 17.6, 17.3, 16.4, 15.2, 14.1, 13.2]
HOURS_C = [3, 3, 61, 61, 3, 3, 3, 2, 2, 1, 0, 0, 0, 1, 1, 2, 2, 0, 0, 1, 2, 3, 3, 3]
HOURS_P = [20, 25, 60, 70, 40, 20, 10, 10, 5, 0, 0, 0, 0, 0, 0, 5, 5, 0, 0, 5, 10, 15, 20, 20]
HOURS_D = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0]
DAILY_CODE = [3, 0, 61, 71, 95, 45, 80]
DAILY_HI = [17.6, 21.2, 15.4, 3.1, 16.5, 12.0, 14.4]
DAILY_LO = [9.4, 8.5, 9.2, -2.6, 9.0, 6.1, 7.7]
DAILY_RISE = ["07:08", "07:09", "07:11", "07:12", "07:14", "07:15", "07:17"]
DAILY_SET = ["19:21", "19:19", "19:16", "19:14", "19:11", "19:09", "19:07"]
DAILY_UV = [3.4, 5.1, 2.0, 0.9, 2.6, 1.2, 6.3]
DAILY_POP = [25, 5, 70, 55, 90, 35, 60]

def reply():
    hours = ["2026-09-%02dT%02d:00" % (20 + (16 + i) // 24, (16 + i) % 24) for i in range(24)]
    o = {"latitude": 49.09, "longitude": -122.57, "generationtime_ms": 0.88, "utc_offset_seconds": -25200,
         "timezone": "America/Vancouver", "timezone_abbreviation": "GMT-7", "elevation": 6.0,
         "current_units": {"time": "iso8601", "interval": "seconds", "temperature_2m": "°C", "apparent_temperature": "°C",
                           "relative_humidity_2m": "%", "wind_speed_10m": "km/h", "weather_code": "wmo code", "pressure_msl": "hPa", "visibility": "m", "is_day": ""},
         "current": {"time": "2026-09-20T16:15", "interval": 900, "temperature_2m": 14.2, "apparent_temperature": 12.8, "relative_humidity_2m": 69,
                     "wind_speed_10m": 11.4, "weather_code": 3, "pressure_msl": 1017.6, "visibility": 24100.00, "is_day": 1},
         "hourly_units": {"time": "iso8601", "temperature_2m": "°C", "precipitation_probability": "%", "weather_code": "wmo code", "is_day": ""},
         "hourly": {"time": hours, "temperature_2m": HOURS_T, "precipitation_probability": HOURS_P, "weather_code": HOURS_C, "is_day": HOURS_D},
         "daily_units": {"time": "iso8601", "weather_code": "wmo code", "temperature_2m_max": "°C", "temperature_2m_min": "°C",
                         "sunrise": "iso8601", "sunset": "iso8601", "uv_index_max": "", "precipitation_probability_max": "%"},
         "daily": {"time": DAYS, "weather_code": DAILY_CODE, "temperature_2m_max": DAILY_HI, "temperature_2m_min": DAILY_LO,
                   "sunrise": [d + "T" + t for d, t in zip(DAYS, DAILY_RISE)], "sunset": [d + "T" + t for d, t in zip(DAYS, DAILY_SET)],
                   "uv_index_max": DAILY_UV, "precipitation_probability_max": DAILY_POP}}
    return json.dumps(o, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

WANT_FIELDS = ("apparent_temperature", "relative_humidity_2m", "wind_speed_10m", "pressure_msl", "visibility",
               "hourly=temperature_2m,precipitation_probability,weather_code,is_day",
               "daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,precipitation_probability_max",
               "forecast_days=7", "forecast_hours=24", "timezone=auto")
WEEK_ROW = "wxrow=7 Sun,Mon,Tue,Wed,Thu,Fri,Sat facts=yes"
SAMPLE_ROW = "wxrow=7 Mon,Tue,Wed,Thu,Fri,Sat,Sun facts=yes"
