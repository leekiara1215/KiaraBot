# 契约证据与target归属门禁

工作区kiarabot-contracts，从干净403a543新建codex/p03-contract-evidence，旧分支保留。允许proto/tests、相关登记、doc/01-contracts/01-runtime-semantics及本报告；不改CMake/业务/proto定义/API/doc18。

输入为reference隔离后的历史证据及P01/P02状态，输出为明确路径解析和独立target生产可用性。错误语义为门禁非零失败：未知/缺失映射、blob漂移、symlink/越界、生产声明缺当前source/target测试、reference冒充生产均拒绝。

文档影响：必须同步proto/tests/README及doc/01-contracts、运行语义的执行范围说明，因为旧implemented的无范围解读会误导可用性；无线协议/HTTP/WS shape、状态码或生产行为变化，因此proto/README、API/doc06及业务模块说明无需由本任务修改，doc18保持只读。

初始实际运行check_implementation_status.py与check_contracts.sh均失败，43条旧Core evidence路径不存在。修复保留全部categories/既有断言，证据原逻辑路径通过已审查core/reference/manifest.json明确映射且逐blob校验。schema v2将历史categories界定reference非生产，target默认not_implemented，仅P02两个HTTP探针有current runner/target测试证据override；ready仍503。词表检查在reference范围保留，target词表扫描排除reference/客户端/测试字符串。业务方法没有因监听或历史成功变为可用。

## 能力一：证据映射与执行范围

实际验证：9项evidence_scopes_test.py通过并接入check_contracts.sh；check_implementation_status.py、enum/semantic门禁、Ruby target_api_test.rb（33操作与全部引用）通过。以临时Python环境grpcio-tools1.76.0/protobuf6.33.5生成全部Python消息，再显式设置KIARABOT_LEGACY_READER_TEST=1、KIARABOT_PYTHON、KIARABOT_GENERATED_PYTHON运行check_contracts.sh，descriptor生成/6个兼容场景/legacy-reader全部通过，无默认skip。该临时Python3.9环境不是P00固定环境，不据此声称固定全量环境完成。

通过git show 403a543逐项比较确认categories、evidence原逻辑声明、vocabulary完全未变；新增执行范围不取消历史implemented。bash -n、git diff --check通过。无C++/proto/API定义变化，不重复full/格式/tidy/正式API lint；本任务只读对应代码确认当前生产两个探针，未自行跑六进程业务或宣称77个RPC已实现。
