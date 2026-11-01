# P04 短期证明签发重试契约

目标：协议模块的 IssueDelegation / ExchangeDelegation。输入仍为当前主体证明、调用服务、
目标 audience 与精确权限资源；输出为重新授权后签发的短期证明或既有稳定 Failure。
无字段、枚举、RPC 签名、截止时间或错误码变动；两方法 requires_idempotency 改为 false，
target_spec 同步标为无业务变更、不要求 key。重试可产生新证明，不能引入配额消费或业务副作用。

允许范围：identity.proto、target_spec、目标契约测试、proto README、doc01 契约/运行语义、
doc02、doc21 和本报告。不会保存 JWT 普通收据，也不扩充秘密收据 variant。

文档影响：上述文档直接解释签发/重试边界，已同步。API 的 REST 方法、请求响应和幂等要求
未改变，doc06/API 无需修改；存储、部署和其他模块责任不变，doc05/doc08 无需修改。
冻结 doc18 保持只读。此为开发中目标契约的语义细化，不承诺旧目标客户端混合运行；
不能以此放宽其他命令的幂等和未知提交恢复要求。

实际检查：

- 固定 full-locked 完整构建成功，生成实际 C++/Python/descriptor。
- `ctest --test-dir build/full-locked -R p01_target_roundtrip --output-on-failure` 通过，30.37秒；
  descriptor 与独立方法清单选项一致，Python/C++ 往返通过，新增证明重试及其他命令保持幂等断言。
- 固定 protoc/Python、实际兼容性工具运行 `proto/tests/check_contracts.sh` 通过，包含六种
  descriptor 兼容性故障场景及 legacy reader。未更改兼容性检查器或跳过失败场景。
- `ruby proto/tests/target_api_test.rb` 通过，46 HTTP 操作/错误映射/REST 和 WS 引用一致。
- Python 语法、`git diff --check` 通过；相对原始提交的 doc18 差异为空。

单独的旧 `proto/tests/check_target.sh` 首次尝试在手工 pkg-config 链接时缺少 macOS
CoreFoundation framework，链接失败；改用仓库正式 CMake 目标重编并运行了完整往返检查，
未修改测试断言或屏蔽链接诊断。未为此局部选项变更重复全仓统一门禁；下一生产组合里程碑再运行。
