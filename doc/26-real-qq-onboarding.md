# 单机真实 QQ 接入

2026-11-01用户要求：暂停其他功能，优先安装配置NapCat并跑通真实QQ；到扫码登录时通知用户。

## 原计划与本次范围

现有方向/实施计划没有承诺KiaraBot面板内置NapCat一键安装器；原基线将NapCat定义为
外部独立安装运行的协议端，运维Compose也不管理它。此处新增的是当前用户授权的单机
接入运维入口，不改变Core/Adapter边界，不修改人类维护的doc18。

本轮目标：依赖检查/安装、隔离启动NapCat、生成私有OneBot配置和token、等待用户扫码、
发现实际登录账号、连接既有NoneBot/reference Core，再在用户指定的会话验证实际收发。
扫码及可能的设备验证由用户完成；未获得测试目标前不向任意QQ/群主动发送。
完整账号管理、插件机制、多主机及模型功能暂停。

## 本机方式

macOS Apple Silicon采用独立Colima Docker profile，NapCat Linux arm64镜像；
Core/NoneBot/Panel仍在宿主机运行。容器仅向宿主机loopback发布6099 WebUI和3001正向WS。
不修改宿主机QQ.app，不使用整套Linux host网络Compose，不改Core监听地址。
只挂载专用私有NapCat数据目录，不挂载用户HOME或Docker socket到容器。

镜像从NapCat官方文档指定的`mlikiowa/napcat-docker`取得，首次解析后记录实际digest；
重跑复用已选digest，不自动升级。WebUI与OneBot各自随机凭据，文件在ignored的data目录，
配置/秘密0600、父目录0700；日志不入库、不输出token、消息正文或完整平台异常。
重启保留QQ登录态和配置；停止仅操作本入口拥有的容器/进程，不删除数据。

## 程序内接入管理：建议方案，尚未确认/实现

用户补充希望在程序内方便执行外部安装/配置，又不绑定NapCat。建议采用统一接入向导、
受控的协议端驱动及两种管理模式；本节是待确认设计，不改写已确认基线或现行契约。

| 层次 | 示例 | 所属责任 |
| --- | --- | --- |
| 渠道 | QQ | 用户要接入的平台，不等于具体软件 |
| 平台协议 | OneBot V11 | 外部软件与Adapter间的协议；新协议需独立适配 |
| 协议端实现 | NapCat或其他兼容实现 | 安装、配置、登录和本机服务生命周期；可替换驱动 |
| Adapter实现 | 当前NoneBot/OneBot V11适配器 | 统一事件/Action转换、可靠日志与平台连接 |
| 管理模式 | 本程序托管／连接已有服务 | 前者可安装启停，后者只配置连接/检查，不接管外部进程 |
| 业务账号与绑定 | Runtime账号ID、平台QQ号、Adapter绑定 | 仍归Runtime，登录材料不放入Runtime权威业务表 |

面板向导 → Control接入编排 → 本机受控执行端口 → 具体协议端驱动。
登录后再通过Runtime已有所有者接口登记账号/绑定，Adapter继续经公共协议接入Runtime。
Control只持有安装操作/进度/脱敏技术配置，不能成为可写的第二套账号目录。
执行层先是单机Control外层/运维脚本，不新增第七种平台服务，也不复用业务插件Host
承担任意系统软件安装。多主机执行属于后续独立设计，不能将SSH shell透传面板。

驱动向Control提供能力说明、配置schema、环境检测、安装/启动/停止、交互挑战及登录/
连通性检查；均为受控类型和有限操作，不让客户端上传任意命令、脚本或镜像运行参数。
第一版用显式注册的内置驱动，不建设动态驱动下载、插件市场或自定义任意脚本机制。
第一驱动为NapCat，另提供不安装软件的已有OneBot服务连接模式。兼容OneBot V11的
替代端可复用Adapter；不兼容协议须新增Adapter，不能仅改provider名称伪装成兼容。

向导顺序：选择渠道/实现和托管模式 → 检测条件 → 安装/导入 → 生成配置与秘密引用 →
启动 → 用户扫码/其他登录交互 → 检测平台在线 → 建立账号/Adapter绑定 → 指定会话验收。
首版使用独立向导，不要求用户手填WS URL/Token；高级模式仍允许显式已有服务配置。
二维码只是可选交互类型，未来驱动也可要求device-code、授权跳转或已有token。
二维码/会话挑战有期限，不入日志/长期存储；秘密仅由所属执行边界持有，面板只获一次
必要交互内容及脱敏配置，普通账号读取权限不能获得系统安装执行能力。

安装完成、进程启动、平台已登录、OneBot可用、Adapter已注册、真实收发成功分别记录
观察与时间，不用一个running/healthy把它们合并。操作必须可重入、可取消、有界超时、
保留准确失败步骤；外部安装不纳入Runtime数据库事务。按operation记录已完成步骤，
断线重查原操作，不重新安装/重置密钥；失败只清理本次拥有的临时资源，不删除登录态
或用户已有部署。需要正式API/错误/权限/进度事件时，先更新OpenAPI/AsyncAPI及模块文档。

本轮先完成NapCat真实链路及可重复执行脚本；下一步再将相同能力接入向导，避免为通用
框架推迟真实QQ验收。不会把当前单机脚本或登录页宣称为已经交付程序内安装向导。

官方来源：

- [NapCat安装](https://napneko.github.io/guide/boot/Shell)
- [NapCat OneBot配置](https://napneko.github.io/config/basic)
- [官方Docker入口](https://github.com/NapNeko/NapCat-Docker)
- [Colima安装](https://colima.run/docs/getting-started/)

## 当前验收状态

本机最初未安装Docker/Colima，现已通过Homebrew安装Colima0.10.3、Docker CLI29.9.2、
Lima2.2.1与Compose5.6.1；专用`kiarabot` profile已运行，Docker server29.5.2。
镜像Linux arm64压缩层合计约570MiB，已下载并按digest启动：
`mlikiowa/napcat-docker@sha256:696e9574a0f6fb2f86cbecba8a32048f2a5823117dc9b13c6c02637ad890fa8b`。
私有WebUI和OneBot配置已生成，WebUI已认证登录并实际显示QQ扫码页。
随后用户扫码登录成功，并明确授权测试群。真实QQ入站与固定回复已通过，用户已确认收到。
NapCat版本4.18.42、QQ版本3.2.34-53644。适配器执行日志有两条不同来源事件对应的
SUCCEEDED终态及平台消息ID，入站outbox三条均acked（含状态事件），没有pending/failed。
敏感账号、测试会话及登录材料只保存在本机私有配置，不提交到运行模板。
断线恢复、NapCat重启后的登录态恢复仍未验收；真实收发通过不等于完整故障矩阵通过。

首次VM启动遇到[Colima0.10.3已知DNS问题](https://github.com/abiosoft/colima/issues/1611)：
guest的`/etc/resolv.conf`是指向不存在目标的symlink，dnsmasq未启动。只在本次专用VM内
移除该悬空symlink，使用guest DHCP租约提供的DNS192.168.5.2创建resolv.conf后，解析与
实际docker pull恢复；未改宿主机DNS，也未关闭TLS/证书校验。新机如重现，先检查对应
profile和guest DHCP租约，不能硬抄此地址到其他环境。此供应商问题尚未在驱动中自动修复，
也未声称已完成所有机器的零操作安装；安装向导的环境检测应将它列为明确失败步骤。

当前`scripts/local/napcat.py`为首个独立部署驱动，提供setup/status/connect/stop，
不等于已实现通用驱动API或Panel向导。定向测试5/5、既有启动器所有权3/3通过；
覆盖配置/token保留、权限、symlink拒绝、外来容器/非loopback拒绝及平台离线/未知不启动业务。
Python编译和边界检查通过，无C++变更。实际setup先返回awaiting_user_login；扫码后
OneBot认证及get_status.online均true，Core/Adapter/local-reply健康。容器非privileged，仅
挂专用config/qq/plugins三目录、6099/3001仅绑定宿主机127.0.0.1；未安装到宿主机QQ。

本机控制台本次使用`http://127.0.0.1:5174`，因为5173被其他进程占用，未终止未知进程。
管理员密码见`data/real-qq/admin-password`；NapCat WebUI为`http://127.0.0.1:6099/webui/`，
密码见`data/napcat-qq/webui-password`，均不发到聊天。真实链路当前保持运行，仅允许用户
指定测试会话回复；变更会话需先停止该实例并明确重新配置，不默认扩大范围。
准确执行证据见[本轮记录](../reports/execution/2026-11-01-real-qq-onboarding.md)。

## 任务与文档影响

目标模块与允许修改范围：`scripts/local/`接入运维入口及相关验证、
`doc/08-operations.md`、本页、doc24/25及直接执行报告；必要的Adapter连接问题仅在
`adapters/nonebot/`按其公共契约修复。输入为本机容器环境、已确认登录和明确测试会话；
输出分开报告NapCat进程存活、QQ登录、OneBot连接、实际业务收发，不能用一个healthy替代。
依赖缺失、端口冲突、身份不符、认证失败、未登录或超时明确失败，不重置状态掩盖问题。

验证：Python语法/相关脚本测试、Docker实际配置/端口/digest检查、真实OneBot认证与
只读账号探测、用户指定会话的端到端收发及停止/重连。无C++变更则不重复C++格式或整仓
构建；若发生变更再执行相关构建/测试。proto/API及其模块文档只有契约发生可观察变化时
同步；本机运维接线复用既有契约，无需为安装NapCat修改它们。doc18保持只读。
