# CI/CD 流程方案

## 责任

CI/CD 层负责把代码变更安全地转换为可验证、可追踪、可回滚的部署版本。

它负责：

- 在 Pull Request 中编译、测试和检查协议兼容性；
- 在合并到 `main` 后生成可追踪的构建产物；
- 将镜像发布到 GitHub Container Registry（GHCR）；
- 自动部署 staging，并在生产部署前执行审批；
- 执行迁移、健康检查、冒烟测试和失败回滚；
- 记录提交、镜像、配置、迁移和部署结果之间的对应关系。

它不负责：

- 在 workflow 中实现业务逻辑；
- 把密钥写入 Git 或 Docker 镜像；
- 绕过 Core、适配器或迁移命令直接修改业务数据；
- 使用不可追踪的 `latest` 镜像部署生产环境。

## 第一版技术选型

第一版使用 GitHub Actions、GHCR、Docker Compose 和一台 Linux 服务器：

```text
Pull Request
    │
    ▼
GitHub Actions CI
编译 · 测试 · 协议检查 · 静态检查
    │
    ├── Pull Request：只验证，不部署
    └── main：构建镜像并部署 staging
                         │
                         ▼
                   冒烟测试
                         │
                         ▼
                vX.Y.Z + production 审批
                         │
                         ▼
                    生产部署
```

GitHub Actions workflow 放在 `.github/workflows/` 中。staging 和 production 使用独立的 GitHub Environment；production Environment 配置 required reviewers，避免未经审批的生产发布。参考：[GitHub Actions Workflows](https://docs.github.com/en/actions/concepts/workflows-and-actions/workflows) 和 [Deployment environments](https://docs.github.com/en/actions/concepts/workflows-and-actions/deployment-environments)。

单机阶段使用 Docker Compose 足够。未来需要多节点调度、自动扩缩容或跨区域部署时，再迁移到 Kubernetes，不改变镜像、迁移、健康检查和发布版本规则。

## 仓库要求

当前仓库已经具备 C++20、CMake、CTest、Core smoke test 和 Protobuf 契约。工具版本和验证命令见 [开发工具链版本](11-toolchain.md)。CI 必须至少覆盖：

| 检查 | 要求 | 失败处理 |
|---|---|---|
| C++ 编译 | GCC 13+ 和 Clang 16+ | 阻止合并 |
| Core 测试 | `ctest` 全部通过 | 阻止合并 |
| 协议生成 | `protoc`、Protobuf runtime 和 gRPC plugin/runtime 的兼容性及实际生成 | 阻止协议变更合并 |
| Descriptor 检查 | 检查字段编号和服务签名 | 阻止不兼容变更 |
| 格式和静态检查 | `clang-format`、`clang-tidy` | 阻止合并 |
| OpenAPI 检查 | YAML 解析、Redocly lint、错误/幂等头和 operationId 完整性 | 阻止合并 |
| AsyncAPI 检查 | YAML 解析、AsyncAPI 2.x parser、频道认证和消息唯一性 | 阻止合并 |
| 镜像扫描 | secret scan、漏洞扫描、SBOM | 阻止正式发布 |

当前 Core 仍是最小骨架，生产 CD 还需要实现真实进程、迁移命令和 `/health/live`、`/health/ready` 接口。CI 可以立即建立，生产部署应在这些接口完成后启用。

## 分支、版本和触发规则

```text
feature/*  → Pull Request → main
main       → 自动部署 staging
v0.1.0     → 生产发布候选
v1.0.0     → 正式生产版本
```

- `main` 禁止直接 push；
- Pull Request 必须通过 CI，并至少由一名 reviewer 批准；
- 每次构建都使用 Git commit SHA 作为镜像标签；
- 正式版本使用语义化版本号，例如 `v1.2.0`；
- 生产部署只接受正式 tag 对应的镜像；
- 修复版本递增 patch，向后兼容功能递增 minor，不兼容协议变更递增 major。

推荐的镜像标签：

```text
ghcr.io/leekiara1215/kiarabot-core:git-<commit-sha>
ghcr.io/leekiara1215/kiarabot-core:v1.0.0
```

## CI 流程

新增 `.github/workflows/ci.yml`，在 Pull Request 和 push 到 `main` 时执行：

```yaml
name: ci

on:
  pull_request:
  push:
    branches: [main]

permissions:
  contents: read

concurrency:
  group: ci-${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build-test:
    name: ${{ matrix.name }}
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: gcc
            cc: gcc-13
            cxx: g++-13
          - name: clang
            cc: clang
            cxx: clang++

    env:
      CC: ${{ matrix.cc }}
      CXX: ${{ matrix.cxx }}

    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ${{ matrix.cxx }}

      - name: Configure
        run: cmake --preset dev

      - name: Build
        run: cmake --build --preset dev --parallel

      - name: Test
        run: ctest --preset dev
```

协议依赖锁定后增加独立的 `contract-check` job：

```bash
mkdir -p build
protoc \
  --descriptor_set_out=build/kiarabot-v1.pb \
  --include_imports \
  -I proto \
  proto/kiarabot/v1/*.proto
```

协议变更必须比较当前 descriptor 与基线，确认没有复用字段编号、删除未废弃字段或改变服务签名。生成文件只能写入 `build/generated/`，不能提交生成源码。

控制面契约检查还必须运行：

```bash
ruby -e "require 'yaml'; YAML.load_file('api/openapi.yaml'); YAML.load_file('api/asyncapi.yaml')"
npx --yes @redocly/cli@latest lint api/openapi.yaml
# 使用锁定版本的 @asyncapi/parser 校验 api/asyncapi.yaml，并拒绝 severity=0 的诊断
```

## 构建镜像

新增：

```text
Dockerfile
.dockerignore
deploy/compose.staging.yml
deploy/compose.prod.yml
```

Dockerfile 使用多阶段构建：第一阶段安装编译工具并构建，第二阶段只复制运行时文件，并使用非 root 用户启动服务。镜像中不能包含 `.env`、SSH key、API Key 或完整 Git 历史。

CI 在 `main` 或正式 tag 上执行：

1. 使用 commit SHA 构建镜像；
2. 运行容器级 smoke test；
3. 生成 SBOM；
4. 执行 secret scan 和漏洞扫描；
5. 推送到 GHCR；
6. 保存镜像 digest 作为部署记录。

## 部署环境

### staging

`main` 合并后自动部署 staging。staging 使用独立数据库和独立密钥，部署完成后执行：

```text
数据库迁移
Core live 检查
Core ready 检查
适配器注册检查
OpenAPI health 检查
最小消息或 Action 冒烟测试
```

### production

正式 tag 创建后进入 production 部署：

1. 读取当前运行版本并保存为 rollback 版本；
2. 备份数据库、schema 版本和备份校验值；
3. 执行可重复的 `kiarabot-migrate`；
4. 拉取指定 SHA 或正式 tag 的镜像；
5. 启动服务并等待 ready；
6. 执行冒烟测试；
7. 写入部署记录；
8. 检查失败时恢复上一个镜像版本。

Compose 服务顺序：

```text
kiarabot-migrate
        ↓
kiarabot-core
        ↓
kiarabot-adapter-nonebot
        ↓
kiarabot-panel
```

Compose 应使用 `healthcheck` 和依赖条件等待服务真正 ready；Docker 官方文档说明，Compose 可以根据依赖服务的健康检查结果决定是否启动后续服务。参考：[Compose services](https://docs.docker.com/reference/compose-file/services/)。

迁移服务示例：

```yaml
services:
  migrate:
    image: ghcr.io/leekiara1215/kiarabot-core:${IMAGE_TAG}
    command: ["/usr/local/bin/kiarabot-migrate"]
    env_file: [.env]

  core:
    image: ghcr.io/leekiara1215/kiarabot-core:${IMAGE_TAG}
    command: ["/usr/local/bin/kiarabot-core"]
    env_file: [.env]
    restart: unless-stopped
    depends_on:
      migrate:
        condition: service_completed_successfully
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:8080/health/ready"]
      interval: 10s
      timeout: 3s
      retries: 6
```

## 密钥和权限

GitHub Secrets 只保存部署所需的最小凭据：

```text
STAGING_HOST
STAGING_USER
STAGING_SSH_KEY
STAGING_KNOWN_HOSTS
PROD_HOST
PROD_USER
PROD_SSH_KEY
PROD_KNOWN_HOSTS
```

应用密钥保存在服务器的 `.env` 或 Secret Manager，不进入 Git。云环境优先使用 OIDC 短期凭证，避免长期云访问密钥写入 GitHub Secrets。参考：[GitHub OpenID Connect](https://docs.github.com/en/actions/concepts/security/openid-connect)。

部署 job 只能获得当前环境所需的权限，CI job 不应获得生产密钥或生产服务器访问权限。

## 回滚规则

发布前必须保留：

```text
上一镜像 digest
当前数据库 schema 版本
迁移前数据库备份
部署 commit SHA
配置版本
```

以下情况触发回滚：

- migration 失败；
- Core 未进入 ready；
- 适配器无法注册；
- 冒烟测试失败；
- 错误率或延迟超过发布阈值。

数据库迁移必须向后兼容，采用 expand/contract 方式，避免回滚应用时旧版本无法读取新 schema。破坏性迁移应拆成多个发布步骤，不能和不可逆的数据删除放在同一次部署中。

## 实施顺序

### 第 1 步：建立 CI

- 创建 `.github/workflows/ci.yml`；
- 配置 GCC、Clang、CMake 和 CTest；
- 加入格式、YAML、OpenAPI 和 secret 检查；
- 设置 `main` 分支保护。

### 第 2 步：锁定协议和依赖

- 增加 `vcpkg.json` 或 Conan lockfile；
- 锁定 Protobuf、gRPC 和 `grpc_cpp_plugin` 的来源与 release 系列；
- 增加 descriptor 基线；
- 增加协议兼容性 job。

### 第 3 步：建立 staging CD

- 增加 Dockerfile；
- 增加 GHCR 推送 job；
- 增加 staging Compose；
- 实现 Core live/ready 检查；
- 增加 staging 冒烟测试。

### 第 4 步：建立 production CD

- 配置 production Environment 和 reviewer；
- 实现 `kiarabot-migrate`；
- 增加数据库备份和校验；
- 增加正式 tag 发布；
- 实现失败回滚。

### 第 5 步：补齐可观测性

- 结构化日志包含 `request_id`、`trace_id`、`event_id` 和 `action_id`；
- 记录部署版本和镜像 digest；
- 监控 live、ready、错误率、延迟和迁移结果；
- 对部署失败、服务未 ready 和备份失败告警。

## 验收标准

- 任意 Pull Request 都能自动编译和运行测试；
- GCC 和 Clang 构建结果都通过；
- Protobuf 不兼容变更会阻止合并；
- 合并到 `main` 会生成带 SHA 的镜像；
- staging 可以自动部署并完成冒烟测试；
- production 发布必须经过审批；
- 迁移失败会阻止 Core 启动；
- 服务未 ready 或冒烟失败可以恢复上一个版本；
- 生产密钥不会出现在代码、日志和镜像中；
- 每次生产部署都能追溯到 Git commit、镜像 digest、配置版本和 schema 版本。

## 代码启动句

> 请只实现 KiaraBot 的 GitHub Actions CI、GHCR 镜像构建、Docker Compose staging/production 部署、迁移前备份、live/ready 健康检查、冒烟测试和失败回滚；遵守 `doc/08-operations.md` 的进程顺序和健康检查语义，不修改 Core、协议、插件或适配器的业务规则。
