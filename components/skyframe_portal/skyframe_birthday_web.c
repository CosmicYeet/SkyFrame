/* Birthdays web page (http://skyframe.local/birthdays): see, add and remove the family
   birthdays in /birthdays.txt, each with an optional photo in /birthdays/, choose the
   owner (who gets the special birthday screen) and edit the owner's message. The
   browser crops photos to a square and shrinks them before uploading. Nothing about
   the family is in the firmware: it is all in the file. */
#include "skyframe_portal.h"
#include "skyframe_sd.h"
#include "skyframe_jpeg.h"
#include "skyframe_engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define BIRTHDAY_FILE   SF_SD_ROOT "/birthdays.txt"
#define BIRTHDAY_PHOTOS SF_SD_ROOT "/birthdays"
#define BIRTHDAY_TMP    SF_SD_ROOT "/birthdays.tmp"   /* the next file, while it's written */
#define FILE_MAX        8192
#define OUT_MAX         (FILE_MAX + 256)
#define PHOTO_MAX       (2 * 1024 * 1024)
static const char *TAG = "birthday_web";

static const char page_html[] =
"<style>.p{display:flex;align-items:center;gap:10px;padding:12px 0;border-bottom:1px solid var(--line)}"
".p img,.p .i{width:56px;height:56px;border-radius:50%;object-fit:cover;background:var(--soft);color:var(--acc);"
"display:flex;align-items:center;justify-content:center;font-size:24px;font-weight:700;flex:none}"
".p div{flex:1;min-width:0}.p b{overflow-wrap:anywhere}.p small{display:block;color:var(--mut)}</style>"
"<section class=card><h2>Birthdays</h2><p class=sub>On each of these days SkyFrame shows a birthday screen.</p>"
"<div id=l>Loading&hellip;</div></section>"
"<section class=card><form id=a><h2>Add a birthday</h2>"
"<label>Name<input id=n required maxlength=30 autocomplete=off></label>"
"<label>Birthday<input id=d type=date required>"
"<span class=hint>Don't know the year? Pick any year and tick the box below.</span></label>"
"<label class=chk><input id=u type=checkbox>Birth year unknown</label>"
"<label class=chk><input id=o type=checkbox>Owner of this SkyFrame (gets the special birthday screen)</label>"
"<label>Photo (optional)<input id=f type=file accept='image/*'>"
"<span class=hint>Cropped to a circle from the middle of the picture.</span></label>"
"<p><button>Add birthday</button></p><span id=s class=note></span></form></section>"
"<section class=card><form id=m><h2>Owner's birthday message</h2>"
"<label>Shown on the owner's birthday screen<input id=t maxlength=60><span class=hint>Up to 60 characters.</span></label>"
"<p><button>Save message</button></p><span id=ms class=note></span></form></section><script>"
"const M=['January','February','March','April','May','June','July','August','September','October','November','December'],"
"$=i=>document.getElementById(i);"
"function say(e,t,ok){e.textContent=t;e.className='note'+(ok===undefined?'':ok?' ok':' err')}"
"function when(d){const p=d.split('-');const y=p.length==3?+p[0]:0,m=+p[p.length-2],dd=+p[p.length-1];"
"let t=M[m-1]+' '+dd;if(y){const n=new Date(),mo=n.getMonth()+1;let a=n.getFullYear()-y;"
"if(mo>m||(mo==m&&n.getDate()>dd))a++;t+=', '+y+'  \\u2022  turns '+a}return t}"
"async function shrink(f){const b=await createImageBitmap(f,{imageOrientation:'from-image'}),s=Math.min(b.width,b.height),"
"c=document.createElement('canvas');c.width=c.height=360;"
"c.getContext('2d').drawImage(b,(b.width-s)/2,(b.height-s)/2,s,s,0,0,360,360);"
"return new Promise(r=>c.toBlob(r,'image/jpeg',0.88))}"
"async function post(u,body){const r=await fetch(u,{method:'POST',body:body||''});if(!r.ok)throw new Error(await r.text())}"
"function q(o){return '?'+Object.entries(o).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&')}"
"function pick(cb){const i=document.createElement('input');i.type='file';i.accept='image/*';i.onchange=()=>cb(i.files[0]);i.click()}"
"function btn(t,c,f){const b=document.createElement('button');b.textContent=t;b.className='sm '+c;b.onclick=f;return b}"
"async function list(){const r=await fetch('/api/birthdays'),L=$('l');"
"if(!r.ok){L.textContent=await r.text();L.className='err';return}const j=await r.json();L.innerHTML='';L.className='';"
"if(document.activeElement!=$('t'))$('t').value=j.message;"
"if(!j.people.length){L.textContent='No birthdays yet. Add the first one below.';L.className='empty'}"
"for(const p of j.people){const e=document.createElement('div');e.className='p';"
"const pic=p.photo?Object.assign(new Image(),{src:'/birthday_photo'+q({file:p.photo,v:Date.now()}),alt:''}):"
"Object.assign(document.createElement('span'),{className:'i',textContent:p.name[0]});"
"const t=document.createElement('div');t.innerHTML='<b></b><small></small>';"
"t.firstChild.textContent=p.name+(p.owner?'  \\u2605 owner':'');t.lastChild.textContent=when(p.date);"
"const ph=btn(p.photo?'Photo':'+ Photo','alt',()=>pick(async f=>{try{await post('/birthday_add'+q({name:p.name,date:p.date,owner:p.owner?1:0}),"
"await shrink(f))}catch(x){alert(x.message)}list()}));"
"ph.title=p.photo?'Change photo':'Add a photo';"
"const rm=btn('\\u2715','bad',async()=>{if(confirm('Remove '+p.name+'?')){"
"try{await post('/birthday_delete'+q({name:p.name,date:p.date}))}catch(x){alert(x.message)}list()}});"
"rm.title='Remove';rm.setAttribute('aria-label','Remove '+p.name);"
"e.append(pic,t,ph,rm);L.append(e)}}"
"$('a').onsubmit=async e=>{e.preventDefault();const n=$('n').value.trim(),d=$('d').value,f=$('f').files[0],S=$('s');"
"if(!n||!d)return;say(S,'Saving\\u2026');"
"try{await post('/birthday_add'+q({name:n,date:$('u').checked?d.slice(5):d,owner:$('o').checked?1:0}),f?await shrink(f):'');"
"e.target.reset();say(S,'Added '+n,true)}catch(x){say(S,x.message,false)}list()};"
"$('m').onsubmit=async e=>{e.preventDefault();const S=$('ms');"
"try{await post('/birthday_message'+q({text:$('t').value.trim()}));say(S,'Message saved',true)}catch(x){say(S,x.message,false)}};"
"list();</script>";

static esp_err_t unavailable(httpd_req_t *r)
{
    httpd_resp_set_status(r, "503 Service Unavailable");
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, sf_sd_usb_active()
        ? "The microSD card is connected to a computer. Eject it there, then try again."
        : "No microSD card in SkyFrame.");
}

/* Query value, %XX-decoded, printable ASCII only (may be empty when allow_empty). */
static bool query_value(httpd_req_t *r, const char *key, char *out, size_t cap, bool allow_empty)
{
    char query[320], raw[200];
    out[0] = 0;
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) return false;
    size_t j = 0;
    for (size_t i = 0; raw[i]; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '%' && isxdigit((unsigned char)raw[i+1]) && isxdigit((unsigned char)raw[i+2])) {
            char hex[3] = {raw[i+1], raw[i+2], 0};
            c = (unsigned char)strtol(hex, NULL, 16);
            i += 2;
        } else if (c == '+') c = ' ';
        if (c < 0x20 || c == 0x7f || j + 1 >= cap) return false;   /* UTF-8 (0x80+) is fine: folded later */
        out[j++] = (char)c;
    }
    out[j] = 0;
    return j > 0 || allow_empty;
}
/* A person from the request: a name that can't break the file format, and a valid date. */
static bool request_person(httpd_req_t *r, sf_birthday_t *b)
{
    char name[40], date[16], owner[4], line[64];
    if (!query_value(r, "name", name, sizeof(name), false) || !query_value(r, "date", date, sizeof(date), false)) return false;
    for (const char *p = name; *p; p++) if (strchr(",#\"\\", *p)) return false;   /* keep the file and JSON intact */
    for (const char *p = date; *p; p++) if (!isdigit((unsigned char)*p) && *p != '-') return false;
    if (!strncasecmp(name, "message:", 8)) return false;
    snprintf(line, sizeof(line), "%s, %s", name, date);
    if (!sf_birthday_parse_line(line, b)) return false;
    b->owner = query_value(r, "owner", owner, sizeof(owner), false) && owner[0] == '1';
    return true;
}
static bool same_person(const sf_birthday_t *a, const sf_birthday_t *b)
{
    return !strcasecmp(a->name, b->name) && a->month == b->month && a->day == b->day;
}
static void format_date(const sf_birthday_t *b, char *out, size_t cap)
{
    if (b->year) snprintf(out, cap, "%04d-%02d-%02d", b->year, b->month, b->day);
    else snprintf(out, cap, "%02d-%02d", b->month, b->day);
}
static int format_line(const sf_birthday_t *b, char *out, size_t cap)
{
    char date[16];
    format_date(b, date, sizeof(date));
    return snprintf(out, cap, "%s, %s%s%s%s\n", b->name, date, b->photo[0] ? ", " : "", b->photo, b->owner ? ", owner" : "");
}
/* Whole file into a PSRAM buffer (empty if missing). Card must be held. *whole is false
   when the file is longer than FILE_MAX: the text then ends at the last complete line, and
   it must not be written back (everything past the limit would be lost). */
static char *read_file(bool *whole)
{
    char *text = heap_caps_calloc(1, FILE_MAX + 2, MALLOC_CAP_SPIRAM);
    *whole = true;
    struct stat st;
    if (stat(BIRTHDAY_FILE, &st) != 0 && stat(BIRTHDAY_TMP, &st) == 0) rename(BIRTHDAY_TMP, BIRTHDAY_FILE);
    FILE *f = text ? fopen(BIRTHDAY_FILE, "r") : NULL;
    if (f) {
        size_t got = fread(text, 1, FILE_MAX + 1, f);
        /* A NUL byte (e.g. the file saved as UTF-16) would cut the text short: never rewrite that. */
        if (memchr(text, 0, got < FILE_MAX ? got : FILE_MAX)) *whole = false;
        if (got > FILE_MAX) {
            *whole = false;
            text[FILE_MAX] = 0;
            char *end = strrchr(text, '\n');
            if (end) end[1] = 0; else text[0] = 0;
        }
        fclose(f);
    }
    return text;
}
/* Text that can go into a JSON string as-is: no quotes, backslashes or control characters
   (a tab typed into birthdays.txt would otherwise break the page). */
static bool json_safe(const char *s)
{
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) return false;
    return true;
}
static char *out_buffer(void) { return heap_caps_calloc(1, OUT_MAX, MALLOC_CAP_SPIRAM); }
/* Append to a rewrite buffer of OUT_MAX bytes. Once something doesn't fit, *over is set and
   nothing more is added: the caller must then not save (a cut-off file would lose people). */
static void __attribute__((format(printf, 4, 5))) out_add(char *out, size_t *n, bool *over, const char *fmt, ...)
{
    if (*over) return;
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(out + *n, OUT_MAX - *n, fmt, a);
    va_end(a);
    if (k < 0 || (size_t)k >= OUT_MAX - *n) { *over = true; out[*n] = 0; return; }
    *n += (size_t)k;
}
/* Replace the file atomically (write a temp file, then rename). Card must be held. */
static bool write_file(const char *text)
{
    const char *tmp = BIRTHDAY_TMP;
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) { remove(BIRTHDAY_FILE); ok = rename(tmp, BIRTHDAY_FILE) == 0; }
    if (!ok) remove(tmp);
    return ok;
}

static esp_err_t page(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return sf_portal_to_own_host(r, "/birthdays");
    sf_portal_page_start(r, "SkyFrame birthdays", SF_TAB_BIRTHDAYS);
    httpd_resp_send_chunk(r, page_html, HTTPD_RESP_USE_STRLEN);
    return sf_portal_page_end(r);
}

static esp_err_t list(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the SkyFrame page");
    if (!sf_sd_begin()) return unavailable(r);
    bool whole;
    char *text = read_file(&whole);
    sf_sd_end();
    const size_t cap = 12 * 1024;
    char *json = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!text || !json) { free(text); free(json); return httpd_resp_send_500(r); }
    char message[80] = "";
    size_t n = (size_t)snprintf(json, cap, "{\"people\":[");
    size_t first = n;
    bool have_owner = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char m[80];
        if (sf_birthday_parse_message(line, m, sizeof(m))) { if (json_safe(m)) snprintf(message, sizeof(message), "%s", m); continue; }
        sf_birthday_t b;
        if (!sf_birthday_parse_line(line, &b) || n + 200 >= cap || !json_safe(b.name) || !json_safe(b.photo)) continue;
        if (b.owner && have_owner) b.owner = false;       /* same rule as the device: first owner wins */
        have_owner |= b.owner;
        char date[16];
        format_date(&b, date, sizeof(date));
        n += (size_t)snprintf(json + n, cap - n, "%s{\"name\":\"%s\",\"date\":\"%s\",\"photo\":\"%s\",\"owner\":%s}",
                              n > first ? "," : "", b.name, date, b.photo, b.owner ? "true" : "false");
    }
    n += (size_t)snprintf(json + n, cap - n, "],\"message\":\"%s\"}", message);
    free(text);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, json, (ssize_t)n);
    free(json);
    return e;
}

static esp_err_t photo(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the SkyFrame page");
    char file[64], path[128];
    if (!query_value(r, "file", file, sizeof(file), false) || strchr(file, '/') || strchr(file, '\\') || file[0] == '.')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad name");
    snprintf(path, sizeof(path), BIRTHDAY_PHOTOS "/%s", file);
    if (!sf_sd_begin()) return unavailable(r);
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t e = sf_jpeg_read_file(path, &data, &len);
    sf_sd_end();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Birthday photo %s not sent: %s", file, esp_err_to_name(e));
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    }
    httpd_resp_set_type(r, "image/jpeg");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    e = httpd_resp_send(r, (const char *)data, (ssize_t)len);
    free(data);
    return e;
}

/* Add someone, or update an existing person (same name and date): photo and owner flag.
   Marking someone owner removes the flag from everyone else. */
static esp_err_t add(httpd_req_t *r)
{
    sf_birthday_t b;
    if (!sf_portal_same_origin(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the SkyFrame page");
    if (!request_person(r, &b)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Check the name and date");
    if (r->content_len > PHOTO_MAX) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Photo too large");
    /* Receive the photo before touching the card. */
    uint8_t *jpeg = NULL;
    size_t got = 0;
    if (r->content_len > 0) {
        jpeg = heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory");
        int timeouts = 0;
        /* A whole upload gets two minutes: a client trickling a byte now and then would otherwise
           hold the web server (and this buffer) forever. */
        const int64_t give_up = esp_timer_get_time() + 120 * 1000000LL;
        while (got < r->content_len) {
            int n = httpd_req_recv(r, (char *)jpeg + got, r->content_len - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
            if (n <= 0 || esp_timer_get_time() > give_up) { free(jpeg); return httpd_resp_send_err(r, HTTPD_408_REQ_TIMEOUT, "Upload interrupted"); }
            got += (size_t)n;
        }
    }
    if (!sf_sd_begin()) { free(jpeg); return unavailable(r); }
    bool ok = false, whole;
    char *text = read_file(&whole), *out = out_buffer();
    if (text && out && whole) {
        size_t n = 0;
        bool over = false;
        char *save = NULL;
        for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
            sf_birthday_t e;
            bool person = sf_birthday_parse_line(line, &e);
            if (person && same_person(&e, &b)) { snprintf(b.photo, sizeof(b.photo), "%s", e.photo); continue; }
            if (person && b.owner && e.owner) { e.owner = false; char l[200]; format_line(&e, l, sizeof(l)); out_add(out, &n, &over, "%s", l); continue; }
            out_add(out, &n, &over, "%s\n", line);   /* comments, message, others: as-is */
        }
        if (jpeg) {
            /* Reuse this person's photo file; otherwise a name from theirs that isn't taken. */
            if (!b.photo[0]) {
                char slug[24] = "";
                size_t k = 0;
                for (const char *p = b.name; *p && k + 1 < 20; p++)
                    slug[k++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '_';
                slug[k] = 0;
                struct stat st;
                snprintf(b.photo, sizeof(b.photo), "%s.jpg", slug[0] ? slug : "person");
                for (int i = 2; i < 100; i++) {
                    char probe[128];
                    snprintf(probe, sizeof(probe), BIRTHDAY_PHOTOS "/%s", b.photo);
                    if (stat(probe, &st) != 0) break;
                    snprintf(b.photo, sizeof(b.photo), "%s-%d.jpg", slug[0] ? slug : "person", i);
                }
            }
            mkdir(BIRTHDAY_PHOTOS, 0775);
            char path[128];
            snprintf(path, sizeof(path), BIRTHDAY_PHOTOS "/%s", b.photo);
            FILE *f = fopen(path, "wb");
            bool w = f && fwrite(jpeg, 1, got, f) == got;
            if (f && fclose(f) != 0) w = false;
            if (!w) { remove(path); b.photo[0] = 0; }
        }
        if (n + 128 < FILE_MAX) {
            char l[200];
            format_line(&b, l, sizeof(l));
            out_add(out, &n, &over, "%s", l);
            ok = !over && write_file(out);
        }
    }
    sf_sd_end();
    free(text); free(out); free(jpeg);
    if (!ok) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save");
    sf_sd_content_changed();          /* the birthday screen picks it up */
    ESP_LOGI(TAG, "Saved birthday for %s", b.name);
    return httpd_resp_sendstr(r, "OK");
}

static esp_err_t del(httpd_req_t *r)
{
    sf_birthday_t b;
    if (!sf_portal_same_origin(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the SkyFrame page");
    if (!request_person(r, &b)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad request");
    if (!sf_sd_begin()) return unavailable(r);
    bool ok = false, found = false, whole;
    char photo[sizeof(b.photo)] = "";
    char *text = read_file(&whole), *out = out_buffer();
    if (text && out && whole) {
        size_t n = 0;
        bool over = false;
        char *save = NULL;
        for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
            sf_birthday_t e;
            if (sf_birthday_parse_line(line, &e) && same_person(&e, &b)) {
                found = true;
                snprintf(photo, sizeof(photo), "%s", e.photo);
                continue;
            }
            out_add(out, &n, &over, "%s\n", line);
        }
        ok = found && !over && write_file(out);
        /* The photo goes only once the person is really gone from the file. */
        if (ok && photo[0]) { char path[128]; snprintf(path, sizeof(path), BIRTHDAY_PHOTOS "/%s", photo); remove(path); }
    }
    sf_sd_end();
    free(text); free(out);
    if (!ok) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    sf_sd_content_changed();
    return httpd_resp_sendstr(r, "OK");
}

/* The owner's birthday message: the file's "message: ..." line (empty text removes it). */
static esp_err_t message(httpd_req_t *r)
{
    char text[80];
    if (!sf_portal_same_origin(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the SkyFrame page");
    if (!query_value(r, "text", text, sizeof(text), true) || strlen(text) > 60 || strpbrk(text, "\"\\"))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Keep the message under 60 characters");
    if (!sf_sd_begin()) return unavailable(r);
    bool ok = false, whole;
    char *old = read_file(&whole), *out = out_buffer();
    if (old && out && whole) {
        size_t n = 0;
        bool over = false;
        if (text[0]) n += (size_t)snprintf(out, OUT_MAX, "message: %s\n", text);
        char m[80], *save = NULL;
        for (char *line = strtok_r(old, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
            if (!sf_birthday_parse_message(line, m, sizeof(m))) out_add(out, &n, &over, "%s\n", line);
        ok = !over && n < FILE_MAX && write_file(out);
    }
    sf_sd_end();
    free(old); free(out);
    if (!ok) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save");
    sf_sd_content_changed();
    return httpd_resp_sendstr(r, "OK");
}

void sf_birthday_web_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/birthdays", .method = HTTP_GET, .handler = page},
        {.uri = "/api/birthdays", .method = HTTP_GET, .handler = list},
        {.uri = "/birthday_photo", .method = HTTP_GET, .handler = photo},
        {.uri = "/birthday_add", .method = HTTP_POST, .handler = add},
        {.uri = "/birthday_delete", .method = HTTP_POST, .handler = del},
        {.uri = "/birthday_message", .method = HTTP_POST, .handler = message},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
}
