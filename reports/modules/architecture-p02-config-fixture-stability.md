# Control reference 配置夹具终止时序

统一固定入口捕获历史test_config_delivery的停用后退出竞态：Core已明确停用实例并
返回PLUGIN_UNAVAILABLE，SDK按其既有语义以非零退出；若SIGTERM先到则正常0退出。
旧测试在停用已发生后仍总要求0，先前整套51/51通过及本次单独重跑通过都不能消除该竞态。

仅修改control/api/tests/config_plugin.py及test_config_delivery.py：辅助子进程保留
PluginError非零结果并输出准确结构化终态；普通stop仍严格要求0。显式disable后测试
先等待子进程自然以1退出，再精确校验PLUGIN_UNAVAILABLE；任意其他异常、错误码、
缺失终态或超时继续失败。没有修改生产SDK、reference源码或放宽业务拒绝断言。

实际：针对真实reference Core/SQLite/HTTP/SDK的test_config_delivery.py修改前后各
运行通过，最终明确终止分支用例1/1通过（1.66秒）；Python编译和git diff --check通过。
该测试仍覆盖保存、应用ACK、拒绝、超时、重连、重启和停用。完整27项suite将在统一
门禁复验；本局部结果不代表新架构业务已实现。

目标为Control测试夹具；无生产接口、状态、错误或配置变化，属无公共文档影响。
proto/API及doc06无需同步；doc18只读。没有C++变化，无需为此任务重跑format/tidy。
