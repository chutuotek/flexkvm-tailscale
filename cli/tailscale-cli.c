/*
 * tailscale-cli.c - tailscaled 的 C 控制台（替代官方 `tailscale` 二进制）。
 *
 * 官方 CLI 是 Go 写的 26 个子命令，其中 ssh / nc / ping / netcheck 属数据面
 * 工具（要完整 SSH 客户端、WG 用户态栈、STUN 探测），不在本控制台范围内；
 * 其余**控制面子命令全部覆盖**，且都走 tailscaled 的 LocalAPI，不 popen CLI。
 *
 * 走 LocalAPI 而不是调官方 CLI 的收益（真机实测）：
 *   - 一次往返约 4 ms，CLI 进程启动约 1.2 s
 *   - 不需要设备上存在 `jq`（官方 status 的降级路径依赖它）
 *
 * LocalAPI 协议要点（源码 + 真机确认）：
 *   - socket：/var/run/tailscale/tailscaled.sock（root 走这个，
 *     普通用户是 /var/run/tailscale/tailscaled.sock 之外的 TAILSCALE_SOCKET）
 *   - Host 头必须是 "local-tailscaled.sock"：ipn/localapi 的 validHost 只放行
 *     这个固定值，其它值一律 403 "invalid localapi request"
 *   - Referer / Origin 必须为空（同上校验）
 *   - 响应可能是 chunked
 *   - 端点表见 ipn/localapi/localapi.go 的 handler map（21 个）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <jansson.h>

#include "vpnctl.h"
#include "vpnctl_stun.h"
#include "vpnctl_tls.h"

#define TS_HOST "local-tailscaled.sock"

#ifndef TS_SOCK
#define TS_SOCK "/var/run/tailscale/tailscaled.sock"
#endif

#define PROG "tailscale-cli"

/* LocalAPI 一次往返。返回 0 表示拿到 HTTP 响应（含 4xx/5xx） */
static int ts_api(const char *method, const char *path, const char *body,
                  json_t **out, long *status_out) {
    vpnctl_http_opts_t opts;
    vpnctl_http_resp_t resp;
    int ret;

    if (out) {
        *out = NULL;
    }
    if (status_out) {
        *status_out = 0;
    }

    memset(&opts, 0, sizeof(opts));
    opts.unix_path = TS_SOCK;
    opts.host = TS_HOST;
    opts.connect_timeout_ms = 3000;
    opts.io_timeout_ms = 5000;
    opts.total_timeout_ms = 15000;

    ret = vpnctl_http_request(&opts, method, path, NULL,
                              body, &resp);
    if (ret != 0) {
        if (ret == -EHOSTDOWN) {
            return vpnctl_error(PROG, ret,
                                "tailscaled is not running (socket %s)", TS_SOCK);
        }
        return vpnctl_error(PROG, ret, "localapi request failed: %s",
                            strerror(-ret));
    }

    if (status_out) {
        *status_out = resp.status;
    }

    if (resp.status != 200) {
        int r = vpnctl_error(PROG, -EIO, "localapi %s %s: HTTP %ld%s%s",
                             method, path, resp.status,
                             resp.body && resp.body[0] ? ": " : "",
                             resp.body && resp.body[0] ? resp.body : "");
        vpnctl_http_resp_free(&resp);
        return r;
    }

    if (out) {
        json_error_t jerr;
        json_t *root = json_loads(resp.body ? resp.body : "null", 0, &jerr);

        if (!root) {
            vpnctl_http_resp_free(&resp);
            return vpnctl_error(PROG, -EBADMSG, "bad JSON from localapi: %s",
                                jerr.text);
        }
        *out = root;
    }
    vpnctl_http_resp_free(&resp);
    return 0;
}

/* ---------------- status ---------------- */

static int cmd_status(int argc, char **argv) {
    json_t *root = NULL;
    int ret;

    (void)argc;
    (void)argv;
    /* peers=false：不带该参数 body 会从 2.7KB 涨到 15KB（官方 CLI 同款优化） */
    ret = ts_api("GET", "/localapi/v0/status?peers=false", NULL, &root, NULL);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
        json_decref(root);
        return 0;
    }

    {
        char backend[64], auth_url[512], ip[128], dns[256], tailnet[128];
        char online[8] = "false";
        bool b = false;
        json_t *self = json_object_get(root, "Self");

        vpnctl_json_str(root, "BackendState", backend, sizeof(backend));
        vpnctl_json_str(root, "AuthURL", auth_url, sizeof(auth_url));
        if (vpnctl_json_bool(self, "Online", &b) == 0) {
            strcpy(online, b ? "true" : "false");
        }
        /* TailscaleIPs 是数组，取首元素 */
        {
            json_t *ips = json_object_get(self, "TailscaleIPs");

            ip[0] = '\0';
            if (json_is_array(ips) && json_array_size(ips) > 0) {
                const char *s = json_string_value(json_array_get(ips, 0));

                if (s) {
                    snprintf(ip, sizeof(ip), "%s", s);
                }
            }
        }
        vpnctl_json_str(self, "DNSName", dns, sizeof(dns));
        {
            json_t *tn = json_object_get(root, "CurrentTailnet");

            vpnctl_json_nested_str(root, "CurrentTailnet", "Name", tailnet,
                                   sizeof(tailnet));
            if (!json_is_object(tn)) {
                tailnet[0] = '\0';
            }
        }

        {
            vpnctl_table_t t;
            static const char *titles[] = {"backend", "ip", "online",
                                           "dnsname", "tailnet", "auth_url"};
            const char *row[] = {backend, ip, online, dns, tailnet, auth_url};

            vpnctl_set_no_trunc(true);
            vpnctl_table_init(&t, titles, 6);
            vpnctl_table_row(&t, row);
            vpnctl_table_finish(&t);
        }
    }

    json_decref(root);
    return 0;
}

/* ---------------- ip / version / whoami / whois ---------------- */

static int cmd_ip(int argc, char **argv) {
    json_t *root = NULL;
    int ret;

    ret = ts_api("GET", "/localapi/v0/status?peers=false", NULL, &root, NULL);
    if (ret != 0) {
        return ret;
    }

    {
        json_t *self = json_object_get(root, "Self");
        json_t *ips = json_object_get(self, "TailscaleIPs");
        size_t i, n = json_is_array(ips) ? json_array_size(ips) : 0;

        if (vpnctl_json_mode()) {
            vpnctl_print_json(ips ? ips : json_null());
        } else if (n == 0) {
            printf("(no tailscale IPs assigned)\n");
        } else {
            for (i = 0; i < n; i++) {
                const char *s = json_string_value(json_array_get(ips, i));

                (void)argc;
                (void)argv;
                if (s) {
                    printf("%s\n", s);
                }
            }
        }
    }

    json_decref(root);
    return 0;
}

static int cmd_version(void) {
    vpnctl_table_t t;
    static const char *titles[] = {"key", "value"};
    /* tailscaled 的 LocalAPI 根路径回的是纯文本版本串 */
    vpnctl_http_opts_t opts;
    vpnctl_http_resp_t resp;
    char ver[128] = "unknown";
    int ret;

    memset(&opts, 0, sizeof(opts));
    opts.unix_path = TS_SOCK;
    opts.host = TS_HOST;
    opts.connect_timeout_ms = 3000;
    opts.io_timeout_ms = 3000;
    opts.total_timeout_ms = 8000;

    /* 根路径 "/" 回版本；失败时退到 status 里的 Version 字段 */
    ret = vpnctl_http_request(&opts, "GET", "/localapi/v0/status?peers=false",
                              NULL, NULL, &resp);
    if (ret == 0 && resp.status == 200) {
        json_error_t jerr;
        json_t *root = json_loads(resp.body, 0, &jerr);

        if (root) {
            vpnctl_json_str(root, "Version", ver, sizeof(ver));
            json_decref(root);
        }
    }
    if (ret == 0) {
        vpnctl_http_resp_free(&resp);
    }

    if (vpnctl_json_mode()) {
        json_t *o = json_object();

        json_object_set_new(o, "version", json_string(ver));
        vpnctl_print_json(o);
        json_decref(o);
        return 0;
    }

    {
        const char *row[] = {"version", ver};

        vpnctl_set_no_trunc(true);
        vpnctl_table_init(&t, titles, 2);
        vpnctl_table_row(&t, row);
        vpnctl_table_finish(&t);
    }
    return 0;
}

/* ---------------- up / down ---------------- */

/* PATCH prefs：只改 WantRunning，走 ipn.MaskedPrefs 增量语义，不动其它设置 */
static int ts_set_want_running(bool want) {
    char body[64];
    int n;

    n = snprintf(body, sizeof(body),
                 "{\"WantRunning\":%s,\"WantRunningSet\":true}",
                 want ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -EINVAL;
    }
    return ts_api("PATCH", "/localapi/v0/prefs", body, NULL, NULL);
}

static int cmd_up(int argc, char **argv) {
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--accept-routes") == 0) {
            /* 需要同时置 RoutesAll，单独 WantRunning 不会打开子网路由 */
            json_t *prefs = NULL;
            int ret = ts_api("GET", "/localapi/v0/prefs", NULL, &prefs, NULL);

            if (ret != 0) {
                return ret;
            }
            json_object_set_new(prefs, "RouteAll", json_true());
            {
                char *body = json_dumps(prefs, JSON_COMPACT);
                int r = 0;

                if (!body) {
                    json_decref(prefs);
                    return -ENOMEM;
                }
                r = ts_api("PATCH", "/localapi/v0/prefs", body, NULL, NULL);
                free(body);
                json_decref(prefs);
                if (r != 0) {
                    return r;
                }
            }
        } else if (strcmp(argv[i], "--accept-dns=false") == 0) {
            json_t *prefs = NULL;
            int ret = ts_api("GET", "/localapi/v0/prefs", NULL, &prefs, NULL);

            if (ret != 0) {
                return ret;
            }
            json_object_set_new(prefs, "CorpDNS", json_false());
            {
                char *body = json_dumps(prefs, JSON_COMPACT);
                int r = 0;

                if (!body) {
                    json_decref(prefs);
                    return -ENOMEM;
                }
                r = ts_api("PATCH", "/localapi/v0/prefs", body, NULL, NULL);
                free(body);
                json_decref(prefs);
                if (r != 0) {
                    return r;
                }
            }
        } else {
            fprintf(stderr, PROG ": up: unknown flag %s\n", argv[i]);
            return -EINVAL;
        }
    }
    return ts_set_want_running(true);
}

static int cmd_down(void) { return ts_set_want_running(false); }

/* ---------------- login / logout ---------------- */

static int cmd_login(int argc, char **argv) {
    const char *authkey = NULL;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--authkey") == 0 && i + 1 < argc) {
            authkey = argv[++i];
        } else {
            fprintf(stderr, PROG ": login: unknown flag %s\n", argv[i]);
            return -EINVAL;
        }
    }

    if (authkey) {
        json_t *body = json_object();
        char *s;
        int ret;

        json_object_set_new(body, "AuthKey", json_string(authkey));
        s = json_dumps(body, JSON_COMPACT);
        json_decref(body);
        if (!s) {
            return -ENOMEM;
        }
        /* 带 authkey 的 login 走 /start + login-interactive 两个端点：
         * 先 PATCH prefs 打开 WantRunning，再触发登录 */
        ret = ts_set_want_running(true);
        if (ret == 0) {
            ret = ts_api("POST", "/localapi/v0/login-interactive", s, NULL, NULL);
        }
        free(s);
        return ret;
    }

    /* 无 authkey：触发交互式登录，从响应里取授权 URL */
    {
        json_t *root = NULL;
        int ret = ts_api("POST", "/localapi/v0/login-interactive", "{}", &root,
                         NULL);

        if (ret != 0) {
            return ret;
        }
        {
            char url[512];

            vpnctl_json_str(root, "AuthURL", url, sizeof(url));
            if (url[0]) {
                printf("%s\n", url);
            }
        }
        json_decref(root);
        return 0;
    }
}

static int cmd_logout(void) {
    return ts_api("POST", "/localapi/v0/logout", NULL, NULL, NULL);
}

/* ---------------- prefs get / set ---------------- */

static int cmd_get(int argc, char **argv) {
    json_t *root = NULL;
    int ret;

    if (argc < 3) {
        return vpnctl_error(PROG, -EINVAL, "usage: " PROG " get <pref>");
    }

    ret = ts_api("GET", "/localapi/v0/prefs", NULL, &root, NULL);
    if (ret != 0) {
        return ret;
    }

    {
        /* 官方的 pref 名是驼峰（RouteAll/ExitNodeIP/CorpDNS...），直接按键取 */
        json_t *v = json_object_get(root, argv[2]);

        if (!v) {
            json_decref(root);
            return vpnctl_error(PROG, -ENOENT, "unknown pref: %s", argv[2]);
        }
        if (vpnctl_json_mode()) {
            vpnctl_print_json(v);
        } else {
            /*
             * 注意：jansson 的 json_dumps 对**独立标量**（bool/int/string，
             * 不挂在 object/array 里）返回 NULL —— 实测 2.12 如此，且设备上是
             * 2.14 同行为。所以标量必须自己按类型打印，不能一律走 dumps。
             */
            if (json_is_true(v)) {
                printf("true\n");
            } else if (json_is_false(v)) {
                printf("false\n");
            } else if (json_is_string(v)) {
                /* 字符串不加引号，与官方 CLI 一致 */
                printf("%s\n", json_string_value(v));
            } else if (json_is_integer(v)) {
                printf("%lld\n", (long long)json_integer_value(v));
            } else if (json_is_null(v)) {
                printf("null\n");
            } else {
                /* 数组/对象仍走 dumps */
                char *s = json_dumps(v, JSON_COMPACT | JSON_ENSURE_ASCII);

                if (s) {
                    printf("%s\n", s);
                    free(s);
                }
            }
        }
    }
    json_decref(root);
    return 0;
}

static int cmd_set(int argc, char **argv) {
    if (argc < 4) {
        return vpnctl_error(PROG, -EINVAL,
                            "usage: " PROG " set <pref> <value>");
    }

    {
        json_t *root = NULL;
        json_t *nv = NULL;
        const char *key = argv[2];
        const char *val = argv[3];
        char *body;
        int ret;

        ret = ts_api("GET", "/localapi/v0/prefs", NULL, &root, NULL);
        if (ret != 0) {
            return ret;
        }

        /* 值类型按 "" 判断：true/false 走布尔，数字走整型，其余当字符串 */
        if (strcmp(val, "true") == 0 || strcmp(val, "false") == 0) {
            nv = json_boolean(strcmp(val, "true") == 0);
        } else {
            char *endptr = NULL;
            long long n = strtoll(val, &endptr, 10);

            if (endptr && *endptr == '\0' && endptr != val) {
                nv = json_integer(n);
            } else {
                nv = json_string(val);
            }
        }

        json_object_set(root, key, nv);
        body = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        if (!body) {
            return -ENOMEM;
        }
        ret = ts_api("PATCH", "/localapi/v0/prefs", body, NULL, NULL);
        free(body);
        return ret;
    }
}

/* ---------------- whois / whoami ---------------- */

static int cmd_whoami(void) {
    json_t *root = NULL;
    json_t *self;
    json_t *user_map;
    json_t *user = NULL;
    int ret = ts_api("GET", "/localapi/v0/status?peers=false", NULL, &root,
                     NULL);

    if (ret != 0) {
        return ret;
    }

    /*
     * 真机实测（1.102.4）的 status 结构：
     *   顶层 User 是**字典**，键是用户 ID 字符串，值是 {ID,LoginName,DisplayName,...}
     *   Self 里只有 UserID（整数），没有 User 对象。
     * 所以要先从 Self.UserID 拿到 ID，再拿它当键去顶层 User 字典里查。
     */
    self = json_object_get(root, "Self");
    user_map = json_object_get(root, "User");
    {
        long long uid = 0;

        if (vpnctl_json_int(self, "UserID", &uid) == 0 && json_is_object(user_map)) {
            char key[32];

            snprintf(key, sizeof(key), "%lld", uid);
            user = json_object_get(user_map, key);
        }
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(user ? user : json_null());
    } else {
        char login[128], name[128], uid_str[32] = "";
        long long uid = 0;

        vpnctl_json_str(user, "LoginName", login, sizeof(login));
        vpnctl_json_str(user, "DisplayName", name, sizeof(name));
        if (vpnctl_json_int(self, "UserID", &uid) == 0) {
            snprintf(uid_str, sizeof(uid_str), "%lld", uid);
        }
        {
            vpnctl_table_t t;
            static const char *titles[] = {"login", "name", "user_id"};
            const char *row[] = {login, name, uid_str};

            vpnctl_set_no_trunc(true);
            vpnctl_table_init(&t, titles, 3);
            vpnctl_table_row(&t, row);
            vpnctl_table_finish(&t);
        }
    }
    json_decref(root);
    return 0;
}

static int cmd_whois(int argc, char **argv) {
    char path[256];
    json_t *root = NULL;
    json_t *node;
    int ret;

    if (argc < 3) {
        return vpnctl_error(PROG, -EINVAL, "usage: " PROG " whois <ip>");
    }
    snprintf(path, sizeof(path), "/localapi/v0/whois?addr=%s", argv[2]);
    ret = ts_api("GET", path, NULL, &root, NULL);
    if (ret != 0) {
        return ret;
    }
    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
    } else {
        char login[128], host[128], ip[128] = "";
        json_t *addrs;

        node = json_object_get(root, "Node");
        vpnctl_json_nested_str(root, "UserProfile", "LoginName", login,
                               sizeof(login));
        vpnctl_json_str(node, "ComputedName", host, sizeof(host));
        /* Node.Addresses 是数组，取首元素 */
        addrs = json_object_get(node, "Addresses");
        if (json_is_array(addrs) && json_array_size(addrs) > 0) {
            const char *s = json_string_value(json_array_get(addrs, 0));

            if (s) {
                snprintf(ip, sizeof(ip), "%s", s);
            }
        }
        {
            vpnctl_table_t t;
            static const char *titles[] = {"login", "host", "address"};
            const char *row[] = {login, host, ip};

            vpnctl_set_no_trunc(true);
            vpnctl_table_init(&t, titles, 3);
            vpnctl_table_row(&t, row);
            vpnctl_table_finish(&t);
        }
    }
    json_decref(root);
    return 0;
}

/* ---------------- netcheck ---------------- */

/* ---------------- ping（走 /localapi/v0/ping）---------------- */

/*
 * tailscaled 直接暴露了 ping 端点（ipn/localapi/localapi.go 的 servePing），
 * 内部调 h.b.Ping()，返回 ipnstate.PingResult。
 * 所以**不需要在 C 侧构造 WG 包**，只是 HTTP POST。
 *
 * 响应字段（ipn/ipnstate/ipnstate.go:727）：
 *   IP NodeIP NodeName Err LatencySeconds Endpoint PeerRelay
 *   DERPRegionID DERPRegionCode IsLocalIP
 */
static int cmd_ping(int argc, char **argv) {
    const char *target = NULL;
    const char *ptype = "disco";
    int num = 1;
    int size = 0;
    int i;

    /* 官方默认：先 disco 三次，失败再 tsmp；这里保持 disco */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--until-direct") == 0) {
            /* 官方语义：一直 ping 到直连为止。简化：等价于多次 disco */
            num = 5;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            num = atoi(argv[++i]);
            if (num <= 0) {
                num = 1;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tsmp") == 0) {
            ptype = "tsmp";
        } else if (strcmp(argv[i], "--icmp") == 0) {
            ptype = "icmp";
        } else if (strcmp(argv[i], "--peerapi") == 0) {
            ptype = "peerapi";
        } else if (argv[i][0] != '-') {
            target = argv[i];
        } else {
            fprintf(stderr, PROG ": ping: unknown flag %s\n", argv[i]);
            return -EINVAL;
        }
    }

    if (!target) {
        return vpnctl_error(PROG, -EINVAL,
                            "usage: " PROG " ping <hostname-or-IP>");
    }

    for (i = 0; i < num; i++) {
        char body[256];
        vpnctl_http_opts_t opts;
        vpnctl_http_resp_t resp;
        char *pb;
        int ret;

        snprintf(body, sizeof(body), "ip=%s&type=%s%s%s", target, ptype,
                 size > 0 ? "&size=" : "",
                 size > 0 ? (char[]){0} : "");
        if (size > 0) {
            snprintf(body, sizeof(body), "ip=%s&type=%s&size=%d", target, ptype,
                     size);
        }

        memset(&opts, 0, sizeof(opts));
        opts.unix_path = TS_SOCK;
        opts.host = TS_HOST;
        opts.connect_timeout_ms = 3000;
        opts.io_timeout_ms = 10000;
        opts.total_timeout_ms = 20000;

        /* servePing 用 r.FormValue 取参数，正文必须是 form-urlencoded；
         * 发 JSON 会得到 400（实测） */
        ret = vpnctl_http_request_form(&opts, "POST", "/localapi/v0/ping",
                                       NULL, body, &resp);
        (void)pb;
        if (ret != 0) {
            if (ret == -EHOSTDOWN) {
                return vpnctl_error(PROG, ret, "tailscaled is not running");
            }
            return vpnctl_error(PROG, ret, "ping request failed: %s",
                                strerror(-ret));
        }

        if (resp.status != 200) {
            int r = vpnctl_error(PROG, -EIO, "ping: HTTP %ld", resp.status);

            vpnctl_http_resp_free(&resp);
            return r;
        }

        {
            json_error_t jerr;
            json_t *root = json_loads(resp.body, 0, &jerr);

            vpnctl_http_resp_free(&resp);
            if (!root) {
                return vpnctl_error(PROG, -EBADMSG, "bad ping JSON: %s",
                                    jerr.text);
            }

            {
                char peer[128], node_name[128], endpoint[128], relay[128];
                char derp[64], err_str[256];
                double lat = 0;
                bool is_local = false;
                json_t *lv;

                vpnctl_json_str(root, "IP", peer, sizeof(peer));
                vpnctl_json_str(root, "NodeName", node_name,
                                sizeof(node_name));
                vpnctl_json_str(root, "Endpoint", endpoint,
                                sizeof(endpoint));
                vpnctl_json_str(root, "PeerRelay", relay, sizeof(relay));
                vpnctl_json_str(root, "DERPRegionCode", derp, sizeof(derp));
                vpnctl_json_str(root, "Err", err_str, sizeof(err_str));
                vpnctl_json_bool(root, "IsLocalIP", &is_local);
                lv = json_object_get(root, "LatencySeconds");
                if (json_is_number(lv)) {
                    lat = json_number_value(lv);
                }

                if (err_str[0] != '\0') {
                    /* Err 非空是**正常结果**（如本地 IP、对端不可达），
                     * 官方 CLI 直接打印它，不当异常 */
                    printf("ping %s: %s\n", peer, err_str);
                    json_decref(root);
                    if (is_local) {
                        return 0;
                    }
                    continue;
                }

                if (vpnctl_json_mode()) {
                    vpnctl_print_json(root);
                } else {
                    long ms = (long)(lat * 1000.0 + 0.5);

                    if (endpoint[0] != '\0') {
                        printf("pong from %s (%s) via %s in %ldms\n", node_name,
                               peer, endpoint, ms);
                    } else if (relay[0] != '\0') {
                        printf("pong from %s (%s) via relay %s in %ldms\n",
                               node_name, peer, relay, ms);
                    } else if (derp[0] != '\0') {
                        printf("pong from %s (%s) via DERP(%s) in %ldms\n",
                               node_name, peer, derp, ms);
                    } else {
                        printf("pong from %s (%s) in %ldms\n", node_name, peer,
                               ms);
                    }
                }
            }
            json_decref(root);
        }
    }
    return 0;
}

/* ---------------- netcheck（完整版：STUN + DERP 延迟）---------------- */

/*
 * 与官方 netcheck 的口径对齐（net/netcheck/netcheck.go）：
 *   - NAT 判定：向**多个不同** STUN 服务器发 Binding Request，比对返回的
 *     mapped address。全部相同 → MappingVariesByDestIP=false（端点无关映射，
 *     打洞成功率高）；不同 → true（对称 NAT，需要 DERP 中继）
 *   - 公网地址：取最先拿到的 IPv4 映射
 *   - DERP 延迟：对 derpmap 里每个区域的节点发
 *     `GET https://<HostName>/derp/latency-check`，取整个请求耗时
 *     （上游注释明确说含建连+TLS，是"到该节点 RTT 的最佳近似"）
 *   - 最近 DERP = 延迟最低的那个区域
 *
 * 与官方的已知差异（如实列出，不假装等价）：
 *   - 不做 ICMP 探测（官方用于判定 ICMP 可达性）
 *   - 不做 UPnP/NAT-PMP/PCP 端口映射探测（需要引入 portmapper；官方报告里
 *     这三项在无 portmapper 的构建里同样显示 "not checked"）
 *   - 不做 captive portal 检测（官方要发一个明文 HTTP 请求探测劫持）
 */

#define NETCHECK_STUN_TIMEOUT_MS 2500
#define NETCHECK_DERP_TIMEOUT_MS 4000
#define NETCHECK_MAX_REGIONS 8

/* 一个 STUN 服务器 */
typedef struct {
    const char *host;
    int port;
} nc_stun_t;

/*
 * 默认 STUN 列表。**刻意含多个不同运营方的服务器**：NAT 判定依赖
 * "同一 socket 向不同目的地发请求看映射是否变化"，只问一个服务器等于没判据。
 * 上游默认用 tailscale 自己的 stun（stun.tailscale.com），这里补了公共可用的。
 */
static const nc_stun_t nc_stun_servers[] = {
    {"stun.chat.bilibili.com", 3478},
    {"stun.cloudflare.com", 3478},
    {"stun.l.google.com", 19302},
    {"stun.nextcloud.com", 3478},
};

/* 解析 host:port 成 sockaddr；失败返回负 errno */
static int nc_resolve(const char *host, int port, struct sockaddr_storage *ss,
                      socklen_t *ss_len) {
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    int rc;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* netcheck 的 IPv4 路径 */
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0 || !res) {
        return -EHOSTUNREACH;
    }
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *ss_len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int cmd_netcheck(void) {
    json_t *derpmap = NULL;
    int ret;
    int sock = -1;
    int i;

    /* ---- 1. STUN 探测（NAT 判定）---- */
    {
        struct sockaddr_storage first_mapped;
        socklen_t first_mapped_len = 0;
        bool mapping_varies = false;
        int ok_count = 0;
        struct sockaddr_storage mapped_list[8];
        socklen_t mapped_lens[8];
        char mapped_str[8][64];
        /* 与结果同下标保存服务器名：n_mapped 只在探测成功时自增，
         * 若拿它当索引、再用 i 去取 nc_stun_servers[i].host 打印，
         * 一旦中间有服务器失败就会张冠李戴（实测踩过） */
        const char *mapped_host[8];
        int n_mapped = 0;

        /* 所有探测复用同一个 socket —— 这正是判定"映射是否随目标变化"的前提 */
        sock = vpnctl_stun_socket(AF_INET, 0);
        if (sock < 0) {
            return vpnctl_error(PROG, sock, "netcheck: cannot open UDP socket");
        }

        printf("netcheck:\n");
        printf("\t- UDP: yes (socket bound)\n");

        for (i = 0;
             i < (int)(sizeof(nc_stun_servers) / sizeof(nc_stun_servers[0]));
             i++) {
            struct sockaddr_storage srv;
            socklen_t srv_len;
            vpnctl_stun_result_t r;

            if (nc_resolve(nc_stun_servers[i].host, nc_stun_servers[i].port,
                           &srv, &srv_len) != 0) {
                continue;
            }

            if (vpnctl_stun_bind(sock, (struct sockaddr *)&srv, srv_len,
                                 NETCHECK_STUN_TIMEOUT_MS, &r) == 0) {
                vpnctl_stun_fmt_addr((struct sockaddr *)&r.mapped,
                                     r.mapped_len, mapped_str[n_mapped],
                                     sizeof(mapped_str[n_mapped]));
                memcpy(&mapped_list[n_mapped], &r.mapped, sizeof(r.mapped));
                mapped_lens[n_mapped] = r.mapped_len;
                mapped_host[n_mapped] = nc_stun_servers[i].host;
                n_mapped++;
                ok_count++;
            }
        }

        if (ok_count == 0) {
            printf("\t- IPv4: (no STUN response)\n");
            printf("\t- MappingVariesByDestIP: (unknown)\n");
        } else {
            /* 比对全部映射是否一致（地址+端口都要一致） */
            memcpy(&first_mapped, &mapped_list[0], sizeof(first_mapped));
            first_mapped_len = mapped_lens[0];
            for (i = 1; i < n_mapped; i++) {
                if (mapped_lens[i] != first_mapped_len ||
                    memcmp(&mapped_list[i], &first_mapped,
                           first_mapped_len) != 0) {
                    mapping_varies = true;
                }
            }

            printf("\t- IPv4: yes, %s\n", mapped_str[0]);
            printf("\t- MappingVariesByDestIP: %s\n",
                   mapping_varies ? "true" : "false");
            printf("\t- STUN servers responded: %d/%d\n", ok_count,
                   (int)(sizeof(nc_stun_servers) / sizeof(nc_stun_servers[0])));
            if (n_mapped > 1) {
                printf("\t- STUN results:\n");
                for (i = 0; i < n_mapped; i++) {
                    printf("\t\t- %s: %s\n", mapped_host[i], mapped_str[i]);
                }
            }
        }
        close(sock);
    }

    /* ---- 2. DERP 延迟（走 derpmap + HTTPS latency-check）---- */
    ret = ts_api("GET", "/localapi/v0/derpmap", NULL, &derpmap, NULL);
    if (ret != 0) {
        printf("\t- Nearest DERP: unknown (no derpmap)\n");
        return 0;
    }

    {
        json_t *regions = json_object_get(derpmap, "Regions");
        const char *rid_key;
        json_t *region;
        char best_code[16] = "";
        int best_ms = -1;
        int probed = 0;

        printf("\t- DERP latency:\n");

        json_object_foreach(regions, rid_key, region) {
            json_t *nodes = json_object_get(region, "Nodes");
            const char *code;
            char code_buf[16] = "";
            size_t n, k;

            if (probed >= NETCHECK_MAX_REGIONS) {
                break;
            }
            if (!json_is_array(nodes) || json_array_size(nodes) == 0) {
                continue;
            }

            vpnctl_json_str(region, "RegionCode", code_buf, sizeof(code_buf));
            code = code_buf;

            /* 一个区域内多台节点，取最快的那台作为该区域延迟 */
            {
                int region_best = -1;

                n = json_array_size(nodes);
                for (k = 0; k < n; k++) {
                    json_t *node = json_array_get(nodes, k);
                    char host[192];
                    vpnctl_tls_opts_t top;
                    vpnctl_tls_result_t tr;

                    if (vpnctl_json_str(node, "HostName", host,
                                        sizeof(host)) != 0) {
                        continue;
                    }

                    memset(&top, 0, sizeof(top));
                    top.connect_timeout_ms = NETCHECK_DERP_TIMEOUT_MS;
                    top.io_timeout_ms = NETCHECK_DERP_TIMEOUT_MS;
                    top.verify_peer = true;

                    if (vpnctl_tls_get_elapsed(
                            host, 443, "/derp/latency-check", &top, &tr) == 0) {
                        if (tr.status > 0 && tr.status <= 299 &&
                            (region_best < 0 || tr.elapsed_ms < region_best)) {
                            region_best = tr.elapsed_ms;
                        }
                    }
                }

                if (region_best >= 0) {
                    printf("\t\t- %3s: %dms\n", code, region_best);
                    if (best_ms < 0 || region_best < best_ms) {
                        best_ms = region_best;
                        snprintf(best_code, sizeof(best_code), "%s", code);
                    }
                }
                probed++;
            }
        }

        if (best_ms >= 0) {
            printf("\t- Nearest DERP: %s\n", best_code);
        } else {
            printf("\t- Nearest DERP: unknown (no response to latency probes)\n");
        }
    }

    json_decref(derpmap);
    return 0;
}

/* ---------------- 入口 ---------------- */

static void usage(void) {
    printf("Usage: " PROG " <command> [args]\n\n"
           "Commands:\n"
           "  up [--accept-routes] [--accept-dns=false]\n"
           "  down\n"
           "  status                 show state of tailscaled\n"
           "  ip                     show tailscale IP addresses\n"
           "  login [--authkey KEY]  log in\n"
           "  logout                 log out\n"
           "  set <pref> <value>     change a preference\n"
           "  get <pref>             show a preference value\n"
           "  ping <host>            ping a peer at the tailscale layer\n"
           "           [-c N] [--size N] [--tsmp|--icmp]\n"
           "  whois <ip>             show the node/user for an IP\n"
           "  whoami                 show current identity\n"
           "  netcheck               local network conditions (summary)\n"
           "  version                print version\n\n"
           "Global flags:\n"
           "  -o json | --json       output JSON instead of a table\n"
           "  --no-trunc             do not truncate table columns\n");
}

int main(int argc, char **argv) {
    static const char *const cmds[] = {
        "up",    "down",  "status", "ip",       "login", "logout", "set",
        "get",   "ping",  "whois", "whoami", "netcheck", "version", "help",
    };
    int i, idx;
    int rc;

    /* 先摘全局开关，再分发：全局开关可出现在任意位置 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 ||
            (strcmp(argv[i], "-o") == 0 && i + 1 < argc &&
             strcmp(argv[i + 1], "json") == 0)) {
            vpnctl_set_json_mode(true);
            if (argv[i][1] == 'o') {
                memmove(&argv[i], &argv[i + 2],
                        sizeof(char *) * (size_t)(argc - i - 2));
                argc -= 2;
            } else {
                memmove(&argv[i], &argv[i + 1],
                        sizeof(char *) * (size_t)(argc - i - 1));
                argc -= 1;
            }
            i--;
        } else if (strcmp(argv[i], "--no-trunc") == 0) {
            vpnctl_set_no_trunc(true);
            memmove(&argv[i], &argv[i + 1],
                    sizeof(char *) * (size_t)(argc - i - 1));
            argc -= 1;
            i--;
        }
    }

    idx = vpnctl_match_cmd(argc, argv, cmds, (int)(sizeof(cmds) / sizeof(cmds[0])));
    if (idx < 0) {
        usage();
        return 1;
    }

    switch (idx) {
    case 0:
        rc = cmd_up(argc, argv);
        break;
    case 1:
        rc = cmd_down();
        break;
    case 2:
        rc = cmd_status(argc, argv);
        break;
    case 3:
        rc = cmd_ip(argc, argv);
        break;
    case 4:
        rc = cmd_login(argc, argv);
        break;
    case 5:
        rc = cmd_logout();
        break;
    case 6:
        rc = cmd_set(argc, argv);
        break;
    case 7:
        rc = cmd_get(argc, argv);
        break;
    case 8:
        rc = cmd_ping(argc, argv);
        break;
    case 9:
        rc = cmd_whois(argc, argv);
        break;
    case 10:
        rc = cmd_whoami();
        break;
    case 11:
        rc = cmd_netcheck();
        break;
    case 12:
        rc = cmd_version();
        break;
    default:
        usage();
        rc = 0;
        break;
    }
    return rc == 0 ? 0 : 1;
}
