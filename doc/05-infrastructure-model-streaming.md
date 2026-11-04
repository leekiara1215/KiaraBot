# Model HTTP streaming 基础设施

## 实现范围与边界

本节对应[批准的流式契约](plans/2026-11-04-model-streaming-plan.md)及
`proto/kiarabot/v1/model_stream.proto`。第一实现切片只接通真实HTTP Provider的纯文本SSE转换和
Core纯值端口；Model事务/在线授权、RPC、Runtime/PluginBridge和SDK完整链另行实现验收。
Provider支持streaming不代表插件能力已经可协商，也不能据此翻转整条target能力状态。

代码入口为 `infrastructure/src/model_http_stream_provider.cpp`，由既有
`ModelHttpProvider::chat_stream` 调用；旧 `chat` 仍发送stream=false，结果、错误映射和返回类型不变。
Core值类型在 `domain/target/model_stream.hpp`，sink/取消接口在 `ports/target/model_stream.hpp`；
这些头不依赖HTTP、protobuf、数据库、第三方解析器或Python。

Provider方法接收pinned ModelProviderRequest、借用sink和C++20 stop_token。调用者必须保证secret、
sink及取消源在方法返回前存活。sink的try_emit只能做非阻塞队列准入：Accepted表示已复制文本，
Full表示没有复制，Cancelled永久停止本次调用。Provider对Full最多等250ms并重复同一个值，
不会直接执行网络Write/插件回调；异常在基础设施边界转稳定错误。应用/Bridge仍需实现各自的
有界队列、串行writer、当前授权和有界join；本切片不以同步回调冒充这些尚未实现的职责。

ModelProviderStreamResult独立记录Provider outcome、termination、finish reason、usage、
may_have_billed、provider_dispatched、实际准入的字节/帧数及partial。Provider Succeeded只证明
收到合法finish+[DONE]且HTTP传输成功，不证明Model txn2已经提交。Model应用必须持久化并确认后
才发客户端COMPLETED。默认ModelProviderPort不支持streaming，返回CAPABILITY_NOT_SUPPORTED、
未dispatch且未计费，不调用旧chat后假分片。

## 依赖来源、许可与缓存

复用已锁curl 8.13.0，SSE字节解析采用
[eventsource-c v0.2.0](https://github.com/rexxars/eventsource-c/tree/95309f6191df705e7c00acfba0675a110ebb79fa)
的C99 parser，MIT许可。固定commit为 `95309f6191df705e7c00acfba0675a110ebb79fa`；来源URL、
归档SHA256/SHA512、LICENSE、parser源和头文件SHA256在
`dependencies/architecture/dependencies.lock.json` 的source-archives.eventsource-c登记。
归档SHA256为 `87b1cc6c5a3128a0ad13059e1124c4b89981fc0d149cc9491e237e1f88733263`。

`cmake/model-streaming.cmake` 从唯一锁读取URL/hash，FetchContent只解压到构建缓存，验证许可、
头和源摘要，只把src/sse_parser.c链接进生产。跳过上游CMake，不编译上游HTTP transport，
不使用其client/reconnect/Last-Event-ID；vcpkg manifest及已安装依赖不变。C99仅作用于第三方
parser目标，手写C++仍为C++20。UTF-8验证复用既有Protobuf依赖的utf8_range::utf8_validity，
不自写编码算法。第三方源码/生成物不得提交仓库，发布打包须保留锁中MIT许可通知。

上游9项回归保留为dependency测试：5项parser，4项fake-transport client用独立仅测试support库。
测试support库没有生产consumer；生产Provider只链接parser。另保留上游200000输入的确定性
fuzz smoke。库的id/retry元数据不作为业务身份或重发指令；本模块只消费标准data/event边界，
不声称实现浏览器EventSource的连接/恢复语义。

## 标准解码和内存界限

外层 `model_sse_parser.cpp` 包装固定解析器，不发明SSE变体。它处理CR/LF/CRLF、任意分段、
开头BOM、注释、空data、多个data合并、未知字段及未闭合EOF；只有空行闭合事件才回调，EOF不补造。
event/data回调视图只借用到回调返回。合法UTF-8可以跨任意网络块，完整字段用成熟库验证；
非法UTF-8数据/事件名、含NUL事件名作为畸形上游失败，不交给插件。

上游明文字节总计≤1MiB，单行（包括注释/未知行）≤64KiB，合并事件data≤64KiB；固定buffer保存
当前事件/类型和受限metadata。累计文本≤64KiB，输出按UTF-8字符边界分成每帧≤4KiB。HTTP响应头
另外限制为16KiB。所有解析、容量和consumer错误都是粘性的：C parser即使继续处理同一输入块，
包装层也不再调用consumer，防止overflow后悄悄投递下一事件。错误后不能继续或自动重开HTTP。

Provider请求stream=true、stream_options.include_usage=true，期待text/event-stream。只支持
单choice/index0、assistant role及delta.content文本，空delta允许；finish为stop、length或
content_filter，后两者为TRUNCATED。有finish之后才接受choices=[]的权威usage-only chunk；
没有usage也可成功，known=false且计数0。最终必须有[DONE]。工具/函数/音频/拒绝对象、错误JSON、
不支持finish、自相矛盾的id/choices、缺失DONE等均失败，不自动降级为另一次非流式POST。

## 取消、错误和计费

HTTP/TLS沿用禁环境代理、禁重定向、开启证书/主机验证，只允许HTTPS或开发回环HTTP。
首次perform前同时通过领域形状检查和curl_url解析，畸形/超范围端口、URL内嵌凭据及查询在
dispatch前拒绝，不把已知坏URL交给网络后再误当未知执行。
secret只在内存Authorization头，不出现在结果、日志、错误、SSE事件或持久存储。
libcurl multi循环最长每20ms检查stop_token和单调deadline，上游不再发字节时也可取消；
不存在分离watchdog线程。非阻塞sink必须遵守端口约定，不能以未知阻塞回调扩大HTTP deadline。

- 非法请求、默认不支持、预先取消及curl配置/分配失败，在首次传输前已知未dispatch，
  DefiniteFailure、may_have_billed=false、usage已知0。预先取消的termination为Cancelled。
  输入/能力错误的recovery为CorrectInput，未dispatch依赖故障为Backoff，Internal/取消为Diagnose；
  dispatch后的未知结果始终QuerySameIdentity，不自动重调Provider。
- 首次curl perform前设置provider_dispatched，随后网络/TLS/解析/超限/背压失败保守Unknown、
  may_have_billed=true。provider_dispatched是“已开始可能送达”，不是已获得HTTP响应的证明。
- 明确HTTP400/422→INVALID_ARGUMENT；401/403→UPSTREAM_UNAVAILABLE；404/405/410→
  CAPABILITY_NOT_SUPPORTED；429→RATE_LIMITED并保留有界整数Retry-After；其余4xx沿已知拒绝。
  这些拒绝无文本、已知未计费。5xx/3xx/超时/畸形200不能套用此未计费语义。
- 超时是TIMEOUT；取消/连接/解析/背压一般UPSTREAM_UNAVAILABLE，unsupported文本形状为
  CAPABILITY_NOT_SUPPORTED，但dispatch后仍Unknown且可能计费。失败携带既有Failure的
  acceptance/recovery；上游自由文本不透传。
- 已准入文本后没有完整COMPLETED则partial=true；0字节失败不意味着未执行。length/filter虽
  是KnownSucceeded的Provider执行事实，文本仍不完整，不能触发QQ默认完整一次发送。
- usage仅来自合法权威最终chunk。未知时绝不按字符估算；收到合法最终usage后又失去传输时可
  保留usage事实，调用执行/交付仍按其未知状态处理。成功可能计费不等于已核对实际账单。

## 验收命令与证据限制

```bash
python3 scripts/quality/check-dependency-lock.py
python3 scripts/quality/tests/dependency_lock_test.py
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked -R '^p11_streaming_' --no-tests=error --output-on-failure
ctest --preset full-locked -R '^p11_model_http_provider$' --no-tests=error --output-on-failure
```

共享configure/build由主代理串行调度。新增测试包括生产包装层18项/337种分段、UTF-8与有界粘性
错误、上游9项与fuzz smoke、真实loopback HTTP的完成前增量屏障、HTTP错误/半流/usage/截断/
预先及在途取消/250ms背压/timeout/输入拒绝/默认不支持。它们不访问外部付费Provider或QQ。
本切片尚不证明跨进程Model/Runtime/Bridge/SDK流闭环、撤权或PG txn2恢复；这些必须另有生产链证据。

2026-11-04第一轮本机锁定构建由主代理执行，所请求Provider/parser/上游测试目标均编译通过；
`ctest --preset full-locked -R '^p11_streaming_|^p11_model_http_provider$' --no-tests=error --output-on-failure`
实际13/13通过，12.17s，其中生产parser0.52s、stream HTTP 2.27s、旧非流式Provider2.93s。
记录为 `build/logs/capability-expansion/build-ws-str.log`、`ctest-str-provider-first.log`。
该构建首轮有上游测试重复链接parser的警告，CMake已改为单一传递链接；重新生成的全部上游测试
link命令逐一确认parser归档只出现一次。其行为未改，不在下述有限复跑中重复上游10项。
独立锁校验及5项锁突变/坏归档测试通过，18项/337分段已迁为可追踪的生产wrapper回归。

随后补齐URL预解析、未dispatch recovery分类及无抛出结果移动返回，完整格式检查通过。主代理
重新构建生产parser/stream Provider/旧非流式Provider并有限复跑，实际3/3通过，5.54s：
`build/logs/capability-expansion/build-str-provider-final.log`、`ctest-str-provider-final.log`。
本轮增加的真实HTTP拒绝负例均保持0请求，同fixture随后合法URL恰1请求成功；未知/计费边界未放宽。
许可组件安装到临时build前缀成功，`install-sse-license.log`记录两份文件；安装后的MIT许可SHA256
和provenance JSON均与锁逐项核对一致。没有安装到真实部署路径，也没有发布动作。

## STR-03/04 ModelStore恢复接入（局部验证中）

Model应用共享旧Chat/newStream受理指纹和txn2结算，详情见[Core应用切片](02-core.md)。
ModelCallRepository新增`list_expired_started(cutoff,limit)`，使用既有state/created_at/owner/logical id
列，以最旧created_at及稳定键排序，limit1..100；应用每轮32条，35秒界限由Core决定，PG只应用
给定DB-time cutoff。保存仍用原版本CAS，晚到成功不能覆盖已恢复UNKNOWN。

原`list_unresolved_unknown(now,limit)`端口明确now语义：PG用Core的
kModelUnknownResolutionWindow计算30天cutoff，应用再以model_unknown_resolution_due复核。
此前真实SQL少减30天而fake正确，本切片修正该差异。真实PG的29天保留、30天处置、34/35秒查询
和旧版本CAS拒绝位于typed_work_checks.cpp的Model检查块；没有schema/manifest/journal改动。
ModelWork已直接持有ModelCallRepository，因此不需要修改factory或观察journal转发。

新增纯应用CTest `p11_streaming_model_application`，与旧`p11_model_gateway_application`共同回归。
独立clang++编译和纯应用运行用于快速定位；真实PG及锁定CMake门禁待主代理统一运行，不以这项
局部结果宣称RPC/Runtime/Bridge/SDK或真实受管插件流已通过。

2026-11-04 Model应用/真实PG首轮由主代理执行：`p11_model_gateway_application`、
`p11_streaming_model_application`、`p03_postgres_database`共3/3通过，总25.10s，记录
`build/logs/capability-expansion/ctest-model-app-pg-first.log`。此证据涵盖应用与真实ModelStore恢复，
RPC/Runtime/Bridge/SDK仍分切片验收，不能等同Provider到SDK闭环已经启用。
