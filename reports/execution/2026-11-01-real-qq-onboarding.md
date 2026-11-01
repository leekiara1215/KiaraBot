# 真实QQ接入：本机收发已通过

用户授权本机安装配置NapCat，其他功能暂停；到扫码时通知用户。后续补充要求程序内方便
执行外部安装/配置并保持协议端可替换。架构方向记录在doc26，初版为建议，随后用户要求
写入文档并推送而确认；未修改doc18，确认不等于通用API/向导已实现。
本轮独立开发分支为`codex/real-qq-onboarding`；运行数据不在Git中，换机仍需重建环境
及在目标机器重新登录，不能仅复制此分支就继承本机QQ会话。

## 本轮范围与实际行为

实现`scripts/local/napcat.py`单机部署驱动及直接测试；输入为私有状态目录、本机容器工具，
connect还必须有用户明确指定的测试会话。输出分开报告容器、WebUI、OneBot认证和QQ在线。
setup只能到awaiting_user_login，不等于平台收发成功；失败非零且仅输出稳定本地原因。
未修改Core/Adapter/SDK/Panel业务、proto、API或数据库schema。

本机Homebrew安装Colima0.10.3、Lima2.2.1、Docker CLI29.9.2、Compose5.6.1。
只创建专用Colima profile kiarabot：vz/aarch64、2CPU/4GiB、20GiB数据盘、不切换用户
默认Docker context、不转发SSH agent，仅挂专用NapCat container数据目录。
VM内Docker server29.5.2；修复其首次启动的悬空resolv.conf问题后下载官方文档指定镜像。
DNS证据与具体修复范围见doc26，未改宿主机网络、未关闭TLS验证；供应商Bug自动修复仍未实现。

实际镜像为Linux arm64：
`mlikiowa/napcat-docker@sha256:696e9574a0f6fb2f86cbecba8a32048f2a5823117dc9b13c6c02637ad890fa8b`。
首次解析后锁到私有image-lock.json，重跑复用，不以latest静默升级。
容器标签与状态目录owner绑定，非privileged，6099/3001只发布到127.0.0.1；仅挂专用
config、qq和plugins三目录，不挂宿主机HOME或Docker socket到容器。不修改宿主机QQ.app。

OneBot和WebUI各自随机凭据，生成文件0600/目录0700；命令参数只含文件路径，
没有秘密明文。既有配置不覆盖，运行中setup不重置密码/QQ登录态；只停止owner标签
匹配的容器，不清理数据或杀其他占端口程序。同一状态目录操作采用flock互斥。

## 实际检查

- `python3 scripts/local/napcat.py setup`退出0，WebUI可访问，返回awaiting_user_login。
- 重复setup退出0：前后密码、OneBot token、image-lock、OneBot/WebUI配置SHA256均未变。
- 实际Docker inspect只投影状态、镜像、端口、mount类型和privileged布尔；确认上述隔离。
- `python3 scripts/local/napcat.py status`扫码前退出1，容器/WebUI true、OneBot false、QQ在线
  null/account null，与尚未扫码一致；不把未知在线状态当作已登录。
- 实际浏览器认证登录WebUI，选择“扫码登录”，看到真实QQ二维码；已通知用户并保留页面。
  未代替用户扫码/确认设备，未提交QQ密码；等待用户明确测试目标后才启用回复。
- 用户扫码完成并指定测试群；status退出0，OneBot认证true、get_status.online true。
  以connect --ui --ui-port 5174启动独立data/real-qq，未终止5173的未知占用者。
  实际Control健康投影显示storage readable、adapter connected、local-reply running；
  live浏览器成功登录并显示实时连接正常和运行中处理器。NapCat4.18.42、QQ3.2.34-53644。
- 用户发送测试消息并确认群内已收到“收到：hello”。只读核对Adapter执行日志：两条
  不同Action均SUCCEEDED/terminal、有平台message_id、无Error；两条均对应不同原始
  ingress event ID，向用户指定会话发送。outbox三条acked，含状态事件，无pending/failed。
  没有用直接OneBot发送绕过Core/SDK/Adapter验收；未输出消息正文、对方身份或完整payload。
- `adapters/nonebot/.venv/bin/python -m pytest -q scripts/local/tests/test_napcat.py scripts/local/tests/test_launcher.py -o asyncio_default_fixture_loop_scope=function`：8/8通过0.24秒。
  新驱动5项覆盖配置保留/权限、symlink拒绝、外来容器及非loopback拒绝、平台离线/未知不启动Core。
- Python py_compile、模块依赖边界、文档链接/UTF8/换行、`git diff --check`通过；doc18未变。

未重复C++构建/格式/tidy或整仓测试：无C++变更，定向脚本与实际容器检查已执行。
真实QQ群聊固定回复、OneBot上线和Adapter连接已验证；真实私聊、断线恢复、容器和业务
重启尚未验证。首轮验收结束时保留运行，随后按用户明确要求停止，结果见下节。
Linux/新电脑、完整通用接入API/Panel向导未验证或未实现；不提升目标六服务完成度。

### 后续：按用户要求停止运行并确认方向

用户随后要求“关闭运行，将上述设计方向写入文档推送到git”。已确认doc26中的统一
接入向导、可替换驱动、托管/已有服务两种模式及分层职责；API/页面仍未实现。
这项确认不修改受保护doc18，也不把方向确认计作实现验收。

实际停止：`start.py stop --state-dir data/real-qq`退出0、run.json为stopped/exit_code0；
逐一核对原记录Core/Adapter/Panel的进程身份均不再存活。`napcat.py stop`退出0且
data_preserved true，Docker无运行容器；随后`colima stop kiarabot`退出0，list显示Stopped。
另核对5173监听进程的UID、完整命令和工作目录，确认为同一仓库的独立Vite后发送SIGTERM，
有界等待其退出，没有按端口号杀未知进程。5173/5174/6099/3001均无监听。
runtime.db/outbox/.actions、管理员和平台凭据、QQ登录态目录均仍存在。未测试重新上线，
不把本次停止扩写成“重启恢复已验收”。

本次仅文档状态/方向同步和运行停止；文档影响范围为doc08、23、25、26及本报告。
doc26补充确认记录、与首版的关系及后续分步验收，计划/交接同步引用。无代码、协议/API
或现行业务状态机修改，无需改proto/API及其他模块文档；无C++变更，不重跑构建/格式/tidy。
实际校验为文档相对链接、UTF8/末尾换行/冲突标记、diff空白及doc18未改，结果随交付核对。

## 文档影响与继续步骤

doc08同步新运维CLI、权限、启停和状态说明；doc24/25及doc23标明用户最新优先级；
doc26记录原计划边界、可替换方案、实际环境及接续点。本报告保存真实执行证据。
没有更改proto/API或Core/Adapter公共语义，无需同步其契约及其他业务模块文档。
受保护doc18保持原文；通用接入管理方向随后已确认，正式服务/API契约仍待定义和实现。

本机已经执行的最小接入步骤及后续验收边界：

1. `napcat.py status`确认经认证OneBot返回实际账号且get_status.online明确true。
2. 保留其他实例数据，按需停止占用Panel端口的本程序实例；执行
   `napcat.py connect --conversation <用户指定数字ID> --ui`，使用独立data/real-qq。
3. 确认真实Adapter connected、公开SDK处理器running；用户在指定会话发消息，核对
   真实QQ收到“收到：原文”、Core Action终态及Adapter可靠日志。不要用直接平台发送绕过Core验收。
4. 后续再测试停止/重连和同目录重启，保留配置/登录态，补记结果及尚未覆盖项；
   当前不将这些故障恢复测试标为完成。本次实例配置只允许用户指定测试群。

扫码后如仍有设备验证或人工确认，交给用户完成；不能以模拟平台或伪造在线状态替代。
