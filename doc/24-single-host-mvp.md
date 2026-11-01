# 单机最小可运行版

2026-11-01后续已转入[真实QQ接入](26-real-qq-onboarding.md)：用户授权安装NapCat、
扫码由用户完成，其他功能暂停。后续真实群聊纯文本收发已通过，详情见上述入口；
本页仍单独记录本地平台替身的运行步骤。

- 范围确认：2026-11-01晚间，按用户“最快跑通流程、非核心功能后移”的最新要求。
- 当前状态：启动器、固定处理器、平台替身及真实消息闭环已集成；本机已验收群聊、私聊、live网页登录、持久重启和停止。
- 长期目标架构仍见doc19–23；人类维护的doc18保持原文。

## 首版范围

一台电脑、一个手动配置的账号、一组明确会话白名单，收纯文本并自动回复“收到：原文”。
提供登录、运行状态、停止与保留本机数据的重启。默认连接本地OneBot协议替身，实际
运行NoneBot、C++ Core、SQLite和SDK固定处理器；不需要真实QQ或付费模型。

这条运行轨复用`core/reference/`的单机入口与现有SDK/Adapter协议。固定处理器位于
`plugins/local-reply/`，复用既有SDK通道，不扩建插件安装、发现、市场或热更新管理。
六服务目标中的Runtime消息业务尚未完成；不能用本版验收将P04–P14或架构迁移打勾。

## 先后顺序

1. 先让一键启动、网页/API登录和实际收发演示可复现。
2. 验证重复事件、错误凭据、依赖失败、停止和同目录重启，不以静默忽略错误换取演示。
3. 再根据真实使用反馈选一个必要能力继续。插件机制、完整账号管理、多主机部署，
   以及其余非闭环所需的身份管理、namespace、任务和模型扩展，均不作为今晚前置。

## 环境与源码

需要CMake/C++工具链、固定vcpkg依赖和Python3.11.18；可选网页还需要锁定Node/npm环境。
首次依赖安装见[工具链](11-toolchain.md)及[换机步骤](25-development-handoff.md)。
从仓库根执行；已配置的构建使用下面命令重建，不使用旧目录残留文件：

```bash
cmake --build build/full-locked --target kiarabot-reference-core kiarabot-control-credentials
uv sync --project adapters/nonebot --locked --python 3.11.18
uv sync --project sdk/python --locked --python 3.11.18
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py proto
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
```

新电脑需先安装固定依赖并配置全新CMake目录，不能直接复制其他电脑的build/venv。
`full-locked`要求先设置新机的`VCPKG_ROOT`和triplet；MVP只构建上面两个目标，
不需要启动PostgreSQL或六服务，也不需要执行其数据库测试。保留依赖锁，不临时升级。

## 启动与登录

`start`会自动初始化私有配置，启动本地平台替身、reference Core/Control、SDK处理器和
真实NoneBot。显式指定本轮构建产物，可避免误选旧文件：

```bash
python3 scripts/local/start.py start \
  --core build/full-locked/reference/core/kiarabot-core \
  --credentials build/full-locked/reference/control/api/kiarabot-control-credentials \
  --python adapters/nonebot/.venv/bin/python
python3 scripts/local/start.py status
```

默认账号123、允许回复的会话456、状态目录`data/local`。`start`输出实际`control_url`、
gRPC端口和进程信息；管理员用户名`admin`，密码保存在`data/local/admin-password`，
不会输出到终端。`init`只生成配置，正常`start`已包含该步骤。

网页需要先安装锁定依赖。首次启动时加`--ui`，或先停止现有实例再启用：

```bash
npm ci --prefix panel
python3 scripts/local/start.py stop
python3 scripts/local/start.py start --ui
```

浏览器打开`http://127.0.0.1:5173`，使用上述本机密码登录。页面连接真实Control HTTP/WS，
收到`SESSION_READY`后加载状态；本机已看到“实时连接正常”、adapter connected和
local-reply running。页面不提供本轮消息输入框，收发使用下一节CLI。默认不加`--ui`
则只启动API和消息链路，不要求Node。5173已占用时明确失败，可指定`--ui-port PORT`。

## 实际收发与停止

```bash
# 群聊456，预期收到：hello
adapters/nonebot/.venv/bin/python scripts/local/onebot_demo.py message \
  --token-file data/local/onebot-token --control-socket "$PWD/data/local/demo.sock" \
  --type group --user-id 789 --group-id 456 --text hello --timeout 10

# 私聊用户456，预期收到：private hello
adapters/nonebot/.venv/bin/python scripts/local/onebot_demo.py message \
  --token-file data/local/onebot-token --control-socket "$PWD/data/local/demo.sock" \
  --type private --user-id 456 --text 'private hello' --timeout 10

python3 scripts/local/start.py stop
```

成功输出`ok:true`及实际`send_group_msg`/`send_private_msg`参数。消息经真实NoneBot、
C++ Core和公开SDK处理器往返，替身只模拟平台。默认只允许会话456，不能将私聊用户789
误当作已授权目标。保留现有限流；连续快速探针可能超时，需检查配额/授权/依赖状态，
不能通过关闭规则让演示成功。替身`reply_timeout`只说明等待窗口内未收到匹配回复，
不等于已定位失败原因。重试前先确认原消息与Action状态，避免实际平台重复副作用。

`start`运行中重复执行会返回同一实例；修改参数必须先`stop`。`status`健康退出0，
停止/不健康退出1；正常`stop`和重复`stop`退出0。重启保留密码、密钥、SQLite与outbox。
数据损坏、依赖缺失、子进程退出或可疑PID均明确失败，不静默重建数据库。

macOS的Unix socket绝对路径UTF8长度限制为100字节。目录过长时，所有命令一致加
`--state-dir /private/tmp/kb-local`，并将消息CLI文件/socket路径改为该目录。状态目录
必须为空或已由同一仓库入口拥有、权限0700且不含符号链接；Linux可用`/tmp/kb-local`。

## 验收证据与限制

主集成处理器、真实NoneBot闭环、替身及启动器所有权定向测试合计24/24通过；
另实际验证当前reference构建的默认群/私聊、错误密码401、登录200、WS首帧、REST状态、
live浏览器登录与组件展示、停止子进程/socket清理、同目录重启不重置数据及秘密不入日志。
准确提交和命令见[本轮验收报告](../reports/execution/2026-11-01-single-host-mvp.md)。
此前旧`build/full/core/kiarabot-core`导致的私聊入站冲突已通过当前reference构建消除，
无需修改权限或限流；换机须重建。真实QQ群聊后续验收见doc26；模型、Linux及远端CI未验收。

## 数据与边界

本机运行数据、密码、token和私有配置仅在忽略的`data/`或指定私有state目录，不能提交。
init/start不得重置已有密码或数据库；stop只终止本程序拥有的进程。默认只监听loopback，
Node面板可选启动但必须live模式，禁止误连mock。真实平台仅在用户显式提供其连接配置
后启用；当前本机替身验收不代表真实QQ/NapCat联调或生产发布。

reference的SQLite与新方向四个私有PostgreSQL库是独立运行轨；不跨库读写、不双写、
不自动导入旧开发快照。后续切换运行轨前先备份并明确处置本机试用数据，不静默删除。

## 换电脑续做

未完事项、已完成未合并的提交和WIP、恢复开发顺序、环境重建与验收入口统一见
[跨电脑续开发文档](25-development-handoff.md)。本机存在的提交不代表已推送到远端；
换机须先迁移源码及相关git分支，不依赖此聊天记录。
