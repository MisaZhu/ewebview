// Browser-wide HTTP cookie store with per-site ("same-site") isolation.
//
// One jar per process, shared by every ewebview instance and by both sides of
// the cookie API: the download worker thread (Cookie:/Set-Cookie headers) and
// the JS thread (document.cookie). Nothing is persisted to disk, so the jar
// dies with the process; session cookies additionally die with... the jar.
//
// Ported verbatim from widget++'s CookieJar: deliberately free of any
// platform headers - the logic is pure string/date work over libc + pthread.

#pragma once

#include <string>
#include <vector>
#include <stdint.h>
#include <pthread.h>

namespace eweb {

/* Cross-site policy of a cookie (RFC 6265bis SameSite). A cookie that does not
 * name the attribute gets Lax, which is what current browsers default to. */
enum EWebCookieSameSite {
    COOKIE_SAMESITE_LAX    = 0,
    COOKIE_SAMESITE_STRICT = 1,
    COOKIE_SAMESITE_NONE   = 2,
};

/* One stored cookie. `domain` never carries a leading dot: `hostOnly` is what
 * distinguishes "Domain=example.com" (matches subdomains) from a cookie set
 * without the attribute (matches that one host only). */
struct EWebCookie {
    std::string name;
    std::string value;
    std::string domain;      /* lower case */
    std::string path;        /* always begins with '/' */
    int64_t     expiresMs;   /* wall clock ms; negative = session cookie */
    int         sameSite;    /* EWebCookieSameSite */
    bool        hostOnly;
    bool        secure;
    bool        httpOnly;
};

class EWebCookieJar {
public:
    /* Lazily created: pthread_mutex_init() may allocate a kernel resource,
     * which must not happen before the process is up (i.e. not in a static
     * ctor). */
    static EWebCookieJar& instance();

    /* ---- HTTP side (download worker thread) ---- */
    /* Value for the request's Cookie: header, or "" when nothing may be sent.
     * `pageUrl` is the document that initiated the fetch and `topLevel` marks
     * the main-document navigation itself; the pair drives SameSite. An empty
     * `pageUrl` means "no initiator known" and is treated as same-site. */
    std::string requestHeader(const std::string& url,
                              const std::string& pageUrl,
                              bool topLevel) const;
    /* Store every Set-Cookie header of one response (a response may carry
     * several; each is one "name=value; attr; attr" string). */
    void storeResponseCookies(const std::string& url,
                              const std::vector<std::string>& setCookieHeaders);

    /* ---- document.cookie side (engine thread) ---- */
    /* Same matching as requestHeader() minus the HttpOnly cookies, which the
     * DOM must never see. */
    std::string jsGet(const std::string& url) const;
    /* One Set-Cookie-shaped string from a script, stored against the document's
     * own site. Attributes are honoured (Path/Domain/Max-Age/Expires), except
     * that HttpOnly is dropped: only a server may set it. */
    void jsSet(const std::string& url, const std::string& setCookie);

    void   clear();
    size_t count() const;

    /* ---- URL helpers ----
     * Public because EWebContainer::loadURL logs the same split. */
    static std::string hostOf(const std::string& url);
    static std::string pathOf(const std::string& url);
    /* Registrable domain of a host ("www.shop.example.co.uk" ->
     * "example.co.uk", "localhost"/IP literal -> itself). Approximates the
     * public suffix list with a shape rule (a two-letter TLD preceded by a
     * known second-level label such as co/com/org eats one more label), so a
     * genuine public suffix like "github.io" is not recognised and stays
     * cookie-shareable between its subdomains. */
    static std::string siteOf(const std::string& host);

private:
    EWebCookieJar();
    ~EWebCookieJar();
    EWebCookieJar(const EWebCookieJar&);
    EWebCookieJar& operator=(const EWebCookieJar&);

    /* pthread_once() trampoline: the ctor is private, so the singleton cannot
     * be created from a free function. */
    static void create();

    /* Split `url` into the site key cookies are filed under. Non-HTTP URLs get
     * their scheme as a pseudo-host ("file", "about") and "/" as the path, so
     * every local document shares one bucket instead of being scoped per
     * directory. Returns an empty host for a URL with no scheme. */
    static void splitUrl(const std::string& url, std::string& host,
                         std::string& path, bool& secure);

    bool store(const std::string& url, const std::string& setCookie, bool fromHttp);
    std::string buildHeader(const std::string& url, const std::string& pageUrl,
                            bool topLevel, bool skipHttpOnly) const;

    /* Insertion order is creation order, which is the tie-breaker RFC 6265
     * asks for when two cookies share a path length. */
    std::vector<EWebCookie>  m_cookies;
    /* Both threads reach the jar (the download worker stores Set-Cookie while
     * the engine thread reads/writes document.cookie), so every access locks
     * it. */
    mutable pthread_mutex_t m_mutex;
};

}
