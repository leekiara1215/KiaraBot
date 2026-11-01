# 目标Core时间跨平台精度修正

基点b2761da3e44ee96932d601b6975b01b2f9b4b5c1，分支codex/p03-core-ports。
优先于尚未提交的P04接口独立交付；不混入P04文件，不修改reference/proto/API/Control树。

## 精确修改文件

- core/include/kiarabot/core/domain/target/foundation.hpp
- core/include/kiarabot/core/domain/target/time.hpp（新）
- core/src/runtime/platform/service_config.cpp（仅获准to_time_t边界）
- core/tests/architecture/time_test.cpp（新）
- doc/02-core.md
- reports/modules/architecture-core-time.md（本报告）

目标TimePoint由native system_clock::time_point改为system_clock+nanoseconds，并静态
要求signed64 rep；同一epoch ns跨主机保持完全相等，不再由macOS native微秒丢低位。
新增TimestampParts(int64秒,int32 nanos)与time_point_from_parts/time_point_to_parts：
输入先核验nanos[0,999999999]及int64 ns可表示边界，再安全乘加；下界floor秒本身的
乘法会溢出，专门以INT64_MIN+已校验偏移构造，绝不先duration_cast再检测。
输出负数使用floor秒/非负nanos，所有内部值含INT64_MIN/MAX精确往返。
不能表示的wire极端年份以Failure InvalidArgument/NotAccepted/CorrectInput拒绝，禁止wrap。

最小为(-9223372037,145224192)，最大为(9223372036,854775807)；表示范围是上述
int64 epoch ns，约1677–2262年；当前业务时间可表示，wire更宽年份不假装受支持。
唯一外层改动是证书to_time_t前显式time_point_cast<system_clock::duration>，该秒级
证书校验转换不承担业务时间持久化。P03 PG exact-ns codec/列仍需基础设施独立验收。

文档影响：更新doc/02与本报告。无新增HTTP/gRPC/业务状态或线协议字段，proto/API与
其他模块/运维文档无需变化；doc/18只读不改。未来wire边界调用统一helper的实际接线
由相应模块完成，本提交不声称已改全部边界或通过PG精度验收。

## 实际检查

- time_test以C++20 -Wall/-Wextra/-Wpedantic/-Werror独立编译执行通过。
- 同测试加ASan/UBSan编译执行通过，无溢出/内存报告。
- 正/负epoch、0/1/999ns亚微秒、当前附近纳秒、INT64_MIN/MAX、两端越界1ns、
  无效nanos、极端秒及wire年份1/9999拒绝均验证，不使用当前时钟或sleep。
- config driver对新TimePoint/显式to_time_t转换以-Werror重新编译，原配置/TLS/FIFO/
  固定证书时间边界与新EC立即加载测试通过；没有为解决类型错误改认证/证书策略。
- format16、既有内层测试、边界/inner及diff检查在收口时补录。

根CMake不在授权范围，建议工具链注册time_test为p03/unit；未执行PG/跨进程实际
时间codec验证，不将纯测试当作P03完成。P04未提交文件保持独立，供后续能力继续。

收口：foundation/invariants/persistence_contract/persistence_rules/time五项已提交相关
测试及另行未提交的identity_contract均按新TimePoint重编译执行通过（共6项）。固定
format16 dry-run、模块边界、inner、git diff --check通过；文档中两个新增文件路径存在。
Git元数据写权限此前受限，精确尝试仅暂存本报告列出的6文件，不涉及P04未提交文件。
