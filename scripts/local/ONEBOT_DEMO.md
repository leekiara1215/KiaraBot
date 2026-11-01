# 本地 OneBot V11 演示替身

此工具让真正的 NoneBot OneBot adapter 连接本机正向 WebSocket，无需 QQ、NapCat、模型
或付费 Provider。它只模拟平台协议，不处理业务、不访问 Core/PG、不自行生成回复。
完整启动、登录、收消息→Core处理→回复由本地 MVP launcher/集成测试组合；此脚本不宣称
真实平台可靠性、持久化或幂等保证。仅支持 macOS/Linux Unix sockets。

使用 adapters/nonebot 已锁定环境的 Python；不新增或升级依赖：

```sh
uv sync --frozen --project adapters/nonebot
PYTHON=adapters/nonebot/.venv/bin/python
DEMO_DIR=$(mktemp -d /tmp/kb-demo-XXXXXX)
chmod 700 "$DEMO_DIR"
# launcher应生成同等随机文件；秘密不放argv、日志或仓库。
"$PYTHON" -c 'import os,secrets,sys; fd=os.open(sys.argv[1],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600); os.write(fd,(secrets.token_hex(32)+"\n").encode()); os.close(fd)' "$DEMO_DIR/token"
"$PYTHON" scripts/local/onebot_demo.py serve --token-file "$DEMO_DIR/token" \
  --control-socket "$DEMO_DIR/demo.sock" --port 0 --account-id 123
```

成功启动只输出一行无秘密 JSON，实际端口由操作系统选择：

```json
{"event":"ready","host":"127.0.0.1","port":49152,"account_id":"123","control_socket":"/tmp/kb-demo-example/demo.sock"}
```

launcher 将 `ws://127.0.0.1:PORT` 配到真实 adapter 的 `ONEBOT_WS_URL`，将同文件内容在
adapter 进程内部作为 `ONEBOT_ACCESS_TOKEN`。替身要求握手 `Authorization: Bearer TOKEN`；
token 文件必须本用户拥有、regular、非symlink、权限恰好0600，内容32–128个ASCII字母/
数字/下划线/横线，可有一个末尾换行。禁止明文token参数或输出token内容。bot account
必须正整数且小于2^63；默认123，须与adapter的ACCOUNT_ID一致。

控制socket父目录须本用户拥有、权限0700，socket为0600；拒绝覆盖已有path。macOS限制
路径长度，因此绝对路径UTF8字节数须≤100，推荐短的 `/tmp/kb-demo-.../demo.sock`。
WS固定仅监听127.0.0.1，不提供外部部署、Core管理或真实账号入口。

另一终端用同一环境和目录注入消息并等回复：

```sh
"$PYTHON" scripts/local/onebot_demo.py status --token-file "$DEMO_DIR/token" --control-socket "$DEMO_DIR/demo.sock"
"$PYTHON" scripts/local/onebot_demo.py message --token-file "$DEMO_DIR/token" --control-socket "$DEMO_DIR/demo.sock" --type group --user-id 789 --group-id 456 --text '你好' --timeout 10
"$PYTHON" scripts/local/onebot_demo.py message --token-file "$DEMO_DIR/token" --control-socket "$DEMO_DIR/demo.sock" --type private --user-id 789 --text '你好'
```

message默认group/user789/group456/timeout10秒，text必填；ID须正整数、user不得为bot。
server将请求转换成完整原始OneBot message event（self_id、time、message_id、sender、
raw_message、text segments和相应group/user）。一次只允许一个WS客户端及一个在途注入。
匹配会话的send API到达后，CLI输出 `{"ok":true,"event_message_id":1,"reply":{"action":
"send_group_msg","params":{"group_id":456,"message":[...]},"message_id":100001}}`，退出0。
服务仅保存最近16条回复供status显示；消息和回复仅存内存。没有原始事件到send API的
可靠关联ID；超时后的延迟回复不能当真实平台投递保证，必要时重启本地演示再试。

## 精确接口与拒绝语义

控制IPC是单连接一条UTF8 JSON行，≤8192字节；需token字段，重复key、NaN/Infinity拒绝。
status请求为 `{"command":"status","token":"..."}`，返回ok、connected、account_id和replies。
message请求为 `{"command":"message","token":"...","message_type":"group|private",
"user_id":789,"group_id":456,"text":"hello","timeout":10}`。private可省group_id；原始
请求只接受上述字段。text非空、无NUL、≤2048 UTF8字节；timeout为有限(0,30]秒。
请求读取3秒上限，最多8个控制连接；控制响应≤128KiB。

失败输出 `{"ok":false,"error":"CODE"}`，CLI退出1。主要稳定码：unauthorized、
invalid_request、invalid_id、invalid_user、invalid_message_type、invalid_message、
invalid_timeout、not_connected、busy、reply_timeout、platform_disconnected、
unsafe_token_file、invalid_token_file、unsafe_control_socket、control_path_too_long、
local_demo_unavailable。错误不包含原始body/token/异常详情。status的connected仅代表本机
WS已连接，不代表Core健康或业务可用。

OneBot只支持get_login_info/get_version_info/get_status（空params）及send_group_msg/
send_private_msg（正target ID、纯文本string或≤16个text segments，合计≤4096 UTF8字节）。
成功回status=ok/retcode=0/data/echo；未知API回failed/1404/unsupported_action，错误params/
消息回failed/1400/稳定message，保留echo。不实现send_msg或其他平台动作、不丢弃未知段。
WS帧≤16KiB；错误token在握手阶段HTTP401，重复连接409。SIGTERM/SIGINT退出关闭自身
WS/IPC并移除自己创建的socket；本机launcher保留token和私有数据目录供同目录重启。
手动独立运行时由调用者管理token和目录，不自动删除已有数据。

## 验证与变更范围

```sh
adapters/nonebot/.venv/bin/python -m pytest -q scripts/local/tests/test_onebot_demo.py \
  -o asyncio_mode=auto -o asyncio_default_fixture_loop_scope=function
```

测试实际启动脚本子进程及真实NoneBot driver/OneBot adapter，分别注入群/私聊并通过真实
WS send_group_msg/send_private_msg回读；测试专用matcher只用于本工具的传输验证，不是
MVP业务处理、FakeCore或真实QQ。另覆盖token拒绝、未知API、坏ID、超时、过大/重复key
控制请求、文件权限、退出清理及秘密不出现在输出。CI作者负责真实仓库整链路验收。

文档影响：本README是新本机演示工具的直接运行说明；按任务窄范围只改脚本/直接测试/
本README，未改doc08等共享文档。没有修改生产Core/adapter行为、proto/API契约、PG/schema、
锁文件或受保护doc18；不代表P04安全管理用例完成。没有C++改动，无需C++构建/格式检查。

本次实际检查：上述pytest命令3/3通过（0.50秒）；Python AST语法、UTF8/末尾换行及
`git diff --check`通过。未在此子任务运行实际Core/面板整链路，因由launcher/CI作者组合
验收；本脚本的真实NoneBot双向WS测试已执行，不把它称作整个MVP完成。
