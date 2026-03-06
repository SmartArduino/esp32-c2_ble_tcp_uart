# ESP32-C2 BLE + TCP + UART (AT) Gateway

本项目实现：

1. **默认 AP 配网**：开机启动 `ESP32C2_CFG` 开放热点（无密码）+ `9000` TCP Server。
2. **收到路由参数后切 STA**：连接路由成功后，按配置连接业务 TCP Server（客户端）。
3. **配置持久化**：Wi-Fi、业务服务端、BLE UUID 写入 NVS，重启自动恢复。
4. **透传关系固定**：仅支持 UART<->TCP Client 与 UART<->BLE，BLE 与 TCP 不直接透传。
5. **支持 AT 配网/连服/BLE参数配置**。
6. **恢复出厂**：AT 指令清空配置。

## 手机 APP 通过 AP TCP 配网

手机连 `ESP32C2_CFG`（开放网络，无密码），向 `9000` 端口发送 JSON：

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

写特征：BLE -> UART 透传。

UART 数据模式下可把 UART 上行同时发往 TCP Client（若已连接）和 BLE Notify（若已连接）。

TCP 下行：仅转发到 UART。

## AT 指令

- `AT`
- `AT+SETCFG=<json>`
- `AT+WIFICFG=<ssid>,<pass>`
- `AT+SRVCFG=<server_ip>,<port>`
- `AT+CONNECT`：STA 已联网时，立即尝试连接业务 TCP Server
- `AT+STATUS?`
- `AT+SAVE`
- `AT+ENTM`：进入 UART 数据透传模式
- `AT+EXIT`：退出 UART 数据透传模式（或发送 `+++`）
- `AT+FACTORY`
- `AT+RST`：重启模组
- `AT+BLEUUID?`
- `AT+BLEUUID=<svc_hex>,<chr_hex>` 例如：`AT+BLEUUID=FFF0,FFF1`

> `AT+BLEUUID=` 修改后会持久化，**重启后生效**（运行中的 GATT 数据库不会热重建）。

## 文件

- `main/app_main.c`：AP 配网、STA、TCP Client、NVS、BLE、AT 主逻辑
- `main/CMakeLists.txt`
- `CMakeLists.txt`


### UART 数据透传模式

- 进入方式：`AT+ENTM`。
- 退出方式：`AT+EXIT` 或连续发送 `+++`。
- 数据路径：
  - UART 上行 -> TCP Client（已连时）
  - UART 上行 -> BLE Notify（已连时）
  - TCP 下行 -> UART
  - BLE Write -> UART



## 编译目标与 Flash 配置

本工程默认按 `ESP32-C2` + `4MB Flash` 使用，建议先执行：

```bash
idf.py set-target esp32c2
idf.py build
```

如需重新生成 sdkconfig，可在 menuconfig 中确认 Flash Size 为 `4MB`。
