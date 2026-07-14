# flexkvm-tailscale

Tailscale VPN ARMv7 交叉编译

## 版本

- Tailscale: v1.100.0
- Go: >= 1.26.4

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
