# Third-Party Notices

KiaraBot 本身的代码采用 MIT 许可证，许可证文本见 [LICENSE.md](LICENSE.md)。当前仓库不包含下列组件的源码；它们由部署环境单独安装或由独立适配器进程使用，并保持各自项目的名称、版权和许可证。

## NoneBot2

- 项目：NoneBot2
- 用途：机器人框架、事件分发和插件运行
- 许可证：MIT License
- 项目地址：https://github.com/nonebot/nonebot2
- 许可证地址：https://github.com/nonebot/nonebot2/blob/master/LICENSE
- 分发方式：作为 Python 依赖单独安装，不复制到 KiaraBot 源码中

使用或分发 NoneBot2 时，应保留其原始版权和 MIT 许可证声明。实际使用版本应记录在项目依赖文件中。

## nonebot-adapter-onebot

- 项目：NoneBot2 OneBot Adapter
- 用途：连接 NoneBot2 与 OneBot V11 协议端
- 许可证：MIT License
- 项目地址：https://github.com/nonebot/adapter-onebot
- 许可证地址：https://github.com/nonebot/adapter-onebot/blob/master/LICENSE
- 分发方式：作为 Python 依赖单独安装，不复制到 KiaraBot 源码中

使用或分发该适配器时，应保留其原始版权和 MIT 许可证声明。实际使用版本应记录在项目依赖文件中。

## NapCatQQ

- 项目：NapCatQQ
- 用途：QQ 协议端，负责登录 QQ 并通过 OneBot V11 提供消息事件和 API
- 许可证：Limited Redistribution License for NapCat
- 项目地址：https://github.com/NapNeko/NapCatQQ
- 许可证地址：https://github.com/NapNeko/NapCatQQ/blob/main/LICENSE
- 分发方式：由用户单独安装和配置；不包含在 KiaraBot 源码、安装包或默认 Docker 镜像中

NapCatQQ 的许可证不是 MIT 或 Apache 等宽松许可证。KiaraBot 不复制、修改或重新发布 NapCatQQ 源码，也不将 NapCatQQ 重新授权为 MIT。需要分发 NapCatQQ、基于其源码修改，或将其打包进商业产品时，应先阅读对应版本的完整许可证并取得必要授权。

NapCatQQ 的安装、QQ 账号使用以及相关平台规则由使用者自行负责。KiaraBot 只通过 OneBot 接口与独立运行的 NapCatQQ 通信。

## AI 插件和其他依赖

AI 插件、模型 SDK、数据库驱动及其他运行时依赖必须按照各自项目的许可证单独记录。添加新依赖时，请同步更新本文件，至少包含：

1. 项目名称和准确版本；
2. 项目地址和许可证地址；
3. 在 KiaraBot 中的用途；
4. 是否随源码、安装包或容器一起分发。

如果某个插件的许可证与 KiaraBot 的分发方式不兼容，应在集成前先确认授权条件。
