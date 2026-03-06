# ESP32-C2 BLE + TCP + UART (AT) Gateway

本项目实现：

1. **默认 AP 配网**：开机启动 `ESP32C2_CFG` 热点 + `9000` TCP Server。
2. **收到路由参数后切 STA**：连接路由成功后，按配置连接业务 TCP Server（客户端）。
3. **配置持久化**：Wi-Fi、业务服务端、BLE UUID 写入 NVS，重启自动恢复。
4. **BLE 外设双向透传**：手机写 BLE 特征会转发到 TCP Client；TCP Server 下行数据会通过 BLE Notify 回传手机。
5. **支持 AT 配网/连服/BLE参数配置**。
6. **恢复出厂**：AT 指令清空配置。

## 手机 APP 通过 AP TCP 配网

手机连 `ESP32C2_CFG`，向 `9000` 端口发送 JSON：

```json
{"wifi_ssid":"HomeWiFi","wifi_pass":"12345678","server_ip":"192.168.1.100","server_port":7001}
```

返回 `OK` 表示配置和保存成功并触发 STA 连接。

## BLE（默认）

- 设备名：`ESP32C2-AT-BLE`
- Service UUID16：`0xFFF0`
- Characteristic UUID16：`0xFFF1`（Read/Write/Notify）

读特征返回：
- `READY`：已完成配网
- `UNPROV`：未配网

写特征：BLE -> TCP 透传。

Notify：TCP -> BLE 透传（需手机订阅通知）。

## AT 指令

- `AT`
- `AT+SETCFG=<json>`
- `AT+CONNECT`：STA 已联网时，立即尝试连接业务 TCP Server
- `AT+STATUS?`
- `AT+SAVE`
- `AT+FACTORY`
- `AT+BLEUUID?`
- `AT+BLEUUID=<svc_hex>,<chr_hex>` 例如：`AT+BLEUUID=FFF0,FFF1`

> `AT+BLEUUID=` 修改后会持久化，**重启后生效**（运行中的 GATT 数据库不会热重建）。

## 文件

- `main/app_main.c`：AP 配网、STA、TCP Client、NVS、BLE、AT 主逻辑
- `main/CMakeLists.txt`
- `CMakeLists.txt`
