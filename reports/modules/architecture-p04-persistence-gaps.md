# P04 后续 Identity 持久端口只读核对

本报告只读核对已提交的协议、设计、Core端口和PG实现，不冻结接口，不授权实现方案。
正常App/crypto/PG fixture通过不等于P04阶段完成。检查基点为本工作树ee4395b；未读取或
复制Credential作者未提交源。父任务随后确认唯一契约、版本和错误语义后再实现PG。

## 范围、证据与文档影响

只新增本报告；不改Core/proto/API/生产源/PG schema/CMake/config/main或doc18。
没有生产行为变动，doc05/doc08和契约文档此时无需同步；本文的接口名均为待冻结草案。
若采纳其中schema/公共行为变动，后续实现必须另行同步唯一契约、模块、运维与实际测试。

主要只读证据：

- `core/ports/target/identity_repository.hpp`：credential目前有current/version/by_certificate/login/save；
  exact assignment只有for_subject/save；authorization仍是P03按主体find/save。
- `infrastructure/postgres/schema/identity_security.hpp:9`：credential head、历史材料和exact关系约束。
- `infrastructure/postgres/schema/services.hpp:35`：authorizations只有subject/version/dirty。
- `infrastructure/postgres/src/common_work.cpp:640`：authorization save更新必须version=expected+1。
- `proto/kiarabot/v1/identity.proto:78`：InvalidationRecord及AuthorizationSnapshot。
- `proto/kiarabot/v1/identity.proto:299`：Set/RevokeAssignment的ID与expected_version。
- `proto/kiarabot/v1/platform.proto:135`：cursor含source_service/epoch/scope/sequence。
- `doc/21-service-contract-design.md:220`：重启或窗口断档明确resync，最近10分钟/4096条重放。
- `doc/22-transactions-and-migration-design.md:60,161`：可合并dirty/version、撤销不受普通outbox满阻断，
  先取权威快照水位再消费之后增量。敏感操作仍在线检查，失效流只是加速器。

## 一、bootstrap verifier 的当前定位

### 已有能力与确切缺口

`credential_bootstraps`保存(id,version)材料、verifier_key/tag、绑定、期限，audiences按position
保存；tag长度32，material=2复合FK指向正确历史类型。`credential_heads`保存每个credential
唯一当前version；只有certificate有唯一当前leaf_fingerprint及指向当前证书版本的复合FK。

bootstrap目前没有按(key,tag)查询的typed端口，没有该查询索引，也没有保证多个当前
credential不会共享同一verifier的唯一约束。历史表按(id,version)查找足够，但不能从不可信
opaque证明定位真实owner。不能要求客户端先报principal/credential/binding当认证权威，也
不能枚举主体或扫全表到内存。证书by_certificate不是bootstrap的替代入口。

### 最小拟端口与约束

建议Core冻结类似：

```text
by_bootstrap(TokenVerifier{purpose=WorkloadBootstrap, trusted_key_ref, tag})
  -> Outcome<optional<CredentialVersionRecord>>
```

- 原始proof只进成熟crypto。服务以可信、受限verification-key集合生成候选HMAC，在无UoW时
  完成crypto；PG只接验证索引，不接raw token或由body选出的身份。
- 查询必须以`bootstrap.id=head.id AND bootstrap.version=head.version`限定CURRENT材料。
  正确定位后仍由Core重读/验证principal、credential Active/期限、绑定、audience/gate和实际
  接收连接身份；匹配索引本身不授予业务权限。客户端字段最多作认证后的相等性声明。
- 推荐与现by_certificate一致返回当前结构记录，拒绝/过期策略由Core决定；缺失是空optional，
  SQL/解码故障是Failure。多条CURRENT匹配不能LIMIT1任选，应Internal/拒绝。
- 一个非唯一历史(key,tag)索引+CURRENT join可提供有界数据库定位，但**不能**保证当前唯一性。
  推荐另有唯一CURRENT locator，例如head增加bootstrap_key/tag并以复合FK指向当前材料，
  或等价的唯一当前locator关系；由Core/父任务选择并冻结覆盖性检查。
- 不可把UNIQUE(key,tag)直接放到整个历史表：同一材料的撤销新版本仍保存原验证材料，合法
  历史可重复该tag。head换代/换类型时必须原子替换或清除CURRENT locator，旧材料不能被认证。
- 沿用证书head模式时，需明确“当前bootstrap必须有locator、其他材料必须无locator”的覆盖
  不变量。仅NULL可跳过的FK不保证覆盖；可考虑head材料判别+check/FK，不能把漏写伪装不存在。

当前credential save已能append版本+head CAS同事务；新locator应由这个save**唯一负责**，
应用不能再单独写索引，避免重复base同步问题。Unknown后按原请求身份/成功收据恢复，
不得新建credential来猜原提交。新增索引/约束需新空库schema版本和descriptor，不能自动升级。

## 二、ExactAssignment 按ID与条件撤销

### 已有能力与确切缺口

exact_assignments具有id PK、subject+version；child关系以assignment开头为PK，能够高效按ID
读取完整paired tuples。base grants的id/subject/version与exact头有延迟复合FK，save已复用
Core helper生成元数据、原子CAS两者。不需要为简单按ID查询新增SQL表或另建授权库。

但RevokeAssignment wire请求只有assignment_id+expected_version，当前for_subject要求先知道
真实主体。不能信body补主体、从actor身份猜owner或全主体扫描。需要直接从ID找到真实owner
与完整旧scope，才能由Core检查旧/新管理范围。数据库的id PK不会自动变成typed入口。

现类型/表均无revoked状态或墓碑。把roles和explicit都清空会违反Core单basis非空校验；
save不是撤销。物理DELETE又会丢失最后版本/owner/旧scope，并允许存储层create重用这个ID，
不能直接拿它实现有版本、幂等与不复用ID的撤销承诺。

### 最小拟端口与必须冻结的表示

至少需要：

```text
current_assignment(AssignmentId)
  -> Outcome<optional<AssignmentEntry>>
revoke_assignment(AssignmentId, expected_version)
  -> Outcome<AssignmentRevocation>  // 含真实subject、ID、新version及必要原scope/状态
```

`AssignmentEntry`究竟是active ExactAssignment还是含Active/Revoked状态的wrapper，由Core定稿。
建议保留可按ID读取的有版本墓碑/状态；for_subject供授权读取时只返回有效exact tuples。
不可让空tuple偷偷表示revoked，亦不能让后续Set把同一墓碑ID换给新主体。

需要同时确认：

1. 撤销后current查询返回状态还是空、缺失/已撤销/错expected如何映射、同键成功重放与新键
   重复撤销的语义，以及uint64最大值处理。不得由SQL自行选择NotFound/Conflict或隐式幂等。
2. base ResourceGrant没有revoked字段：撤销后保留、抑制查询还是删除live投影必须明确。
   exact FK要求同版本base存在，不能只删base；若删两个live头，须另留有版本ID墓碑及必要
   旧scope以支撑重放前再授权。旧base元数据不能被拿来重新授予权限。
3. 若保留state列，权限查询必须过滤revoked，child保留仅供历史/范围核验；若移往墓碑关系，
   create-only与旧base写入也须尊重不复用规则。两种方案都需新schema，不能只加一个DELETE端口。
4. repo负责base投影同步；Core在同一UoW完成当前actor/实际owner、旧新scope并集检查、
   assignment CAS、subject authorization version/dirty、普通成功收据及审计。

Revoke wire只返回新version，现有普通ResourceMutationResult可保存原成功事实，无须秘密
收据。Set返回AssignmentRecord：当前ReplayResult有ResourceGrant元数据，但没有明确的
ExactAssignment/AssignmentProjection快照契约。现helper产物在单basis约束下可经**明确的
Core逆投影**恢复，不能把任意P03 ResourceGrant都当成exact；父任务应选择冻结该逆投影，
或末尾追加无秘密typed结果。PG不自行决定元数据等于完整原响应，尤其不能重放时回查live。

## 三、dirty/version 与失效流、范围快照

### 当前安全基础可保留，但不足以生产流

每主体authorizations PK行保存uint64 version和dirty，Refresh/Revoke等已同事务推进，
普通outbox满不妨碍该标记；重启不会丢失最后权威版本。这是合并恢复的良好基础。

当前缺少：

- bounded dirty枚举/领取、发布后对“观察到的那个version”确认的typed维护方法；
- 流epoch、序号/high-water、重放下界、时间戳、持久或明确定义的重启resync状态；
- 最近10分钟/4096条的顺序重放与安全裁剪；
- 对ResourceScope的全范围授权快照查询。for_subject既不枚举全部相关主体，也不提供
  和流cursor一致的scope cut；已删/已缩scope的旧影响范围无法从当前grant还原；
- 全库Snapshot容量、每记录scope/字节容量、发布/读取批量和订阅端缓冲的可信上限。

三类数字不能混用：assignment.version、每主体authorization.version、全流sequence。
取所有主体version的max不是全局提交水位；一个主体到v100不表示另一个主体的v2已被消费。

### 建议的最小typed持久能力（接口草案，非冻结）

| 能力 | 最小输入/输出与不变量 |
| --- | --- |
| dirty批读 | trusted limit +内部扫描位置 -> subject/version及待发布影响信息；不是客户端stream cursor |
| 变化登记 | 业务同事务记录当前version与影响scope/保守resync事实；必须声明与authorizations.save谁唯一负责同步 |
| 发布与确认 | expected subject/version +稳定publication身份 -> 原或新cursor；append与清dirty在同一UoW |
| 当前流状态 | typed source/epoch/high-water/replay-floor；缺初始化状态Internal，不默认0或重建 |
| replay批读 | 绑定source/epoch/canonical scope的after cursor +可信limit -> 有序记录/next扫描cursor/明确resync |
| 原子scope快照 | 已授权scope +可信总容量 -> 完整safe assignment投影及同一cut的cursor/版本；超额显式失败不截断 |
| 有界裁剪 | DB UTC、可信batch -> 前缀删除及同事务推进replay-floor；不接用户now伪造保留期 |

这些可组合为一个或少量repository，仍借用同一CommonWork，不能暴露pqxx或另开事务。
若Core选择只发整体resync而非精确scope增量，接口/数据可以更小，但必须先明确wire形状与
授权隐私边界，不能由PG发明“所有主体广播给所有订阅者”的规则。

### dirty确认、影响范围与容量

现authorizations.save要求version=expected+1。发布后仅清dirty不应制造新的业务授权版本，
因此不能用通用save假装维护确认。拟ack应只在subject/version仍等于观察值时清dirty；
若已变成v+1必须保留新dirty，不能按subject无条件清。已确认重复/版本变动的返回语义由Core定。

dirty bool不记旧scope。授予从A移到B或撤销后，仅看当前B不能通知缓存A。可冻结以下一种：

- 持久合并old/new影响scope；但集合会随离线发布积累，必须有scope/字节容量及安全溢出策略；
- 保守主体失效/整个已授权范围resync；须确认如何过滤而不向无关订阅者泄漏principal IDs。

新失效history与普通outbox容量分离。已确认窗口是10分钟/4096条，超窗/溢出必须推进明确
replay下界并触发resync，不能阻塞已授权撤销、默默丢dirty或假装“无事件”。4096条本身
不限制字节数：resource列表、ID长度、每batch和transport缓冲仍须可信有界。
DB写失败不能报持久成功；发布失败保留dirty，流读取依赖失败返回Failure，不能成功空集合。

### 游标、重启、未知提交

- 客户端cursor必须同时校验source=Identity、epoch、规范scope相等及sequence范围；不能只取
  一个序号放行，也不能把未来sequence当空历史。范围过滤后的序号有跳跃不等于丢通知。
- 建议把current epoch/high-water/replay-floor持久化。cursor序号保持uint64精确，不能转double
  或signed bigint截断。若用非事务PG sequence，要明确rollback号洞，不能按连续数字猜缺口。
- 文档已要求服务重启明确resync；不要假定旧epoch重启后继续有效。新epoch由Core流生命周期
  用可信随机生成并正常事务登记，不在PG open/probe上偷偷改变业务epoch，也不做启动DDL。
- 不需要默认为每个网络订阅者建持久ACK行；现wire是server stream，没有客户端ACK承诺。
  保存有界history+floor，让客户端带cursor重连，避免不受控的subscriber持久状态。
- 业务Unknown仍按原命令收据/身份恢复，不新建授予/凭据。发布Unknown可按稳定的
  (epoch,subject,authorization_version)或冻结batch身份查原publication/cursor，防止盲目分配
  新seq；append与ack在一个本地事务，任意一侧未知不能无条件清理另一侧。
- Watch每次只持短UoW拉取/检查，释放后再网络发送；当前权限/证明到期检查仍由Core/传输做，
  不持2秒事务等待网络，也不把失效流当在线授权的替代。

### 快照与cursor的两个契约问题必须先定

1. AuthorizationSnapshot.authorization_version只有一个scalar，而scope可能跨主体。
   它是全局投影revision还是限定单主体版本？不能取max(subject versions)凑值；需要冻结
   允许scope形状、scalar含义，以及与cursor.sequence和各主体version的关系。
2. 当前CommonWork为READ COMMITTED。一个UoW的多次SELECT不天然是同一MVCC快照。
   单writer/permit确实串行本进程业务，但快照一致性仍应由明确原子查询cut或专用隔离语义
   保证，不能靠先读watermark、另读grant就自称一致。返回快照后再看after增量，不留中间缺口。

scope快照必须只暴露获准范围的paired tuples，不泄漏同Assignment的其他资源，更不能将
权限/资源独立列表跨id做笛卡尔积。安全snapshot不包含验证材料/秘密，也不代替principal、
source credential/session、resource gate的当前核验。

## 冻结与实现顺序建议

1. Core/协议拥有者确认bootstrap定位语义、assignment撤销状态/base投影/收据，失效scalar、
   scope、epoch/reset/resync与容量；必要时先同步proto及doc21/22。
2. 冻结typed模型/端口及严格fake/纯规则；明确单一同步责任，避免再次重复base CAS。
3. PG只在确认后更新私有schema/约束/typed查询、版本/manifest、模块/运维文档，再做真实
   正常/拒绝/并发CAS/Unknown/丢通知/窗口溢出/重启/损坏/依赖故障测试。

当前没有实现这些后续端口或schema。本报告不是P04验收，也不把此前正常21项组合门禁
视为剩余授权/凭据/流职责已完成。

## 本次实际检查与未运行项

执行只读rg/sed/nl/git show/log核对上述固定提交文件，未读取其他工作树的未提交内容。
报告校验：UTF8、末尾换行、主要三组缺口与“非冻结”标记、git diff --check；保护基线无变更。
未运行C++构建/DB fixture/proto生成：本次没有改C++、schema或协议，只有只读发现报告，
运行它们不能证明尚未冻结/实现的端口。本文不声称做过实际新schema故障或流恢复实验。
