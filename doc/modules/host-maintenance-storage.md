# Host 维护存储（HOST-02）

日期：2026-11-04。实现切片，不替代proto/API契约，不宣称RPC或真实Runtime gate完成。

存储属于Host私有SQLite：本地admission/版本/活动operation、不可变命令身份与幂等键、维护operation及
逐执行快照/完成证据、审计、统一operation changes。纯端口不含SQL/JSON/wire；SQLite只实现Host用例
事务，不决定管理权限。认证后的Control人类主体每次通过Host授权端口核验精确host.read/execute。

每个提交原子保存operation与Host状态、审计和commit-ordered changes。SQLite写事务先取得写锁再分配
序号；回滚不产生通知。重放原幂等身份返回原operation，不分配新操作、不再次append伪通知；同键不同
意图CONFLICT。原提交响应丢失沿原键/operation查询；已记录阶段先于进程/gate副作用，崩溃恢复不猜测
完成。只有全部目标都KnownStopped才成功，未确认或缺失进程证据保持Unknown/Blocked。

统一OperationObservationPort的Host scope严格为自身单RESOURCE_HOST；无cursor只返回持久head。每页
重新授权，读取为独立短事务，不夹业务写/远端调用。日志最多65536条/64MiB、600秒，容量/保留淘汰推进
持久low-water；旧epoch/未来sequence/低于low-water返回Conflict/ReadCurrent，由边界映射RESYNC_SNAPSHOT。
普通重启保持epoch，可信离线重建可显式轮换epoch；不复活旧cursor。日志仅保存Operation安全投影，不含
PID、marker、配置、凭据或模型/消息正文；执行细节只留Host私有维护快照。

Begin：Accepted→LocalClosed→RuntimeClosed→Snapshot→逐执行Drain→Succeeded。每次最多停止一个目标，
没有数据库事务跨越端口调用。Cancel只允许Accepted，之后Conflict。Resume：Accepted→不可取消阶段→
确认RuntimeOpen→本地Open+Succeeded；任一Unknown/依赖失败保持Closed/Blocked。Restore不恢复旧任务/
凭据/租约；不存在“把未知执行状态清空后恢复”的捷径。既有HostService排空必须传播逐执行失败，而不是
仅凭命令receipt完成标记推断已停止。

进程终止证据：只有 supervisor 对本组合根 spawn 的原 owned child 取得真实 waitpid 退出事实（grace 内
或 KILL 后）才把执行记为 Exited+Known*；身份/marker 不符、未登记 pid、目标仍存活、signal 被拒、proc
信息失败或无法 reap 一律保持 Unknown/Blocked，不能由 alive=false 或 EPERM/ESRCH 推断已停止。重复停止
一个已由 waitpid 确认退出的执行是非 forced 成功，不产生新信号副作用。
