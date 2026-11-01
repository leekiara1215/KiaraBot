# Protocol tests

本目录包含跨语言契约的静态门禁、descriptor 兼容性工具、跨语言 round-trip 和 API 契约检查。所有"未运行"都必须报告为未运行；脚本在缺少依赖时以退出码 `2` 失败，不能当作通过。

## 无需 protoc 的 Python 门禁

```bash
python3 proto/tests/check_enum_consistency.py
python3 proto/tests/check_implementation_status.py
python3 proto/tests/check_semantic_rules.py
```

- `check_enum_consistency.py`：解析 v1 全部 10 个枚举（含嵌套枚举），与 `proto/tests/enum_baseline.json` 的冻结名称/数值逐项比较；名称/数值/顺序漂移、删除、`allow_alias` 都会失败。新增枚举值允许，但必须在同一变更更新基线。随后比较 `ErrorCode`、`HealthState`、`PluginState`、`ActionStatus` 在 OpenAPI/AsyncAPI 中的字符串词表。
- `check_implementation_status.py`：以 `proto/tests/contract_status.json` 为唯一登记表，覆盖每个 Protobuf 字段/oneof 载荷、枚举值、AsyncAPI 频道/事件、OpenAPI operationId 和跨契约语义规则；要求未实现项的 `.proto` 内联 `CONTRACT_STATUS` 注释、`api/*.yaml` 的 `x-contract-status` 列表、存在的证据路径、`doc/01-contracts.md` 状态表行，以及冻结的权限/能力/订阅词表同时出现在文档与实现源码中。
- `check_semantic_rules.py`：校验统一 ID 语法（文档表 ↔ OpenAPI/AsyncAPI 参数）、Action deadline 300 秒上限、gRPC 传输状态与 in-band 业务错误、reserved/deprecated 规则、优雅停机/drain、REST Action 的 168 小时保留/EXPIRED 语义、admin 无通配且无默认 `message.read`、PluginManifest 枚举只声明已实现值。

## 需要 protoc / 构建产物的完整门禁

```bash
./proto/tests/check_contracts.sh
```

`check_contracts.sh` 依次执行：oneof 判别结构检查、上述三个 Python 门禁、OpenAPI/AsyncAPI 静态引用检查、`protoc` 编译全部 v1 `.proto` 为 descriptor，然后：

- 调用 `descriptor_compatibility_test.sh`（6 个负例：新增兼容、字段类型、字段删除、枚举重编号、oneof 改名、service 签名）——该脚本此前没有任何调用方；
- 设置 `KIARABOT_LEGACY_READER_TEST=1` 时，用 `KIARABOT_PYTHON`（默认 `sdk/python/.venv/bin/python`）与 `KIARABOT_GENERATED_PYTHON`（默认 `sdk/python/build/generated`）执行 `action_contract_test.py`（旧 reader 线兼容）——该测试此前只被本 README 提及；CI 与 acceptance 应开启该开关，本地 protoc-only 环境默认跳过并打印提示；
- 若设置 `KIARABOT_BASELINE_DESCRIPTOR`，再调用 `kiarabot_descriptor_compatibility` 比较基线 descriptor。

缺少 `protoc` 或 descriptor 兼容工具时，脚本以 `2` 失败并说明缺少什么；开启 `KIARABOT_LEGACY_READER_TEST=1` 后缺少 SDK 环境/生成包同样以 `2` 失败。默认未开启时旧 reader 测试跳过并打印提示，因此 `scripts/check-toolchain.sh` 的统一本地检查不需要 SDK venv。`PROTOBUF_INCLUDE_DIR` 会作为额外 `--proto_path` 传给 `protoc`，与 `scripts/ci/check-descriptor.sh` 保持一致。

For the Action feature negotiation and legacy Python reader test, generate the SDK types, then run `PYTHONPATH=sdk/python/build/generated sdk/python/.venv/bin/python proto/tests/action_contract_test.py` from the root. This verifies legacy readers preserve unknown fields, legacy servers default to no enabled features, query selectors and unknown status values. The C++ roundtrip test also covers the new frames and unchanged field numbers. This wire test does not certify that a service implements Action dispatch.

## descriptor 兼容性

CI 可设置 `KIARABOT_BASELINE_DESCRIPTOR` 指向上一发布版 descriptor。`kiarabot_descriptor_compatibility` 按文件名、消息字段编号及完整元数据、oneof、嵌套类型、扩展/保留范围、枚举值、service 方法签名与 options 做语义比较；新字段和新枚举值允许，删除或改变已发布定义失败。没有发布版 descriptor 时只能比较仓库内基线，不能声称发布兼容认证。

构建 proto 预设后运行 `./proto/tests/descriptor_compatibility_test.sh`（也由 `check_contracts.sh` 调用）。该夹具测试不替代与发布基线的比较。

## REST/WS 契约与 Core 策略对照

`ruby proto/tests/api_contract_test.rb` 校验频道 envelope/payload 引用、会话握手、messageId 唯一性、`x-contract-status` 列表、messages 频道不可用标记、admin 显式权限、PluginManifest 枚举、ID 参数、Action 查询的 168 小时保留/EXPIRED 语义；随后需要 `build/proto/kiarabot-control-application-test`（或 `CONTROL_POLICY_BINARY`）运行 Core `--dump-policy`，逐项对照 23 个 REST 操作×权限×角色×mutating 与 OpenAPI。缺少该二进制时本测试不能完成，属于未运行而不是通过。

该测试验证：

- Event 和 Action envelope 只用 `oneof` 判别；
- Protobuf、OpenAPI、AsyncAPI 的 error/health/plugin/action 词表一致；
- OpenAPI 声明 `Idempotency-Key`，AsyncAPI 绑定首帧 bearer 认证并引用 hello/resume/resync 与 `/ws/messages`；
- 不可用的 REST 操作/WS 频道/事件由 `x-contract-status` 与状态表显式标注；
- 所有 Protobuf 源（含 imports）可编译为 descriptor；
- 生成的 C++ 消息以同一 oneof 判别序列化/解析。

## P01 统一目标独立入口

`KIARABOT_TARGET_PYTHON=<含 protobuf 和 grpcio-tools 的 Python> proto/tests/check_target.sh` 不依赖根 CMake，临时生成全部 C++/gRPC 与 Python/gRPC、descriptor，编译反射 round-trip 工具，执行目标方法/descriptor options 逐项匹配、fence presence/零值反例和 Python→C++→Python 消息往返。临时目录退出清理，不提交生成物。Python generator 使用其自带兼容运行时；禁止修改生成版本检查。`target_contract_test.py` 的校验 oracle 是规格夹具，不是生产边界实现或数据库并发证明。旧 descriptor 兼容夹具仍是旧开发快照检查，不能当作目标兼容承诺。
