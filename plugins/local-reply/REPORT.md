# MVP local-reply 实施与检查

目标及允许范围仅本插件目录与其 README/report；基点干净 root da3b2bb，独立分支
codex/mvp-local-reply。已读 AGENTS、C++主规范、模块边界、doc/03与现SDK/协议/示例。
无 SDK/C++/P04/launcher/CMake/协议改动。文档影响限本目录 README：新固定处理器行为、
输入输出、期限/幂等/错误和既有managed启动参数；共享 SDK/运维契约无需变更，未改doc18。

输入生成 EventEnvelope.message_received，群/私聊；输出既有SendMessage纯文本Action。
稳定公开UUID5 ID只使用插件/adapter/account/event身份，不含trace或可变时间；deadline从
原occurred_at精确纳秒+60秒。先查原key，accepted/全部terminal不重复创建；仅NotFound
submit，Unknown/依赖失败上抛，不换key/内部盲重试。自身/空/非消息/过期忽略，查询中
跨过deadline也不发。UTF8截断保留合法字符、总4096bytes，不解释CQ、不记录正文/token。

实际命令（只读复用固定工具链venv/generated协议）：

```sh
PYTHONPATH=/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-operations/sdk/python/src:/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain/sdk/python/build/generated \
 /Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain/sdk/python/.venv/bin/python \
 -m pytest -q -c sdk/python/pyproject.toml plugins/local-reply/tests
```

15passed。覆盖两种conversation exactaction、纳秒deadline、同源重放/trace改变/新实例稳定ID、
accepted/5种状态不重复发、自身/空/非消息/过期、query延迟过期、query错误/submit未知向上传播、
未知query状态失败、长Unicode/CQ保持纯文本。使用SDK公开生成类型与context fixture，
不是真实Core持久化或QQ副作用验收。另用现SDK load_manifest验证id/main:create/ordered/
最小permissions并实际构造LocalReply成功。git diff --check通过，未提交生成物/凭据。

未运行真实 reference Core+SDK+OneBot协议替身闭环：父/CI作者负责合验，不把本单测声称
端到端已通过。没有真实QQ/付费Provider/外部部署。后续验收应观察一条入站产生一次准确
出站且重复投递不重复submit；账号身份和会话白名单由现Core负责。
