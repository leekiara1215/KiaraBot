# 单机最小可运行版

- 范围确认：2026-11-01晚间，按用户“最快跑通流程、非核心功能后移”的最新要求。
- 当前工作：固定处理器与真实消息链路验收已提交；一键启动与交互入口正在集成。
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

使用匹配本次源码的reference可执行文件，不使用旧目录残留文件：

```bash
cmake --build build/full-locked --target kiarabot-reference-core kiarabot-control-credentials
uv sync --project adapters/nonebot --locked --python 3.11.18
uv sync --project sdk/python --locked --python 3.11.18
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py proto
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
```

首次CMake依赖安装与固定环境见[工具链](11-toolchain.md)，不能将其他电脑的build/venv
直接复制当作已配置。完整锁文件和代码生成是前置；不为了启动临时升级依赖。

一键脚本与准确CLI将在集成验收后写入本节；尚未集成时不能把本段当作已运行的命令。

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
