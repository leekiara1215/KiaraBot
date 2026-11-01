# P04 完整会话成功快照

目标为Core共享domain的成功收据。RevokeSession既有wire响应要求持久created_at，
旧ReplayResult的LoginSession不含创建时间，不能用本机时间或可变业务行补原始成功。
原SessionMetadata定义移service_records同一namespace/布局，ReplayResult末尾追加
该完整无秘密快照（index12），不重编号旧12类，不将秘密/验证材料塞入普通收据。

文档影响：同步doc02和本记录。wire/API字段及语义已要求完整投影，不改proto/API/
doc01；对应PG关系结果编解码/schema版本和测试由独立基础设施能力同步。doc18只读。
重放先当前鉴权，结果数据是原快照；response.checked_at代表本次权威验证时间。

Core现有foundation/invariants/persistence_contract/persistence_rules/time/identity_contract
六个相关测试以C++20 -Wall/-Wextra/-Wpedantic/-Werror编译执行通过，三个改动头
format16、边界/inner与diff通过。PG codec中的封闭visitor需新增此分支，故主树只在
基础设施同步提交后一起整合，不以类型编译代替真实库回放验收。未运行本能力PG/网络。
