# P04 全局会话容量端口

目标Core内部SessionSecurity仓储。既有唯一OpenAPI规定最多4096未过期认证会话，
maximum_sessions_per_subject不能代表全库上限，不能让应用扫描所有主体或以缓存0替代。
新增count_unexpired纯查询，返回当前UoW内、实际DB UTC下未撤销且refresh未到期的全库
数量。全局常量4096来源于既有API；新会话最终事务检查，原成功重放先行，满额RateLimited。

文档影响：同步doc02及本记录。落实既有API容量，无新wire字段/状态/HTTP接口，不改
proto/API/doc01；具体PG计数和实际边界测试在独立实现能力同步，doc18只读。
公开头独立C++20 -Wall/-Wextra/-Wpedantic/-Werror语法编译、format16、inner/边界与
diff通过。新纯方法必须与PG/测试实现一起整合，不以声明代替真实全局计数验收。
