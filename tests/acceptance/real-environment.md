# T07b 真实联调环境准备

当前用户尚未配置真实 QQ/NapCat、模型服务与目标会话，T07b 未验收。保持 C++ Core、独立 Python 适配器/SDK 和 TypeScript 面板。

## 1. QQ 与 NapCat

准备测试 QQ 账号和测试私聊/小群，记录机器人 QQ 号、测试对端 QQ/群号、NapCat/QQ 版本。Mac 可继续运行 Core 和独立适配器，NapCat 放在 Linux/Windows，按 [官方安装说明](https://napneko.github.io/guide/boot/Shell) 部署。

NapCat WebUI 登录后，新建并启用 **OneBot V11 正向 WebSocket 服务端**：监听 127.0.0.1:3001，消息格式 array，reportSelfMessage=false，使用全双工 API + 事件连接，设置 OneBot token。这个 token 与 WebUI 登录密码不同。字段见 [官方配置说明](https://napneko.github.io/config/basic)。

远端 NapCat 可用 SSH 转发到 Mac（替换用户和地址，保持此终端运行）：

```sh
ssh -N -L 127.0.0.1:3001:127.0.0.1:3001 用户@服务器
```

适配器连接 ws://127.0.0.1:3001/；不要把 token 放到 URL 查询参数。容器内 127.0.0.1 与宿主机不同，容器部署需先把端口发布到宿主机回环地址。

## 2. 模型

准备兼容 Chat Completions 的完整 HTTPS URL（通常以 /v1/chat/completions 结尾）、上游模型名和 API Key。当前仅支持 system/user/assistant 纯文本非流式调用；插件 default 别名映射到配置模型。Core 必须能直接连接，当前 Provider 禁用环境代理；本机模型可用 http://127.0.0.1:端口/...。密钥只配置给 Core，不传给适配器或插件。

## 3. 本地依赖与私有配置

KiaraBot 根目录执行：

```sh
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON
cmake --build --preset proto --parallel 4
uv sync --project sdk/python --locked --python 3.11.18
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
mkdir -p data/t07b
cp tests/acceptance/core.env.example data/t07b/core.env
cp tests/acceptance/adapter.env.example data/t07b/adapter.env
chmod 600 data/t07b/core.env data/t07b/adapter.env
```

替换两个文件中的全部 REPLACE_ 占位符。可用 `openssl rand -hex 32` 在本机生成随机 token：Core adapter token 与适配器 CORE_AUTH_TOKEN 相同；plugin token 使用另一个随机值；ONEBOT_ACCESS_TOKEN 填 NapCat OneBot token。data/ 已被 Git 忽略，密钥不要粘贴到聊天。

独立适配器目录（与 KiaraBot 同级）执行：

```sh
uv sync --locked --python 3.11.18
.venv/bin/python tools/generate_proto.py ../KiaraBot/proto
```

若 uv 不在 PATH，可使用本机已有 KiaraBot/.tools/uv-aarch64-apple-darwin/uv。两个项目的 .venv、生成目录和锁文件保持独立。

## 4. 启动

终端 A 在 KiaraBot 根目录，只加载 Core 配置：

```sh
set -a
source data/t07b/core.env
set +a
build/proto/core/kiarabot-core --grpc-listen 127.0.0.1:50051
```

终端 B 在独立适配器根目录，只加载适配器配置：

```sh
set -a
source ../KiaraBot/data/t07b/adapter.env
set +a
PYTHONPATH=generated:src .venv/bin/python -m kiarabot_adapter_nonebot
```

模板先启动 event-observer，只接收消息，不发送回复或调用模型。Core 应显示 adapter_ingress_ready=true、plugin_delivery_ready=true、operation=plugin_start outcome=registered。rpc_ready=false 仍是健康/部署任务未完成的标记，不能用它认定本条接收链路失败，也不能仅凭没有报错认定联调通过。

准备完成后提供：两个本机配置文件路径、机器人 QQ 号、目标会话类型（private/group）和 ID、NapCat/QQ 版本、可用于验证的测试消息。不要发送任何 token 或 API Key。随后分别做真实接收、受控发送、模型、断线及备份恢复验收，记录实际结果；目前不将这些项目标为通过。

## 5. 停止与备份

分别 Ctrl+C 停止适配器和 Core，再停止 SSH 隧道。保留 data/t07b/core.db、adapter-outbox.db 和 adapter-outbox.db.actions。全部停止后再复制整个数据目录到本机受保护位置，不上传 env 文件。schema v5 不能由旧二进制直接读取，回滚使用匹配二进制与迁移前备份；不要删除未知结果的 Action 日志再重发。真实回滚演练仍待验收。
