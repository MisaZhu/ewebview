// Cookie store implementation: RFC 6265 parsing, matching and SameSite rules.
//
// Deliberately free of platform headers (no logging, no OS services): the
// logic is pure string/date work, so it builds on any hosted or cross libc
// with pthread + clock_gettime.

#include "EWebCookies.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

namespace eweb {
namespace {

/* Both caps bound one shared resource on a device with a few MB of heap: a
 * page that keeps writing cookies evicts its own oldest entries instead of
 * growing the jar forever. 64 entries x 2 KB is the worst case (~128 KB);
 * real sites stay far below it. */
const size_t kMaxCookies     = 64;
const size_t kMaxCookieBytes = 2048;
/* Browsers cap a cookie's lifetime at 400 days; it also keeps max_age*1000
 * away from int64 overflow. */
const int64_t kMaxAgeSeconds = 400LL * 24 * 3600;

int64_t now_ms()
{
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
    return (int64_t)time(NULL) * 1000;
}

std::string to_lower(const std::string& in)
{
    std::string out = in;
    for(size_t i = 0; i < out.size(); i++) {
        char ch = out[i];
        if(ch >= 'A' && ch <= 'Z')
            out[i] = (char)(ch - 'A' + 'a');
    }
    return out;
}

std::string trim(const std::string& in)
{
    size_t b = 0;
    size_t e = in.size();
    while(b < e) {
        char ch = in[b];
        if(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { ++b; continue; }
        break;
    }
    while(e > b) {
        char ch = in[e - 1];
        if(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { --e; continue; }
        break;
    }
    return in.substr(b, e - b);
}

/* Case-insensitive equality with a literal. strcasecmp() would do, but keeping
 * it local means this file builds on the host without any libc assumption. */
bool ci_equal(const std::string& a, const char* b)
{
    size_t n = strlen(b);
    if(a.size() != n)
        return false;
    for(size_t i = 0; i < n; i++) {
        char x = a[i];
        char y = b[i];
        if(x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if(y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if(x != y)
            return false;
    }
    return true;
}

bool all_digits(const std::string& s)
{
    if(s.empty())
        return false;
    for(size_t i = 0; i < s.size(); i++) {
        if(s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/* Howard Hinnant's days_from_civil: days since 1970-01-01 for a proleptic
 * Gregorian date. Only ever called with a sane parsed date. */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                            /* [0, 399] */
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;       /* [0, 365] */
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

int month_from_name(const std::string& tok)
{
    static const char* names[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec" };
    if(tok.size() < 3)
        return -1;
    std::string head = to_lower(tok.substr(0, 3));
    for(int i = 0; i < 12; i++) {
        if(head == names[i])
            return i + 1;
    }
    return -1;
}

/* Parse the three date shapes RFC 6265 allows for Expires=
 *     Sun, 06 Nov 1994 08:49:37 GMT     (RFC 1123, what servers actually send)
 *     Sunday, 06-Nov-94 08:49:37 GMT    (RFC 850)
 *     Sun Nov  6 08:49:37 1994          (asctime)
 * by pulling the time out first (so its fields are not mistaken for a day or a
 * year) and then reading the remaining numeric tokens position-independently. */
bool parse_http_date(const std::string& in, int64_t& outMs)
{
    std::string s = in;

    int hour = -1, minute = 0, second = 0;
    for(size_t i = 2; i + 5 < s.size(); i++) {
        if(s[i] != ':')
            continue;
        if(s[i - 1] < '0' || s[i - 1] > '9' || s[i - 2] < '0' || s[i - 2] > '9')
            continue;
        if(s[i + 1] < '0' || s[i + 1] > '9' || s[i + 2] < '0' || s[i + 2] > '9')
            continue;
        if(s[i + 3] != ':')
            continue;
        if(s[i + 4] < '0' || s[i + 4] > '9' || s[i + 5] < '0' || s[i + 5] > '9')
            continue;
        hour   = (s[i - 2] - '0') * 10 + (s[i - 1] - '0');
        minute = (s[i + 1] - '0') * 10 + (s[i + 2] - '0');
        second = (s[i + 4] - '0') * 10 + (s[i + 5] - '0');
        s.erase(i - 2, 8);
        break;
    }
    if(hour < 0 || hour > 23 || minute > 59 || second > 59)
        return false;

    int month = -1;
    std::vector<std::string> numbers;
    size_t pos = 0;
    while(pos < s.size()) {
        char ch = s[pos];
        bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if(!alnum) { ++pos; continue; }
        size_t end = pos;
        while(end < s.size()) {
            char c2 = s[end];
            if((c2 >= '0' && c2 <= '9') || (c2 >= 'a' && c2 <= 'z') || (c2 >= 'A' && c2 <= 'Z'))
                { ++end; continue; }
            break;
        }
        std::string tok = s.substr(pos, end - pos);
        if(all_digits(tok)) {
            numbers.push_back(tok);
        } else if(month < 0) {
            month = month_from_name(tok);
        }
        pos = end;
    }
    if(month < 0 || numbers.empty())
        return false;

    int day = -1;
    int64_t year = -1;
    for(size_t i = 0; i < numbers.size(); i++) {
        int64_t v = (int64_t)strtoll(numbers[i].c_str(), NULL, 10);
        if(day < 0 && v >= 1 && v <= 31 && numbers[i].size() <= 2) {
            day = (int)v;
            continue;
        }
        if(year < 0)
            year = v;
    }
    if(day < 0 || year < 0)
        return false;
    if(numbers[0].size() == 4)
        year = (int64_t)strtoll(numbers[0].c_str(), NULL, 10);   /* 4-digit year wins */
    if(year < 100)
        year += (year >= 70) ? 1900 : 2000;                      /* RFC 6265 two-digit rule */
    if(year < 1601)
        return false;

    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    outMs = (days * 86400 + hour * 3600 + minute * 60 + second) * 1000;
    return true;
}

bool parse_int64(const std::string& s, int64_t& out)
{
    size_t i = 0;
    bool neg = false;
    if(i < s.size() && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        i++;
    }
    if(i >= s.size())
        return false;
    int64_t v = 0;
    for(; i < s.size(); i++) {
        if(s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
        if(v > 1000000000000000LL)     /* absurd: ignore the attribute */
            return false;
    }
    out = neg ? -v : v;
    return true;
}

bool is_ip_literal(const std::string& host)
{
    if(host.empty())
        return false;
    if(host[0] == '[')          /* IPv6 */
        return true;
    int dots = 0;
    for(size_t i = 0; i < host.size(); i++) {
        if(host[i] == '.') { dots++; continue; }
        if(host[i] < '0' || host[i] > '9')
            return false;
    }
    return dots == 3;
}

/* Second-level registration labels that turn a two-letter TLD into a public
 * suffix ("co.uk", "com.au", "ne.jp", ...). Only consulted for a two-letter
 * TLD, so "www.ne.com" keeps "ne.com" as its site. */
bool is_second_level_suffix(const std::string& label)
{
    static const char* suffixes[] = { "co", "com", "net", "org", "gov", "edu",
                                      "ac", "or", "ne", "go", "ad", "as" };
    for(size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if(ci_equal(label, suffixes[i]))
            return true;
    }
    return false;
}

/* RFC 6265 §5.1.3 domain-match, with `host` as the request host. */
bool domain_matches(const std::string& host, const EWebCookie& c)
{
    if(c.hostOnly)
        return host == c.domain;
    if(host == c.domain)
        return true;
    size_t n = c.domain.size();
    if(host.size() <= n + 1)
        return false;
    if(host.compare(host.size() - n, n, c.domain) != 0)
        return false;
    return host[host.size() - n - 1] == '.';
}

/* RFC 6265 §5.1.4 path-match. */
bool path_matches(const std::string& path, const EWebCookie& c)
{
    if(c.path == path)
        return true;
    if(path.size() <= c.path.size())
        return false;
    if(path.compare(0, c.path.size(), c.path) != 0)
        return false;
    if(c.path[c.path.size() - 1] == '/')
        return true;
    return path[c.path.size()] == '/';
}

bool samesite_allows(const EWebCookie& c, bool topLevel)
{
    switch(c.sameSite) {
    case COOKIE_SAMESITE_NONE:   return true;
    case COOKIE_SAMESITE_STRICT: return false;
    default:                     return topLevel;   /* Lax: navigation only */
    }
}

/* RFC 6265 §5.1.4 default-path: the request path minus its last segment. */
std::string default_path(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    if(slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

} /* anonymous namespace */

/* ------------------------------------------------------------------ */
/* singleton                                                          */
/* ------------------------------------------------------------------ */

static EWebCookieJar* s_jar  = NULL;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

void EWebCookieJar::create()
{
    s_jar = new EWebCookieJar();
}

EWebCookieJar& EWebCookieJar::instance()
{
    pthread_once(&s_once, create);
    return *s_jar;
}

EWebCookieJar::EWebCookieJar()
{
    pthread_mutex_init(&m_mutex, NULL);
}

EWebCookieJar::~EWebCookieJar()
{
    pthread_mutex_destroy(&m_mutex);
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                        */
/* ------------------------------------------------------------------ */

std::string EWebCookieJar::hostOf(const std::string& url)
{
    size_t scheme = url.find("://");
    if(scheme == std::string::npos)
        return std::string();
    size_t start = scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = (end == std::string::npos) ? url.substr(start)
                                                       : url.substr(start, end - start);
    size_t at = authority.find_last_of('@');
    if(at != std::string::npos)
        authority = authority.substr(at + 1);
    if(!authority.empty() && authority[0] == '[') {      /* [::1]:port */
        size_t close = authority.find(']');
        if(close != std::string::npos)
            return to_lower(authority.substr(0, close + 1));
    }
    size_t colon = authority.find_last_of(':');
    if(colon != std::string::npos &&
       all_digits(authority.substr(colon + 1))) {
        authority = authority.substr(0, colon);
    }
    return to_lower(authority);
}

std::string EWebCookieJar::pathOf(const std::string& url)
{
    size_t scheme = url.find("://");
    if(scheme == std::string::npos)
        return "/";
    size_t slash = url.find('/', scheme + 3);
    if(slash == std::string::npos)
        return "/";
    size_t end = url.find_first_of("?#", slash);
    std::string path = (end == std::string::npos) ? url.substr(slash)
                                                  : url.substr(slash, end - slash);
    return path.empty() ? std::string("/") : path;
}

std::string EWebCookieJar::siteOf(const std::string& host)
{
    if(host.empty() || is_ip_literal(host))
        return host;
    size_t p2 = host.find_last_of('.');
    if(p2 == std::string::npos || p2 == 0)
        return host;                       /* single label: "localhost" */
    size_t p1 = host.find_last_of('.', p2 - 1);
    if(p1 == std::string::npos)
        return host;                       /* two labels: "example.com" */

    std::string tld = host.substr(p2 + 1);
    std::string second = host.substr(p1 + 1, p2 - p1 - 1);
    if(tld.size() != 2 || !is_second_level_suffix(second))
        return host.substr(p1 + 1);        /* "www.example.com" -> "example.com" */

    /* "shop.example.co.uk": the public suffix is "co.uk", so the site keeps one
     * more label. A bare "co.uk" has nothing left to give and stays as-is. */
    if(p1 == 0)
        return host;
    size_t p0 = host.find_last_of('.', p1 - 1);
    if(p0 == std::string::npos)
        return host;
    return host.substr(p0 + 1);
}

void EWebCookieJar::splitUrl(const std::string& url, std::string& host,
                             std::string& path, bool& secure)
{
    secure = false;
    if(url.compare(0, 8, "https://") == 0) {
        secure = true;
        host = hostOf(url);
        path = pathOf(url);
        return;
    }
    if(url.compare(0, 7, "http://") == 0) {
        host = hostOf(url);
        path = pathOf(url);
        return;
    }
    /* Any other absolute URL: cookies are filed under the scheme name so that
     * every file:// document shares one bucket, and no cookie can ever cross
     * into an HTTP site. */
    size_t slash = url.find('/');
    size_t colon = url.find(':');
    if(colon == std::string::npos || (slash != std::string::npos && slash < colon)) {
        host = std::string();
        path = "/";
        return;
    }
    host = to_lower(url.substr(0, colon));
    /* file: is a "potentially trustworthy origin" (W3C Secure Contexts), so a Secure
     * cookie a local document sets is kept and stays readable there, exactly as
     * it does in a browser. Any other scheme (about:, data:) is not. */
    secure = (host == "file");
    path = "/";
}

/* ------------------------------------------------------------------ */
/* storing                                                            */
/* ------------------------------------------------------------------ */

bool EWebCookieJar::store(const std::string& url, const std::string& header, bool fromHttp)
{
    std::string host;
    std::string path;
    bool secure_ctx = false;
    splitUrl(url, host, path, secure_ctx);
    if(host.empty() || header.empty())
        return false;

    EWebCookie c;
    c.expiresMs = -1;                              /* session cookie */
    c.sameSite  = COOKIE_SAMESITE_LAX;
    c.hostOnly  = true;
    c.secure    = false;
    c.httpOnly  = false;
    c.domain    = host;
    c.path      = default_path(path);

    size_t semi = header.find(';');
    std::string pair = trim(header.substr(0, semi));
    size_t eq = pair.find('=');
    if(eq == std::string::npos)
        return false;
    c.name  = trim(pair.substr(0, eq));
    c.value = trim(pair.substr(eq + 1));
    if(c.name.empty())
        return false;
    if(c.name.find_first_of(" \t;,=") != std::string::npos)
        return false;
    if(c.name.size() + c.value.size() > kMaxCookieBytes)
        return false;
    if(!c.value.empty() && c.value[0] == '"' &&
       c.value[c.value.size() - 1] == '"' && c.value.size() >= 2) {
        c.value = c.value.substr(1, c.value.size() - 2);   /* quoted cookie-value */
    }

    bool have_max_age = false;
    bool have_expires = false;
    int64_t max_age = 0;
    int64_t expires = 0;

    size_t pos = (semi == std::string::npos) ? header.size() : semi + 1;
    while(pos < header.size()) {
        size_t next = header.find(';', pos);
        size_t stop = (next == std::string::npos) ? header.size() : next;
        std::string one = header.substr(pos, stop - pos);
        size_t aeq = one.find('=');
        std::string aname = to_lower(trim(aeq == std::string::npos ? one
                                                                   : one.substr(0, aeq)));
        std::string aval = (aeq == std::string::npos) ? std::string()
                                                      : trim(one.substr(aeq + 1));
        if(aname == "domain") {
            std::string d = to_lower(aval);
            if(!d.empty() && d[0] == '.')
                d = d.substr(1);
            if(!d.empty()) {
                /* RFC 6265 §5.3: reject the whole cookie unless the Domain
                 * domain-matches the request host. The site comparison is the
                 * public-suffix guard that stops "Domain=com" (and any other
                 * suffix this host sits under) from claiming a shared jar. */
                EWebCookie probe;
                probe.domain = d;
                probe.hostOnly = false;
                if(!domain_matches(host, probe) || siteOf(d) != siteOf(host))
                    return false;
                c.domain = d;
                c.hostOnly = false;
            }
        }
        else if(aname == "path") {
            if(!aval.empty() && aval[0] == '/')
                c.path = aval;
        }
        else if(aname == "max-age") {
            int64_t v;
            if(parse_int64(aval, v)) {
                have_max_age = true;
                max_age = v;
            }
        }
        else if(aname == "expires") {
            int64_t ms;
            if(parse_http_date(aval, ms)) {
                have_expires = true;
                expires = ms;
            }
        }
        else if(aname == "secure") {
            c.secure = true;
        }
        else if(aname == "httponly") {
            /* Only a server may create an HttpOnly cookie; a script writing
             * document.cookie must not be able to hide one from itself or
             * un-hide one the server set. */
            c.httpOnly = fromHttp;
        }
        else if(aname == "samesite") {
            std::string v = to_lower(aval);
            if(v == "strict")    c.sameSite = COOKIE_SAMESITE_STRICT;
            else if(v == "none") c.sameSite = COOKIE_SAMESITE_NONE;
            else                 c.sameSite = COOKIE_SAMESITE_LAX;
        }
        if(next == std::string::npos)
            break;
        pos = next + 1;
    }

    /* Two attributes are only meaningful over TLS, so both are refused when the
     * context is not secure: a plain-HTTP response must not be able to plant a
     * Secure cookie that only the HTTPS site can then read (RFC 6265bis 5.3),
     * and SameSite=None without Secure would be a cross-site leak. */
    if(c.secure && !secure_ctx)
        return false;
    if(c.sameSite == COOKIE_SAMESITE_NONE && !c.secure)
        return false;

    int64_t now = now_ms();
    if(have_max_age) {                              /* Max-Age wins over Expires */
        if(max_age <= 0)
            c.expiresMs = 0;                        /* already expired: delete */
        else
            c.expiresMs = now + ((max_age > kMaxAgeSeconds) ? kMaxAgeSeconds
                                                            : max_age) * 1000;
    } else if(have_expires) {
        c.expiresMs = expires;
    }

    pthread_mutex_lock(&m_mutex);
    /* Opportunistic purge: the jar is scanned on every store anyway. */
    for(size_t i = 0; i < m_cookies.size(); ) {
        if(m_cookies[i].expiresMs >= 0 && m_cookies[i].expiresMs <= now)
            m_cookies.erase(m_cookies.begin() + i);
        else
            ++i;
    }

    int found = -1;
    for(size_t i = 0; i < m_cookies.size(); i++) {
        if(m_cookies[i].name == c.name && m_cookies[i].domain == c.domain &&
           m_cookies[i].path == c.path) {
            found = (int)i;
            break;
        }
    }
    /* A script may not touch a cookie the server marked HttpOnly. */
    if(found >= 0 && m_cookies[found].httpOnly && !fromHttp) {
        pthread_mutex_unlock(&m_mutex);
        return false;
    }
    if(c.expiresMs >= 0 && c.expiresMs <= now) {
        if(found >= 0)
            m_cookies.erase(m_cookies.begin() + found);
        pthread_mutex_unlock(&m_mutex);
        return true;                                /* an expiry in the past deletes */
    }
    if(found >= 0) {
        m_cookies[found] = c;                       /* keeps the creation order */
    } else {
        if(m_cookies.size() >= kMaxCookies)
            m_cookies.erase(m_cookies.begin());     /* oldest entry goes */
        m_cookies.push_back(c);
    }
    pthread_mutex_unlock(&m_mutex);
    return true;
}

void EWebCookieJar::storeResponseCookies(const std::string& url,
                                         const std::vector<std::string>& setCookieHeaders)
{
    for(size_t i = 0; i < setCookieHeaders.size(); i++)
        store(url, setCookieHeaders[i], true);
}

void EWebCookieJar::jsSet(const std::string& url, const std::string& setCookie)
{
    store(url, setCookie, false);
}

/* ------------------------------------------------------------------ */
/* matching                                                           */
/* ------------------------------------------------------------------ */

std::string EWebCookieJar::buildHeader(const std::string& url, const std::string& pageUrl,
                                       bool topLevel, bool skipHttpOnly) const
{
    std::string host;
    std::string path;
    bool secure_ctx = false;
    splitUrl(url, host, path, secure_ctx);
    if(host.empty())
        return std::string();

    /* Same-site test: the initiator document's site against the request's. No
     * initiator (a navigation typed into the address bar, the first load) is
     * same-site by definition. */
    bool cross_site = false;
    if(!pageUrl.empty()) {
        std::string phost;
        std::string ppath;
        bool psecure = false;
        splitUrl(pageUrl, phost, ppath, psecure);
        if(!phost.empty())
            cross_site = (siteOf(phost) != siteOf(host));
    }

    int64_t now = now_ms();
    std::vector<const EWebCookie*> hits;
    std::string out;

    pthread_mutex_lock(&m_mutex);
    for(size_t i = 0; i < m_cookies.size(); i++) {
        const EWebCookie& c = m_cookies[i];
        if(c.expiresMs >= 0 && c.expiresMs <= now)   continue;
        if(skipHttpOnly && c.httpOnly)               continue;
        if(c.secure && !secure_ctx)                  continue;
        if(!domain_matches(host, c))                 continue;
        if(!path_matches(path, c))                   continue;
        if(cross_site && !samesite_allows(c, topLevel)) continue;
        hits.push_back(&c);
    }
    /* RFC 6265 §5.4: longer paths first, insertion order kept within one path
     * length. A plain insertion sort does both and needs no <algorithm>. */
    for(size_t i = 1; i < hits.size(); i++) {
        const EWebCookie* key = hits[i];
        size_t j = i;
        while(j > 0 && hits[j - 1]->path.size() < key->path.size()) {
            hits[j] = hits[j - 1];
            j--;
        }
        hits[j] = key;
    }
    /* Built under the lock: `hits` points into m_cookies, which a concurrent
     * store() on the worker thread could reallocate. */
    for(size_t i = 0; i < hits.size(); i++) {
        if(!out.empty())
            out += "; ";
        out += hits[i]->name;
        out += "=";
        out += hits[i]->value;
    }
    pthread_mutex_unlock(&m_mutex);
    return out;
}

std::string EWebCookieJar::requestHeader(const std::string& url, const std::string& pageUrl,
                                         bool topLevel) const
{
    return buildHeader(url, pageUrl, topLevel, false);
}

std::string EWebCookieJar::jsGet(const std::string& url) const
{
    /* The document is its own initiator, so SameSite never applies here. */
    return buildHeader(url, url, true, true);
}

void EWebCookieJar::clear()
{
    pthread_mutex_lock(&m_mutex);
    m_cookies.clear();
    pthread_mutex_unlock(&m_mutex);
}

size_t EWebCookieJar::count() const
{
    pthread_mutex_lock(&m_mutex);
    size_t n = m_cookies.size();
    pthread_mutex_unlock(&m_mutex);
    return n;
}

}
