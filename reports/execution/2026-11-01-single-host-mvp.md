# 单机可运行版与换机交接验收

2026-11-01晚间，按用户“先最快跑通流程，其余后补”的最新优先级实施。
本轮没有完成或宣称完成六服务迁移；复用隔离reference路径作为单机临时交付轨。
真实QQ/NapCat、付费模型、多主机和生产发布均未执行。

## 独立提交与范围

| 能力 | 作者原提交 | 主集成提交 |
| --- | --- | --- |
| 公开固定SDK回复处理器 | adc0d06 | 762d6e1 |
| 交互式loopback OneBot替身 | 4990932 | 407dd8c |
| 实际NoneBot消息闭环及公开factory | 67f91e5、3c1da07 | c37df48、994a8b4 |
| 受管单机启动器、所有权检查 | 3a48f85 | 5ac73c2 |
| 当前reference群/私聊证据更正 | 0aff49f | 3d731c7 |
| 浏览器live fetch上下文修复 | 主协调独立实现 | a34b040 |

目标模块：`plugins/local-reply/`只经SDK回复；`scripts/local/`只负责演示协议与进程管理；
`panel/src/{main.tsx,api/client.ts}`绑定原生fetch，修复实际浏览器登录的Illegal invocation。
未修改本轮C++、proto/API契约、PG schema或冻结架构基线。之前已开始的P04构建收口
另见[冻结报告](2026-11-01-p04-freeze-and-mvp.md)，不计作本轮MVP业务完成。

输入为一个已配置账号、明确的会话白名单和纯文本事件；输出为实际平台send调用与
“收到：原文”。处理器使用稳定Action身份、先查询幂等状态；不访问OneBot或数据库。
启动器默认只监听loopback，自动生成并保留私有配置、认证秘密与SQLite/outbox。
配置/依赖失败非零，子进程死亡退出，疑似他人PID拒绝接管；不重置数据、不杀占端口程序。
既有认证/allowlist/配额保留，未新增线协议或客户端错误枚举。

## 主集成实际检查

1. 先前收口的`full-locked`完整configure/build通过，匹配源码的
   `build/full-locked/reference/core/kiarabot-core`及
   `reference/control/api/kiarabot-control-credentials`实际参与本轮运行。
   Adapter重新`uv sync --locked --python 3.11.18`，两处Python协议生成成功；未升级锁。
2. 最终定向Python测试 **24/24通过，3.15秒**：

   ```bash
   PYTHONPATH="$PWD/sdk/python/src:$PWD/sdk/python/build/generated:$PWD/adapters/nonebot/src:$PWD/adapters/nonebot/generated" \
   CORE_TEST_BINARY="$PWD/build/full-locked/reference/core/kiarabot-core" \
   SDK_SOURCE="$PWD/sdk/python/src" \
   adapters/nonebot/.venv/bin/python -m pytest -q \
     adapters/nonebot/tests/test_mvp_local_smoke.py \
     adapters/nonebot/tests/test_action_pipeline.py \
     adapters/nonebot/tests/test_nonebot_process.py \
     scripts/local/tests/test_onebot_demo.py \
     scripts/local/tests/test_launcher.py \
     plugins/local-reply/tests/test_local_reply.py \
     -o asyncio_default_fixture_loop_scope=function
   ```

   覆盖公开处理器、重复消息、真实NoneBot/Core/SDK/WS收发、平台协议拒绝以及启动器
   symlink/不安全文件权限/伪造外来PID拒绝；测试不替代下面的交互启动验收。
3. `python3 scripts/local/start.py start --state-dir data/mvp-review`，显式传入上述
   reference二进制、credentials和Adapter解释器，并加`--ui`，成功启动：
   demo、Core/Control、Core受管SDK处理器、真实NoneBot及Vite live页面。
   demo实际WS connected后才返回running。
4. 实际交互群聊456/user789的hello返回`send_group_msg`/`收到：hello`；随后同号
   私聊user456的private hello返回`send_private_msg`/`收到：private hello`。
   未修改allowlist或配额；独立CI A/B将此前私聊失败定位到旧Oct31构建的入站CONFLICT，
   当前reference重建后通过，未以放宽生产校验消除失败。
5. 真实HTTP错误密码401、正确登录200；真实`/ws/control`首帧`SESSION_READY`之后，
   `/auth/me`、`/health`、`/adapters`、`/plugins`均200。首个手写探针误用Principal的
   username字段导致检查脚本KeyError，改为契约user_id后完整通过，未改服务端字段。
6. 实际浏览器先发现native fetch的Illegal invocation，修复绑定上下文后重新登录，
   页面显示“实时连接正常”、storage readable、adapter connected、local-reply running；
   无MOCK标识。截图保存在ignored验收目录，含运行界面不含密码。
7. 相同参数的重复start返回同一PID；stop确认已记录孩子全部退出且demo.sock移除；
   重复stop成功。再start的PID变化，密码/token/control.json摘要及SQLite/outbox inode
   保持，最后stop正常，停止后status退出1。检查process.log不含生成密码、各token、
   加密密钥和实际登录access/refresh token。验收进程最终已停止，数据保留。
8. `npm --prefix panel run lint`、`run check:contracts`、`run build`通过；
   `npm --prefix panel test -- --run tests/integration.test.tsx` **17/17通过**。
   契约检查确认23个端点与5个已实现事件。模块边界检查、文档相对链接/UTF8/冲突标记、
   `git diff --check`通过；doc18对初始基点保持不变。

未再次执行整仓统一toolchain/tidy/全部CTest：本轮新增Python脚本和局部前端修复，
用户已明确收缩至最小闭环，相关测试/构建已执行。未作Linux/新电脑/远端CI、Docker、
真实QQ/NapCat/模型或生产环境验收；新机必须重建并重跑doc24，不复制旧build/venv。

## 文档影响与接续

实际同步：README与doc索引提供优先运行入口；doc07记录live客户端修复及启动方式；
doc08记录本地启停/安全配置/状态边界；doc23调整近期优先级并保留原计划；
doc24提供已验证准确命令；doc25记录P04 WIP、P05–P14后移项、分支/SHA与换机恢复；
平台替身README更正token/私有数据保留行为。本报告保存实际验收与限制。

无需更新proto/、OpenAPI/AsyncAPI、doc01/02/03/04/05：本轮复用既有公共通道与业务语义，
未更改这些模块契约、状态或所有权；不存在“临时更新协议绕过未实现新Runtime”的接线。
doc18为受保护只读文档，未改动；与新方向的迁移未完成差异在doc24/25明确列出。

跨机交接采用仓库外bundle保存全部已提交refs，另备份工具链worktree未提交patch与两个
untracked脚本。bundle不含数据库、密钥、build、venv或其他ignored文件；源码和测试状态
与本机运行数据分开。最终HEAD、分支、hash和恢复入口写入同目录清单，未push或上传。
