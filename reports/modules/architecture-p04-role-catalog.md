# P04默认角色跨契约一致性

目标：Core Identity默认角色目录。允许修改policy头、identity_rules_test、API与直接
模块文档及新Ruby跨契约测试；不改App/PG/CMake/doc18。

输入为已知FixedRole/Permission，输出为明确bool；未知枚举或名字拒绝。外部能力唯一
来源为OpenAPI的x-control-policy.target_role_capabilities。用户明确要求model.read三角色、
identity.session.read/revoke为admin/operator；API补operator的model.read，删除与同处
notes及G08冲突的admin action.inspect。Core保持敏感action.inspect/message.read默认拒绝。
account.policy.manage原本已为admin/operator，不改。本人会话专用规则不变。

API完整基点为父已提交a231c9d，不能使用本分支缺G08的旧API表。Core保留
account.read/plugin.read/adapter.read/host.read三角色及plugin.config.read的admin/operator。
viewer exact AccountRead成功断言保留，不因旧API错误降权。API相对a231c9d只删除admin
action.inspect、补operator model.read；其余G08内容原样导入。授权仍按单个assignment的
exact resource，没有跨scope扩权或默认全局角色；metadata helper继续复用实际规则。

回归：identity_rules_test支持--role-catalog，通过kPermissionNames名称调用实际函数并
输出role/tab/permission；Ruby读取API并执行该binary，逐角色比较精确集合，拒绝未知
角色、重复行、格式错误和执行失败。不复制第二份预期权限表。测试路径参数可选传入
临时API文件用于负向验证，不修改唯一来源。未知枚举由volatile运行时整数转换，保留
拒绝断言，避免clang-analyzer EnumCastOutOfRange常量诊断。policy显式include algorithm/
string，支撑bdc05b9 metadata helper而不依赖传递include。

精确文件：

- api/openapi.yaml
- core/include/kiarabot/core/domain/target/identity_policy.hpp
- core/tests/architecture/identity_rules_test.cpp
- proto/tests/identity_role_catalog_test.rb
- doc/02-core.md
- doc/06-control-api.md
- reports/modules/architecture-p04-role-catalog.md

文档影响：API为唯一来源需同步；doc02说明实际默认权限，doc06说明Control消费的目标
角色目录及跨契约验收。其他模块/协议没有字段、错误、生命周期或责任变化，无需更新。
doc18保护只读。doc21 G08的敏感权限显式授予规则保持不变，无需修订。

依赖：bdc05b9由父任务代同步；本工作树git cherry-pick因index.lock写权限被拒绝，
没有升级权限或手工改写git元数据。buildchat负责新Ruby测试的CMake注册，本批不改CMake。

验证：

- 严格C++20/Wall/Wextra/Wpedantic/Werror编译执行规则测试，通过。
- Ruby实际目录比较，3个角色精确集合通过；修改临时API为viewer加message.read时明确失败。
- ruby proto/tests/target_api_test.rb通过，完整G08基点的46个target HTTP operation与references检查通过。
- ruby proto/tests/api_contract_test.rb失败于旧not_implemented操作清单；从HEAD导出原始
  script/API/AsyncAPI到临时目录可复现相同失败，确认非本变更引入，不放宽旧断言。
- ASan/UBSan、固定clang-format16、clang-analyzer与架构gate结果追加于下文。

旧基点验证已被完整G08基点重跑取代；以a231c9d API的实际结果为交付依据。
规则测试ASan/UBSan及sanitized binary的Ruby比较通过；固定format16
dry-run通过；clang-tidy22 clang-analyzer-*、warnings-as-errors与明确macOS SDK头路径
检查通过，无EnumCastOutOfRange；Ruby语法检查通过；boundary、inner gate、diff空白
检查通过。依赖bdc05b9尚未正式导入，本批不把临时overlay验证当作正式集成。
另逐行验证当前API相对a231c9d只有admin/operator两行角色数组差异，G08其余内容未改。

未运行全仓build/check-toolchain/PG/RPC验收：本能力是纯授权函数与跨契约对比，新测试
注册归buildchat，其他生产模块不在本批范围。不将目录一致性当作端到端授权验收。
