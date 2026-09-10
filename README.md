# 生产助手

基于 Qt Widgets 和 Qt SerialPort 的下位机生产配置工具。

## 功能

- 自动枚举串口，可选择波特率并连接/断开设备
- 五项操作按“左侧操作、右侧独立状态”排列
- 固定密码不会以明文写入收发日志
- 每个操作分别显示等待、进行中、成功或失败
- 支持设备明确拒绝、串口异常和 3 秒响应超时
- 收发记录便于现场排查
- 独立的“4G 路由器配置与检测”标签页，支持单项检测和一键顺序配置

## 默认串口协议

发送内容均以 `\r\n` 结尾：

| 操作 | 默认发送格式 |
| --- | --- |
| 输入密码 | `mei+688` |
| 配置初始时间 | `date 2025 10 30 0 0 0` |
| 修改 UID | 完全按照输入框内容发送，不增加任何命令头、命令尾或换行符 |
| 配置当前时间 | `date 年 月 日 时 分 秒`，例如 `date 2026 9 9 11 20 30` |
| 重启 | `reset` |

UID 操作收到包含 `ok` 的回复（忽略大小写）时判定成功，没有返回时在 3 秒后判定失败。其他操作返回以 `OK`、`ACK`、`SUCCESS`、`PASS` 或“成功”开头时判定成功；以 `ERR`、`ERROR`、`FAIL`、`FAILED`、`NACK`、`NG` 或“失败”开头时判定失败。协议如有不同，请集中修改 `src/serialprotocol.cpp`。

## 4G 路由器配置与检测

四条 AT 指令均以 `\r\n` 结束：

1. `AT*GPSCFG=4,5,0`
2. `AT*SERVER0=9,broker.emqx.io,1883`
3. `AT*MQSET0=00000001,,`
4. `AT*MQTOP0=sendev/1/temperature,sendev/10/temperature`

每条指令只有收到完全大写的独立回复 `OK` 才判定成功。“一键配置”先发送第 1 条，收到大写 `OK` 后才发送下一条；最后一条也收到大写 `OK` 后整体完成。任一步返回错误或 5 秒内没有收到大写 `OK`，流程立即停止并显示失败步骤。

## 构建

需要 Qt 5/6 的 Widgets、SerialPort 组件和 CMake：

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="D:/Qt/6.5.3/mingw_64"
cmake --build build
```

也可以直接用 Qt Creator 打开根目录的 `CMakeLists.txt`，选择带有 SerialPort 组件的 Qt 套件后构建运行。
