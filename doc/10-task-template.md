# 可执行编码任务模板

“代码启动句”说明方向；真正开始编码时使用下面模板，把规格冻结到文件、命令和验收条件。

```markdown
# 任务：<模块>/<功能>

## 目标
一句话说明要交付的可运行能力。

## 允许修改
- <明确文件或目录>

## 禁止修改
- <明确目录>
- 不改变已发布的 proto 字段编号
- 不增加跨层反向依赖

## 契约
- Protobuf：<文件和 message/service>
- OpenAPI：<路径和 schema>
- AsyncAPI：<频道、首帧或消息 envelope；不涉及则写“不涉及”>
- 输入、输出和稳定错误码：<完整列表>
- HTTP/gRPC 状态、健康/生命周期状态：<映射和完整列表；不涉及则写“不涉及”>

## 文档同步
- 必须更新：<契约、模块、运行语义、运维和测试文档路径>
- 无需更新：<已检查但不受影响的文档及理由>
- 唯一来源和更新顺序：<先协议/API，再模块语义，后实现和测试>
- 文档校验命令：<YAML lint、descriptor diff、代码生成或其他命令>

## 运行语义
- 顺序、投递、重试、幂等和超时：<明确数值>

## 依赖和命令
- 编译器与库版本：<版本>
- 生成命令：<完整命令>
- 构建和测试命令：<完整命令>

## 验收标准
- [ ] <正常路径>
- [ ] <故障路径>
- [ ] <边界路径>
```

## 第一批可执行任务

### 协议层

> 只修改 `proto/kiarabot/v1/`、`proto/README.md` 和协议测试；使用 protoc 25.x 与匹配的 gRPC 插件，实现并校验事件、Action、适配器桥、插件桥和错误码；执行 descriptor、C++/Python 代码生成和 round-trip 测试；禁止修改 `core/`、`adapters/`、`panel/`。

### C++ Core 骨架

> 只修改 `core/` 和根目录 CMake 文件；使用 C++20 和 CMake 3.24+，实现无第三方业务 SDK 的 Core 进程、mock adapter、事件 inbox 和 Action dispatcher；执行 `cmake -S . -B build && cmake --build build && ctest --test-dir build`；禁止导入 NoneBot、NapCat、模型 SDK 或面板代码。

### Python 适配器

> 在独立适配器项目中使用 Python 3.11、NoneBot2 和 nonebot-adapter-onebot，实现 `AdapterBridge.Connect`、OneBot V11 消息段转换、Action 执行、重连和能力注册；使用 mock Core 测试；不得修改本仓库的 C++ Core、插件或面板。

### 控制面板 API

> 只修改 `control/` 和 `api/openapi.yaml`，实现 bearer token 认证、refresh token 轮换、RBAC、分页、幂等写请求、统一错误映射和 WebSocket resume；使用 mock Core 和 HTTPS 测试；禁止直接访问 NapCat、数据库表或插件内部状态。
