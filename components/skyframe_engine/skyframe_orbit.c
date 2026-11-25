/* Space station passes: see skyframe_orbit.h.
   SGP4 near-Earth propagation after Vallado et al., "Revisiting Spacetrack Report #3" (2006),
   WGS-72 constants as used for published element sets. The sun is the low-precision almanac
   formula; Earth's shadow is a cylinder (enough to tell a sunlit pass from a dark one). */
#include "skyframe_orbit.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define TWOPI (2.0 * PI)
#define DEG (PI / 180.0)
/* WGS-72 */
#define RE 6378.135                         /* km */
#define MU 398600.8
#define J2 0.001082616
#define J3 -0.00000253881
#define J4 -0.00000165597
#define J3OJ2 (J3 / J2)
static double xke(void) { return 60.0 / sqrt(RE * RE * RE / MU); }

/* ---- TLE ---- */
static bool checksum_ok(const char *l)
{
    int sum = 0;
    for (int i = 0; i < 68; i++) {
        if (isdigit((unsigned char)l[i])) sum += l[i] - '0';
        else if (l[i] == '-') sum++;
    }
    return isdigit((unsigned char)l[68]) && sum % 10 == l[68] - '0';
}
static double field(const char *l, int from, int len)   /* 1-based column, like the TLE spec */
{
    char b[24];
    if (len >= (int)sizeof(b)) return 0;
    memcpy(b, l + from - 1, (size_t)len);
    b[len] = 0;
    return atof(b);
}
/* "12345-5" means 0.12345e-5 (an implied decimal point and exponent). */
static double exp_field(const char *l, int from)
{
    char b[16];
    int k = 0;
    char sign = l[from - 1];
    b[k++] = '0'; b[k++] = '.';
    for (int i = 0; i < 5; i++) b[k++] = l[from + i];
    b[k++] = 'e';
    b[k++] = l[from + 5];
    b[k++] = l[from + 6];
    b[k] = 0;
    double v = atof(b);
    return sign == '-' ? -v : v;
}
static double days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (double)(era * 146097 + doe - 719468);
}

static void sgp4_init(sf_orbit_t *o)
{
    const double ss = 78.0 / RE + 1.0, x2o3 = 2.0 / 3.0;
    double qzms2t = pow((120.0 - 78.0) / RE, 4);
    double ecco = o->ecco, inclo = o->inclo, eccsq = ecco * ecco, omeosq = 1.0 - eccsq, rteosq = sqrt(omeosq);
    double cosio = cos(inclo), cosio2 = cosio * cosio;
    /* un-Kozai the mean motion */
    double ak = pow(xke() / o->no_kozai, x2o3);
    double d1 = 0.75 * J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    o->no = o->no_kozai / (1.0 + del);
    o->ao = pow(xke() / o->no, x2o3);
    double sinio = sin(inclo), po = o->ao * omeosq, con42 = 1.0 - 5.0 * cosio2;
    o->con41 = -con42 - cosio2 - cosio2;
    double posq = po * po, rp = o->ao * (1.0 - ecco);
    o->isimp = rp < 220.0 / RE + 1.0;
    double sfour = ss, qzms24 = qzms2t, perige = (rp - 1.0) * RE;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        qzms24 = pow((120.0 - sfour) / RE, 4);
        sfour = sfour / RE + 1.0;
    }
    double pinvsq = 1.0 / posq, tsi = 1.0 / (o->ao - sfour);
    o->eta = o->ao * ecco * tsi;
    double etasq = o->eta * o->eta, eeta = ecco * o->eta, psisq = fabs(1.0 - etasq);
    double coef = qzms24 * pow(tsi, 4), coef1 = coef / pow(psisq, 3.5);
    double cc2 = coef1 * o->no * (o->ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                 0.375 * J2 * tsi / psisq * o->con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    o->cc1 = o->bstar * cc2;
    double cc3 = ecco > 1.0e-4 ? -2.0 * coef * tsi * J3OJ2 * o->no * sinio / ecco : 0.0;
    o->x1mth2 = 1.0 - cosio2;
    o->cc4 = 2.0 * o->no * coef1 * o->ao * omeosq *
             (o->eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq) -
              J2 * tsi / (o->ao * psisq) *
              (-3.0 * o->con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
               0.75 * o->x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cos(2.0 * o->argpo)));
    o->cc5 = 2.0 * coef1 * o->ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
    double cosio4 = cosio2 * cosio2, temp1 = 1.5 * J2 * pinvsq * o->no, temp2 = 0.5 * temp1 * J2 * pinvsq,
           temp3 = -0.46875 * J4 * pinvsq * pinvsq * o->no;
    o->mdot = o->no + 0.5 * temp1 * rteosq * o->con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
    o->argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
                 temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
    double xhdot1 = -temp1 * cosio;
    o->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
    o->omgcof = o->bstar * cc3 * cos(o->argpo);
    o->xmcof = ecco > 1.0e-4 ? -x2o3 * coef * o->bstar / eeta : 0.0;
    o->nodecf = 3.5 * omeosq * xhdot1 * o->cc1;
    o->t2cof = 1.5 * o->cc1;
    o->xlcof = fabs(cosio + 1.0) > 1.5e-12 ? -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio)
                                            : -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / 1.5e-12;
    o->aycof = -0.5 * J3OJ2 * sinio;
    o->delmo = pow(1.0 + o->eta * cos(o->mo), 3);
    o->sinmao = sin(o->mo);
    o->x7thm1 = 7.0 * cosio2 - 1.0;
    if (!o->isimp) {
        double cc1sq = o->cc1 * o->cc1;
        o->d2 = 4.0 * o->ao * tsi * cc1sq;
        double temp = o->d2 * tsi * o->cc1 / 3.0;
        o->d3 = (17.0 * o->ao + sfour) * temp;
        o->d4 = 0.5 * temp * o->ao * tsi * (221.0 * o->ao + 31.0 * sfour) * o->cc1;
        o->t3cof = o->d2 + 2.0 * cc1sq;
        o->t4cof = 0.25 * (3.0 * o->d3 + o->cc1 * (12.0 * o->d2 + 10.0 * cc1sq));
        o->t5cof = 0.2 * (3.0 * o->d4 + 12.0 * o->cc1 * o->d3 + 6.0 * o->d2 * o->d2 + 15.0 * cc1sq * (2.0 * o->d2 + cc1sq));
    }
}

bool sf_orbit_parse(const char *l1, const char *l2, sf_orbit_t *o)
{
    if (!l1 || !l2 || !o) return false;
    memset(o, 0, sizeof(*o));
    if (strlen(l1) < 69 || strlen(l2) < 69 || l1[0] != '1' || l2[0] != '2') return false;
    if (!checksum_ok(l1) || !checksum_ok(l2)) return false;
    int yy = (int)field(l1, 19, 2);
    double day = field(l1, 21, 12);
    int year = yy < 57 ? 2000 + yy : 1900 + yy;
    o->epoch_unix = (days_from_civil(year, 1, 1) + day - 1.0) * 86400.0;
    o->bstar = exp_field(l1, 54);
    o->inclo = field(l2, 9, 8) * DEG;
    o->nodeo = field(l2, 18, 8) * DEG;
    char e[10] = "0.";
    memcpy(e + 2, l2 + 26, 7);
    e[9] = 0;
    o->ecco = atof(e);
    o->argpo = field(l2, 35, 8) * DEG;
    o->mo = field(l2, 44, 8) * DEG;
    double revs = field(l2, 53, 11);
    if (revs < 6.4 || o->ecco >= 1.0) return false;   /* period over 225 min: deep space, not modelled */
    o->no_kozai = revs * TWOPI / 1440.0;
    sgp4_init(o);
    o->valid = true;
    return true;
}

bool sf_orbit_parse_text(const char *text, size_t len, sf_orbit_t *out)
{
    char l1[80] = "", l2[80] = "", line[80];
    size_t i = 0;
    while (i < len) {
        size_t n = 0;
        while (i < len && text[i] != '\n' && text[i] != '\r') { if (n < sizeof(line) - 1) line[n++] = text[i]; i++; }
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        line[n] = 0;
        while (n && line[n - 1] == ' ') line[--n] = 0;
        if (line[0] == '1' && line[1] == ' ' && !l1[0]) strcpy(l1, line);
        else if (line[0] == '2' && line[1] == ' ' && l1[0] && !l2[0]) strcpy(l2, line);
    }
    return sf_orbit_parse(l1, l2, out);
}

bool sf_orbit_position(const sf_orbit_t *o, double unix_time, double r[3])
{
    if (!o || !o->valid) return false;
    double t = (unix_time - o->epoch_unix) / 60.0;   /* minutes since epoch */
    double xmdf = o->mo + o->mdot * t, argpdf = o->argpo + o->argpdot * t, nodedf = o->nodeo + o->nodedot * t;
    double argpm = argpdf, mm = xmdf, t2 = t * t;
    double nodem = nodedf + o->nodecf * t2, tempa = 1.0 - o->cc1 * t, tempe = o->bstar * o->cc4 * t, templ = o->t2cof * t2;
    if (!o->isimp) {
        double delomg = o->omgcof * t;
        double delm = o->xmcof * (pow(1.0 + o->eta * cos(xmdf), 3) - o->delmo);
        double temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        double t3 = t2 * t, t4 = t3 * t;
        tempa = tempa - o->d2 * t2 - o->d3 * t3 - o->d4 * t4;
        tempe = tempe + o->bstar * o->cc5 * (sin(mm) - o->sinmao);
        templ = templ + o->t3cof * t3 + t4 * (o->t4cof + t * o->t5cof);
    }
    double am = pow(xke() / o->no, 2.0 / 3.0) * tempa * tempa;
    double em = o->ecco - tempe;
    if (em >= 1.0 || em < -0.001 || am < 0.95) return false;   /* decayed */
    if (em < 1.0e-6) em = 1.0e-6;
    mm = mm + o->no * templ;
    double xlm = mm + argpm + nodem;
    nodem = fmod(nodem, TWOPI);
    argpm = fmod(argpm, TWOPI);
    xlm = fmod(xlm, TWOPI);
    mm = fmod(xlm - argpm - nodem, TWOPI);
    double sinip = sin(o->inclo), cosip = cos(o->inclo);
    /* long-period periodics */
    double axnl = em * cos(argpm);
    double temp = 1.0 / (am * (1.0 - em * em));
    double aynl = em * sin(argpm) + temp * o->aycof;
    double xl = mm + argpm + nodem + temp * o->xlcof * axnl;
    /* Kepler's equation */
    double u = fmod(xl - nodem, TWOPI), eo1 = u, tem5 = 9999.9, sineo1 = 0, coseo1 = 1;
    for (int k = 0; fabs(tem5) >= 1.0e-12 && k < 10; k++) {
        sineo1 = sin(eo1);
        coseo1 = cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (fabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
        eo1 += tem5;
    }
    /* short-period periodics */
    double ecose = axnl * coseo1 + aynl * sineo1, esine = axnl * sineo1 - aynl * coseo1;
    double el2 = axnl * axnl + aynl * aynl, pl = am * (1.0 - el2);
    if (pl < 0.0) return false;
    double rl = am * (1.0 - ecose), betal = sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    double sinu = am / rl * (sineo1 - aynl - axnl * temp), cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = atan2(sinu, cosu), sin2u = (cosu + cosu) * sinu, cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    double temp1 = 0.5 * J2 * temp, temp2 = temp1 * temp;
    double mrt = rl * (1.0 - 1.5 * temp2 * betal * o->con41) + 0.5 * temp1 * o->x1mth2 * cos2u;
    su = su - 0.25 * temp2 * o->x7thm1 * sin2u;
    double xnode = nodem + 1.5 * temp2 * cosip * sin2u;
    double xinc = o->inclo + 1.5 * temp2 * cosip * sinip * cos2u;
    double sinsu = sin(su), cossu = cos(su), snod = sin(xnode), cnod = cos(xnode), sini = sin(xinc), cosi = cos(xinc);
    double xmx = -snod * cosi, xmy = cnod * cosi;
    double ux = xmx * sinsu + cnod * cossu, uy = xmy * sinsu + snod * cossu, uz = sini * sinsu;
    if (mrt < 1.0) return false;   /* below the surface: decayed */
    r[0] = mrt * ux * RE;
    r[1] = mrt * uy * RE;
    r[2] = mrt * uz * RE;
    return true;
}

/* ---- Looking up ---- */
static double gmst(double unix_time)   /* Greenwich mean sidereal time, radians (IAU-82) */
{
    double jd = unix_time / 86400.0 + 2440587.5, tut1 = (jd - 2451545.0) / 36525.0;
    double s = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 + (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;
    double g = fmod(s * DEG / 240.0, TWOPI);
    return g < 0 ? g + TWOPI : g;
}
static void sun_eci(double unix_time, double s[3])   /* unit vector toward the sun */
{
    double jd = unix_time / 86400.0 + 2440587.5, t = (jd - 2451545.0) / 36525.0;
    double lm = fmod(280.460 + 36000.771 * t, 360.0) * DEG, m = fmod(357.5291092 + 35999.05034 * t, 360.0) * DEG;
    double le = lm + (1.914666471 * sin(m) + 0.019994643 * sin(2.0 * m)) * DEG;
    double eps = (23.439291 - 0.0130042 * t) * DEG;
    s[0] = cos(le);
    s[1] = cos(eps) * sin(le);
    s[2] = sin(eps) * sin(le);
}
/* ECI vector -> azimuth / elevation from the observer (km vectors in ECI, observer computed here). */
static void topocentric(const double eci[3], double lat, double lon, double unix_time, bool is_direction,
                        double *az, double *el, double *range)
{
    double g = gmst(unix_time), cg = cos(g), sg = sin(g);
    double x = cg * eci[0] + sg * eci[1], y = -sg * eci[0] + cg * eci[1], z = eci[2];   /* -> Earth-fixed */
    double phi = lat * DEG, lam = lon * DEG, f = 1.0 / 298.26, e2 = f * (2.0 - f);
    double sp = sin(phi), cp = cos(phi), sl = sin(lam), cl = cos(lam);
    double c = RE / sqrt(1.0 - e2 * sp * sp);
    double ox = c * cp * cl, oy = c * cp * sl, oz = c * (1.0 - e2) * sp;
    double rx = is_direction ? x : x - ox, ry = is_direction ? y : y - oy, rz = is_direction ? z : z - oz;
    double south = sp * cl * rx + sp * sl * ry - cp * rz;
    double east = -sl * rx + cl * ry;
    double up = cp * cl * rx + cp * sl * ry + sp * rz;
    double r = sqrt(rx * rx + ry * ry + rz * rz);
    *el = asin(up / r) / DEG;
    double a = atan2(east, -south) / DEG;
    *az = a < 0 ? a + 360.0 : a;
    if (range) *range = r;
}

bool sf_orbit_look(const sf_orbit_t *o, double lat, double lon, double unix_time, sf_look_t *out)
{
    double r[3], s[3];
    if (!out || !sf_orbit_position(o, unix_time, r)) return false;
    topocentric(r, lat, lon, unix_time, false, &out->az_deg, &out->el_deg, &out->range_km);
    sun_eci(unix_time, s);
    double saz, sel;
    topocentric(s, lat, lon, unix_time, true, &saz, &sel, NULL);
    out->sun_el_deg = sel;
    /* in sunlight unless behind the Earth, inside its shadow cylinder */
    double along = r[0] * s[0] + r[1] * s[1] + r[2] * s[2];
    double px = r[0] - along * s[0], py = r[1] - along * s[1], pz = r[2] - along * s[2];
    out->sunlit = along > 0.0 || sqrt(px * px + py * py + pz * pz) > RE;
    return true;
}

static bool visible(const sf_look_t *l)
{
    return l->el_deg >= SF_PASS_MIN_EL && l->sunlit && l->sun_el_deg <= SF_PASS_DARK_SUN_EL;
}
/* Elevation only (no sun): enough to find when it is above the horizon. */
static bool elevation(const sf_orbit_t *o, double lat, double lon, double unix_time, double *el)
{
    double r[3], az;
    if (!sf_orbit_position(o, unix_time, r)) return false;
    topocentric(r, lat, lon, unix_time, false, &az, el, NULL);
    return true;
}
bool sf_orbit_next_pass(const sf_orbit_t *o, double lat, double lon, time_t from, int hours,
                        double min_peak_deg, sf_pass_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!o || !o->valid || hours <= 0) return false;
    /* Every minute, is it up at all? (A pass above the horizon lasts several minutes.) Then
       through that pass in 10 s steps, with the sun and the shadow. */
    const int coarse = 60, fine = 10;
    const time_t last = from + (time_t)hours * 3600;
    for (time_t t = from; t <= last; t += coarse) {
        double el;
        if (!elevation(o, lat, lon, (double)t, &el)) return false;
        if (el < 0.0) continue;
        sf_pass_t cur = {0};
        bool in = false;
        time_t u = t - coarse < from ? from : t - coarse;
        for (; u <= last; u += fine) {
            sf_look_t l;
            if (!sf_orbit_look(o, lat, lon, (double)u, &l)) return false;
            if (visible(&l)) {
                if (!in) { memset(&cur, 0, sizeof(cur)); cur.start = u; cur.start_az_deg = l.az_deg; in = true; }
                if (l.el_deg > cur.max_el_deg) cur.max_el_deg = l.el_deg;
                cur.end = u;
                cur.end_az_deg = l.az_deg;
            } else if (in) {
                in = false;
                if (cur.max_el_deg >= min_peak_deg) { cur.valid = true; *out = cur; return true; }
            }
            if (l.el_deg < 0.0 && u > t) break;   /* set: back to the coarse search */
        }
        if (in && cur.max_el_deg >= min_peak_deg) { cur.valid = true; *out = cur; return true; }
        t = u;
    }
    return false;
}

const char *sf_compass_word(double az)
{
    static const char *const w[8] = {"north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest"};
    az = fmod(az, 360.0);
    if (az < 0) az += 360.0;
    return w[(int)((az + 22.5) / 45.0) % 8];
}
