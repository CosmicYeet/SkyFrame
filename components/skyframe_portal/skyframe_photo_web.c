/* Photo manager web page: family members on the home Wi-Fi can add and remove
   slideshow photos from a phone or computer browser (http://skyframe.local/photos).
   The browser does the heavy lifting: it applies the photo's rotation, center-crops
   to 480x480 and re-encodes as a small baseline JPEG before uploading, so the
   device only has to store the file. */
#include "skyframe_portal.h"
#include "skyframe_wifi.h"
#include "skyframe_sd.h"
#include "skyframe_jpeg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PHOTO_DIR SF_SD_ROOT "/photos"
#define MAX_UPLOAD (4 * 1024 * 1024)
#define MAX_LIST 256
static const char *TAG = "photo_web";

static const char page_html[] =
"<style>#g{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:14px}#g div{position:relative}"
"#g img{display:block;width:100%;aspect-ratio:1;object-fit:cover;border-radius:50%;background:var(--soft);"
"border:3px solid var(--line)}"
"#g button{position:absolute;top:0;right:0;width:38px;height:38px;min-height:0;padding:0;border-radius:50%;"
"background:var(--card);color:var(--bad);border:1px solid var(--line);box-shadow:0 1px 4px #0003}"
".drop{position:relative;display:flex;flex-direction:column;align-items:center;gap:2px;padding:20px 12px;"
"border:2px dashed var(--acc);border-radius:14px;background:var(--soft);color:var(--acc);text-align:center;"
"cursor:pointer;font-size:19px}.drop b{font-size:34px;line-height:1}"
".drop input{position:absolute;width:1px;height:1px;opacity:0}"
"progress{width:100%;height:10px;margin-top:12px;accent-color:var(--acc)}</style>"
"<section class=card><h2>Slideshow photos</h2>"
"<p class=sub>Shown on SkyFrame's round screen, cropped to a circle from the middle of each picture.</p>"
"<label class=drop><b>+</b>Add photos<input type=file id=f accept='image/*' multiple>"
"<span class=hint>Choose one or many from your phone or computer</span></label>"
"<progress id=pg hidden></progress><p id=s class=note>Loading&hellip;</p><div id=g></div></section><script>"
"const S=document.getElementById('s'),G=document.getElementById('g'),P=document.getElementById('pg');"
"function say(t,ok){S.textContent=t;S.className='note'+(ok===undefined?'':ok?' ok':' err')}"
"async function list(){const r=await fetch('/api/photos');if(!r.ok){say(await r.text(),false);return}"
"const n=await r.json();G.innerHTML='';"
"for(const x of n){const d=document.createElement('div'),i=new Image(),b=document.createElement('button');"
"i.loading='lazy';i.alt='';i.src='/photo?name='+encodeURIComponent(x);b.textContent='\\u2715';b.title='Remove';"
"b.setAttribute('aria-label','Remove '+x);"
"b.onclick=async()=>{if(confirm('Remove '+x+' from the slideshow?')){"
"const r=await fetch('/delete?name='+encodeURIComponent(x),{method:'POST'});if(r.ok)list();else say(await r.text(),false)}};"
"d.append(i,b);G.append(d)}"
"say(n.length?n.length+' photo'+(n.length==1?'':'s')+' in the slideshow':'No photos yet. Tap \\u201cAdd photos\\u201d to choose some.')}"
"function name(f){return(f.name.replace(/\\.[^.]*$/,'').replace(/[^A-Za-z0-9_-]+/g,'_').slice(0,40)||'photo')+'.jpg'}"
"async function shrink(f){const b=await createImageBitmap(f,{imageOrientation:'from-image'}),s=Math.min(b.width,b.height),"
"c=document.createElement('canvas');c.width=c.height=480;"
"c.getContext('2d').drawImage(b,(b.width-s)/2,(b.height-s)/2,s,s,0,0,480,480);"
"return new Promise(r=>c.toBlob(r,'image/jpeg',0.88))}"
"document.getElementById('f').onchange=async e=>{const fs=[...e.target.files],bad=[];let k=0;"
"P.max=fs.length;P.value=0;P.hidden=!fs.length;"
"for(const f of fs){say('Adding '+(++k)+' of '+fs.length+'\\u2026');"
"try{const j=await shrink(f),r=await fetch('/upload?name='+encodeURIComponent(name(f)),{method:'POST',body:j});"
"if(!r.ok)throw new Error(await r.text())}catch(x){bad.push(f.name+' ('+x.message+')')}P.value=k}"
"e.target.value='';P.hidden=true;await list();"
"if(bad.length)say('Could not add: '+bad.join(', '),false);"
"else if(fs.length)say('Added '+fs.length+' photo'+(fs.length==1?'':'s')+'. '+S.textContent,true)};list();</script>";

static esp_err_t unavailable(httpd_req_t *r)
{
    httpd_resp_set_status(r, "503 Service Unavailable");
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, sf_sd_usb_active()
        ? "The microSD card is connected to a computer. Eject it there, then try again."
        : "No microSD card in SkyFrame.");
}

/* Accept printable ASCII names without path separators; decode %XX from the query. */
static bool query_name(httpd_req_t *r, char *out, size_t cap)
{
    char query[160], raw[128];
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", raw, sizeof(raw)) != ESP_OK) return false;
    size_t j = 0;
    for (size_t i = 0; raw[i]; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '%' && isxdigit((unsigned char)raw[i+1]) && isxdigit((unsigned char)raw[i+2])) {
            char hex[3] = {raw[i+1], raw[i+2], 0};
            c = (unsigned char)strtol(hex, NULL, 16);
            i += 2;
        }
        if (c < 0x20 || c > 0x7e || c == '/' || c == '\\' || j + 1 >= cap) return false;
        out[j++] = (char)c;
    }
    out[j] = 0;
    return j > 0 && out[0] != '.';
}
static bool is_jpeg(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static esp_err_t forbidden(httpd_req_t *r);
static esp_err_t photos_page(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return sf_portal_to_own_host(r, "/photos");
    sf_portal_page_start(r, "SkyFrame photos", SF_TAB_PHOTOS);
    httpd_resp_send_chunk(r, page_html, HTTPD_RESP_USE_STRLEN);
    return sf_portal_page_end(r);
}

static esp_err_t photo_list(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return forbidden(r);
    if (!sf_sd_begin()) return unavailable(r);
    const size_t cap = 24 * 1024;
    char *json = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);   /* internal RAM is scarce */
    if (!json) { sf_sd_end(); return httpd_resp_send_500(r); }
    size_t n = 0;
    json[n++] = '[';
    DIR *d = opendir(PHOTO_DIR);
    int count = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && count < MAX_LIST) {
            const char *s = e->d_name;
            if (s[0] == '.' || !is_jpeg(s)) continue;
            bool plain = true;   /* names the page can show and send back */
            for (const char *p = s; *p; p++)
                if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '"' || *p == '\\') plain = false;
            /* Only names the page can send back (printable, under the 95-char query limit). */
            if (!plain || strlen(s) > 90 || n + strlen(s) + 4 >= cap) continue;
            n += (size_t)snprintf(json + n, cap - n, "%s\"%s\"", count ? "," : "", s);
            count++;
        }
        closedir(d);
    }
    sf_sd_end();
    json[n++] = ']';
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, json, (ssize_t)n);
    free(json);
    return e;
}

/* Changes (upload/delete/save) must come from SkyFrame's own pages: a browser sends an
   Origin header with cross-site form posts, so another site open on the home network
   can't quietly change things. Requests without Origin (non-browser tools) are allowed. */
/* Only SkyFrame's own names: a site that points its own name at the device ("DNS
   rebinding") would otherwise pass the Origin check with Host and Origin both its own. */
static bool own_host(const char *host)
{
    char name[64], ip[16];
    snprintf(name, sizeof(name), "%s", host);
    char *port = strchr(name, ':');
    if (port) *port = 0;
    return !strcasecmp(name, "skyframe.local") || !strcmp(name, "192.168.4.1") ||
           (sf_wifi_ip(ip, sizeof(ip)) && !strcmp(name, ip));
}
bool sf_portal_own_host(httpd_req_t *r)
{
    char host[64];
    return httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) == ESP_OK && own_host(host);
}
esp_err_t sf_portal_to_own_host(httpd_req_t *r, const char *path)
{
    char ip[16] = "192.168.4.1", to[64];
    struct sockaddr_storage a;
    socklen_t len = sizeof(a);
    if (getsockname(httpd_req_to_sockfd(r), (struct sockaddr *)&a, &len) == 0) {
        if (a.ss_family == AF_INET) inet_ntoa_r(((struct sockaddr_in *)&a)->sin_addr, ip, sizeof(ip));
#if CONFIG_LWIP_IPV6
        else if (a.ss_family == AF_INET6) {   /* an IPv4 client on a dual-stack socket: ::ffff:a.b.c.d */
            const uint8_t *b = ((struct sockaddr_in6 *)&a)->sin6_addr.s6_addr;
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        }
#endif
    }
    snprintf(to, sizeof(to), "http://%s%s", ip, path);
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", to);
    return httpd_resp_send(r, NULL, 0);
}
bool sf_portal_same_origin(httpd_req_t *r)
{
    char origin[96], host[64];
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK || !own_host(host)) return false;
    esp_err_t e = httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin));
    if (e == ESP_ERR_NOT_FOUND) return true;   /* not a browser form post */
    if (e != ESP_OK) return false;             /* e.g. too long to check: refuse */
    const char *o = strncmp(origin, "http://", 7) ? NULL : origin + 7;
    return o && !strcasecmp(o, host);
}
static esp_err_t forbidden(httpd_req_t *r)
{
    return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the SkyFrame page to make changes");
}

/* The card is held only to read the file; the (possibly slow) send happens afterwards,
   so a sluggish phone can never delay handing the card to a computer. */
static esp_err_t photo_get(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return forbidden(r);
    char name[96], path[160];
    if (!query_name(r, name, sizeof(name)) || !is_jpeg(name)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad name");
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
    if (!sf_sd_begin()) return unavailable(r);
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t e = sf_jpeg_read_file(path, &data, &len);
    sf_sd_end();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Photo %s not sent: %s", name, esp_err_to_name(e));
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    }
    httpd_resp_set_type(r, "image/jpeg");
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=300");
    e = httpd_resp_send(r, (const char *)data, (ssize_t)len);
    free(data);
    return e;
}

/* The whole photo is received into memory first; the card is held only for the write. */
static esp_err_t photo_upload(httpd_req_t *r)
{
    char name[96], path[160], tmp[160];
    if (!sf_portal_same_origin(r)) return forbidden(r);
    if (!query_name(r, name, sizeof(name)) || !is_jpeg(name)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad name");
    if (r->content_len <= 0 || r->content_len > MAX_UPLOAD)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Photo too large (4 MB max)");
    uint8_t *body = heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory");
    size_t got = 0;
    int timeouts = 0;
    /* A whole upload gets two minutes: a client trickling a byte now and then would otherwise
       hold the web server (and this buffer) forever. */
    const int64_t give_up = esp_timer_get_time() + 120 * 1000000LL;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, (char *)body + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;   /* give up after ~15 s */
        if (n <= 0 || esp_timer_get_time() > give_up) { free(body); return httpd_resp_send_err(r, HTTPD_408_REQ_TIMEOUT, "Upload interrupted"); }
        got += (size_t)n;
    }
    if (!sf_sd_begin()) { free(body); return unavailable(r); }
    esp_err_t result = ESP_FAIL;
    if (mkdir(PHOTO_DIR, 0775) == 0 || errno == EEXIST) {
        /* Don't overwrite: photo.jpg, photo-2.jpg, photo-3.jpg ... */
        struct stat st;
        snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
        char *dot = strrchr(name, '.');
        for (int k = 2; stat(path, &st) == 0 && k < 1000; k++)
            snprintf(path, sizeof(path), PHOTO_DIR "/%.*s-%d%s", (int)(dot - name), name, k, dot);
        snprintf(tmp, sizeof(tmp), PHOTO_DIR "/upload.tmp");
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool ok = fwrite(body, 1, got, f) == got;
            ok = (fclose(f) == 0) && ok;
            if (ok && rename(tmp, path) == 0) result = ESP_OK;
            else remove(tmp);
        }
    }
    sf_sd_end();
    free(body);
    if (result != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save the photo");
    sf_sd_content_changed();   /* slideshow rescans */
    ESP_LOGI(TAG, "Added %s", path);
    return httpd_resp_sendstr(r, "OK");
}

static esp_err_t photo_delete(httpd_req_t *r)
{
    char name[96], path[160];
    if (!sf_portal_same_origin(r)) return forbidden(r);
    if (!query_name(r, name, sizeof(name)) || !is_jpeg(name)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad name");
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
    if (!sf_sd_begin()) return unavailable(r);
    int rc = remove(path);
    sf_sd_end();
    if (rc != 0) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    sf_sd_content_changed();
    ESP_LOGI(TAG, "Removed %s", path);
    return httpd_resp_sendstr(r, "OK");
}

void sf_photo_web_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/photos", .method = HTTP_GET, .handler = photos_page},
        {.uri = "/api/photos", .method = HTTP_GET, .handler = photo_list},
        {.uri = "/photo", .method = HTTP_GET, .handler = photo_get},
        {.uri = "/upload", .method = HTTP_POST, .handler = photo_upload},
        {.uri = "/delete", .method = HTTP_POST, .handler = photo_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
}
