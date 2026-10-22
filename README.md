# flexkvm-tailscale

Tailscale VPN ARMv7 交叉编译

## 版本

- Tailscale: v1.104.2
- Go: >= 1.27.3（低于此版本时 Makefile 会通过 `GOTOOLCHAIN` 自动选用/下载对应工具链）

## 编译

```bash
make
```

## 输出

```
out/
├── tailscale    # CLI 工具
└── tailscaled   # 守护进程
```

## 工具链

- 目标平台: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
- 默认工具链前缀: `arm-rockchip830-linux-uclibcgnueabihf-`

## 自定义工具链

```bash
export CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
make
```

## C 控制台（替代官方 `tailscale` CLI）

官方 `tailscale` 是独立二进制（2.4 MB），构建时**不再编译**
（`cmd/tailscale` 已从 go build 目标里移除），改由 `cli/tailscale-cli.c` 提供：

```bash
make cli      # 只编 C 控制台 → out/tailscale
```

| | |
|---|---|
| 体积 | **34 KB**（官方 2,472,048 B 的 1/72）|
| 依赖 | `libvpnctl.a` + jansson + OpenSSL（设备已有）|
| 通信 | tailscaled 的 LocalAPI（Unix socket），**不 popen** |

命令面（13 个，全部实测）：

```
up / down / status / ip / login / logout / set / get
ping / whois / whoami / netcheck / version
```

`ping` 走 `/localapi/v0/ping`（受 FormValue 约束，正文必须是
`application/x-www-form-urlencoded`）；`netcheck` 自带 STUN 客户端与
DERP 延迟探测，口径对齐上游 `net/netcheck`。

**未覆盖**：`ssh` / `nc`（数据面，需完整 SSH 客户端与 WG 用户态栈）、
`bugreport` / `metrics` / `debug`（纯诊断）、`service` / `configure`
（systemd 集成）、`completion` / `licenses`（桌面便利）。

> netcheck 的 `PortMapping` / `CaptivePortal` / ICMP 三项未实现
> （分别需要 portmapper、明文 HTTP 探测、raw socket）。
