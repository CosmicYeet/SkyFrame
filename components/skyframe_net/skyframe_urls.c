#include "skyframe_urls.h"
#include <math.h>
#include <stdio.h>
static bool valid(double lat,double lon){return isfinite(lat)&&isfinite(lon)&&fabs(lat)<=90&&fabs(lon)<=180;}
bool sf_aircraft_url(char *out,size_t cap,double lat,double lon,double miles){
 if(!out||cap==0||!valid(lat,lon)||!isfinite(miles)||miles<1||miles>100)return false;
 /* adsb.fi uses nautical miles in the dist path, not statute miles. */
 double nautical=miles/1.150779448;
 int n=snprintf(out,cap,"https://opendata.adsb.fi/api/v3/lat/%.5f/lon/%.5f/dist/%.1f",lat,lon,nautical);
 return n>=0&&(size_t)n<cap;
}
bool sf_weather_url(char *out,size_t cap,double lat,double lon){
 if(!out||cap==0||!valid(lat,lon))return false;
 int n=snprintf(out,cap,"https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
   "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,wind_direction_10m,weather_code,is_day"
   "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
   "&hourly=temperature_2m,weather_code,is_day,precipitation_probability"
   "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=2",lat,lon);
 return n>=0&&(size_t)n<cap;
}
