// The WiFi setup page, on the print server's web ports (80 and 631):
//   GET  /wifi           the page: status, networks in range, a form
//   GET  /wifi/status    JSON: network, connected, IP, signal, setup AP
//   GET  /wifi/scan      JSON: networks in range (takes ~2 s)
//   POST /wifi           ssid=...&password=...[&setup_password=...]: save and join
//   POST /wifi/forget    back to the .env network (or setup mode)
// While the setup access point is open, every other page asked through it
// redirects here, so phones show it as the network's sign-in page.
//
// Who may change the network: anyone on the setup access point (it only
// opens while no network works; SETUP_PASSWORD in .env protects it); from
// the home network only in MODE=debug, or with setup_password = a non-empty
// SETUP_PASSWORD. Otherwise anyone on the network could move the printer
// to another network.
#include "wifi_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "local_config.h"
#include "wifi.h"

static const char *TAG = "wifi_setup";

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WiFi setup</title><style>"
    "body{font:16px system-ui,sans-serif;margin:0;padding:16px;max-width:520px;color:#222;background:#fafafa}"
    "@media(prefers-color-scheme:dark){body{color:#eee;background:#161616}.card{background:#222!important}"
    "input{background:#111;color:#eee;border-color:#444!important}}"
    "h1{font-size:22px}.card{background:#fff;border-radius:10px;padding:14px;margin:12px 0;box-shadow:0 1px 3px #0002}"
    "button{font:inherit;padding:10px 14px;border:0;border-radius:8px;background:#2563eb;color:#fff;margin:4px 4px 0 0}"
    "button.alt{background:#6b7280}input{font:inherit;width:100%;box-sizing:border-box;padding:10px;margin:6px 0;"
    "border:1px solid #ccc;border-radius:8px}ul{list-style:none;padding:0;margin:0}li{padding:10px 4px;"
    "border-bottom:1px solid #8883;cursor:pointer;display:flex;justify-content:space-between}#msg{min-height:1.4em}"
    "</style></head><body><h1>WiFi setup</h1>"
    "<div class=card id=st>…</div>"
    "<div class=card><b>Networks</b> <button class=alt onclick=scan()>Scan</button><ul id=nets></ul></div>"
    "<div class=card><form onsubmit='return save(event)'>"
    "<label>Network name<input id=ssid maxlength=32 required></label>"
    "<label>Password (empty for an open network)<input id=pw type=password maxlength=64></label>"
    "<label id=spl hidden>Setup password<input id=sp type=password></label>"
    "<button>Save and connect</button><button type=button class=alt onclick=forget()>Forget saved network</button>"
    "</form><p id=msg></p></div>"
    "<p><a href=/>Printer page</a></p>"
    "<script>"
    "const $=i=>document.getElementById(i);const esc=s=>s.replace(/[&<>\"]/g,c=>'&#'+c.charCodeAt(0)+';');"
    "async function status(){try{const s=await(await fetch('/wifi/status')).json();"
    "$('st').innerHTML=(s.connected?'Connected to <b>'+esc(s.ssid)+'</b><br>Address: <b>'+s.ip+'</b> ('+s.rssi+' dBm)'"
    ":s.ssid?'Connecting to <b>'+esc(s.ssid)+'</b>…':'No network configured')"
    "+(s.source?'<br><small>from '+s.source+'</small>':'')+(s.ap?'<br><small>Setup network <b>'+esc(s.ap_ssid)"
    "+'</b> is open</small>':'');$('spl').hidden=s.may_change;}catch(e){}}"
    "async function scan(){$('nets').innerHTML='<li>Scanning…</li>';try{const l=await(await fetch('/wifi/scan')).json();"
    "$('nets').innerHTML=l.map(n=>'<li data-s=\"'+esc(n.ssid)+'\"><span>'+esc(n.ssid)+(n.secure?' 🔒':'')+'</span><span>'"
    "+n.rssi+' dBm</span></li>').join('')||'<li>No network found</li>';"
    "document.querySelectorAll('#nets li[data-s]').forEach(li=>li.onclick=()=>{$('ssid').value=li.dataset.s;$('pw').focus()});"
    "}catch(e){$('nets').innerHTML='<li>Scan failed</li>'}}"
    "async function post(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':"
    "'application/x-www-form-urlencoded'},body:new URLSearchParams(b)});return r.json()}"
    "async function save(e){e.preventDefault();const r=await post('/wifi',{ssid:$('ssid').value,password:$('pw').value,"
    "setup_password:$('sp').value});$('msg').textContent=r.message;return false}"
    "async function forget(){const r=await post('/wifi/forget',{setup_password:$('sp').value});$('msg').textContent=r.message}"
    "status();setInterval(status,3000);scan();"
    "</script></body></html>";

static bool via_setup_ap(airprint_http_t *h)
{
    uint8_t ip[4];
    return airprint_http_local_ip(h, ip) && ip[0] == 192 && ip[1] == 168 && ip[2] == 4 && ip[3] == 1;
}

static bool may_change(airprint_http_t *h, const char *setup_password)
{
    if (via_setup_ap(h) || strcmp(MODE, "debug") == 0)
        return true;
    return SETUP_PASSWORD[0] && setup_password && strcmp(setup_password, SETUP_PASSWORD) == 0;
}

static void json_str(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            o += (size_t)snprintf(out + o, n - o, "\\%c", ch);
        else if (ch < 0x20)
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", ch);
        else
            out[o++] = (char)ch;
    }
    out[o] = 0;
}

// value of key in an x-www-form-urlencoded body, decoded
static void form_value(const char *body, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=')
            continue;
        p += kl + 1;
        size_t o = 0;
        while (*p && *p != '&' && o + 1 < n) {
            if (*p == '+') {
                out[o++] = ' ';
                p++;
            } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hx[3] = {p[1], p[2], 0};
                out[o++] = (char)strtol(hx, NULL, 16);
                p += 3;
            } else {
                out[o++] = *p++;
            }
        }
        out[o] = 0;
        return;
    }
}

static void reply(airprint_http_t *h, const char *status, bool ok, const char *msg)
{
    char m[200], body[260];
    json_str(m, sizeof(m), msg);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"message\":\"%s\"}", ok ? "true" : "false", m);
    airprint_http_send(h, status, "application/json", body, strlen(body));
}

static bool handle_post(airprint_http_t *h, const char *path)
{
    char body[512];
    int n = airprint_http_read_body(h, body, sizeof(body) - 1);
    body[n > 0 ? n : 0] = 0;
    char ssid[64], pw[96], sp[96];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "password", pw, sizeof(pw));
    form_value(body, "setup_password", sp, sizeof(sp));
    if (!may_change(h, sp)) {
        reply(h, "403 Forbidden", false,
              SETUP_PASSWORD[0] ? "Wrong setup password"
                                : "Change the network from the board's setup WiFi (it opens when no network works)");
        return true;
    }
    if (!strcmp(path, "/wifi/forget")) {
        wifi_forget_network();
        reply(h, "200 OK", true, "Saved network forgotten");
        return true;
    }
    size_t pl = strlen(pw);
    if (!ssid[0] || strlen(ssid) > 32 || pl > 64 || (pl && pl < 8)) {
        reply(h, "400 Bad Request", false, "Network name 1-32 characters; password empty or 8-64 characters");
        return true;
    }
    if (wifi_set_network(ssid, pw) != ESP_OK) {
        reply(h, "500 Internal Server Error", false, "Could not save the network");
        return true;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "Saved. Connecting to %s… The printer will then be at http://%s.local/", ssid, HOSTNAME);
    reply(h, "200 OK", true, msg);
    return true;
}

bool wifi_setup_http(airprint_http_t *h, const char *method, const char *path)
{
    bool post = !strcmp(method, "POST");
    if (post && (!strcmp(path, "/wifi") || !strcmp(path, "/wifi/forget")))
        return handle_post(h, path);
    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
        if (!strcmp(path, "/wifi") || !strncmp(path, "/wifi?", 6)) {
            airprint_http_send_ex(h, "200 OK", "text/html; charset=utf-8", "Cache-Control: no-store\r\n", PAGE,
                                  sizeof(PAGE) - 1);
            return true;
        }
        if (!strcmp(path, "/wifi/status")) {
            wifi_status_t st;
            wifi_get_status(&st);
            char ssid[80], ap[80], out[320];
            json_str(ssid, sizeof(ssid), st.ssid);
            json_str(ap, sizeof(ap), st.ap_ssid);
            snprintf(out, sizeof(out),
                     "{\"ssid\":\"%s\",\"source\":\"%s\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d,\"ap\":%s,"
                     "\"ap_ssid\":\"%s\",\"may_change\":%s}",
                     ssid, st.source == WIFI_CRED_SAVED ? "setup page" : st.source == WIFI_CRED_BUILD ? ".env" : "",
                     st.connected ? "true" : "false", st.ip, st.rssi, st.ap_on ? "true" : "false", ap,
                     may_change(h, NULL) ? "true" : "false");
            airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", out, strlen(out));
            return true;
        }
        if (!strcmp(path, "/wifi/scan")) {
            wifi_network_t nets[20];
            int k = wifi_scan(nets, 20);
            char *out = malloc(20 * 120 + 8);
            if (!out)
                return false;
            size_t o = 0;
            out[o++] = '[';
            for (int i = 0; i < k; i++) {
                char s[80];
                json_str(s, sizeof(s), nets[i].ssid);
                o += (size_t)snprintf(out + o, 120, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", i ? "," : "", s,
                                      nets[i].rssi, nets[i].secure ? "true" : "false");
            }
            out[o++] = ']';
            airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", out, o);
            free(out);
            return true;
        }
        // captive portal: through the setup network everything leads here
        if (wifi_setup_ap_on() && via_setup_ap(h) && strncmp(path, "/ipp", 4) != 0 && strcmp(path, "/icon.png")) {
            airprint_http_send_ex(h, "302 Found", "text/plain", "Location: http://192.168.4.1/wifi\r\n", "", 0);
            ESP_LOGD(TAG, "captive redirect of %s", path);
            return true;
        }
    }
    return false;
}
