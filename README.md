# ESP32-C2 BLE + TCP + UART(AT) 固件

该工程实现以下需求：

1. **默认 AP 配网模式**：设备上电启动 `ESP32C2_PROVISION` 热点，同时开启 TCP Server（默认端口 `7000`）。
2. **手机发送配网信息**：APP 连接热点后向 TCP Server 发送：
   `SSID=<ssid>;PASS=<pass>;HOST=<server>;PORT=<port>`
3. **自动连接路由与上位 TCP Server**：收到配置后，设备切换 STA 连接路由，成功后建立 TCP Client 连接到 `HOST:PORT`。
4. **掉电保存/自动重连**：配置存 NVS，后续开机自动恢复并连接。
5. **BLE GATT Server 透传**：提供自定义 128-bit 服务和特征值，手机可扫描、连接并向特征写入；写入数据透传到 TCP Client，TCP 收到数据可通过 notify 发回 BLE。
6. **AT 指令支持配网/连接/恢复出厂 + 自定义 BLE UUID**。

## 配网 TCP 协议

手机连接 AP 后，向 `7000` 端口发送：

```text
SSID=<ssid>;PASS=<pass>;HOST=<host_or_ip>;PORT=<port>
```

设备返回：

- `OK`：保存并触发连接
- `ERR:FORMAT`：格式错误
- `ERR:STORE`：存储失败

## AT 指令

- `AT+SETCFG=SSID=<ssid>;PASS=<pass>;HOST=<host>;PORT=<port>`：写入 Wi-Fi/TCP 配置并触发连接
- `AT+CONNECT`：按当前配置重新连接路由/TCP server
- `AT+GETCFG?`：查询当前配置（含 BLE UUID）
- `AT+SETBLEUUID=SERVICE=<32hex>;CHAR=<32hex>`：设置 BLE 服务/特征 UUID（128-bit，32位HEX，无`-`）并保存
- `AT+FACTORY`：恢复出厂（擦除 NVS 并重启）

> `AT+SETBLEUUID` 生效方式：返回 `+INFO:REBOOT_TO_APPLY`，重启后生效。

## 构建

```bash
idf.py set-target esp32c2
idf.py build
```
