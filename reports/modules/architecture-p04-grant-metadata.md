# P04 grant 元数据投影

目标Core纯领域；在既有project_assignment校验后生成继承ResourceGrant的元数据视图。
Role分支每个exact resource只展开Core已批准default_role_allows的封闭权限，显式分支
保留原tuple；permission字符串唯一来源kPermissionNames。id/subject/version/单role保留，
不生成占位permission，不把多个Assignment混为笛卡尔积；损坏数据Internal。

文档影响：同步doc02及本记录。此内部投影不改变wire字段/错误/状态，proto/API/doc01
无需更新，PG具体仓储接线在独立能力同步doc05。doc18只读。
独立C++20 -Wall/-Wextra/-Wpedantic/-Werror编译执行新投影测试，验证两个exact scope、
默认role不含敏感权限、显式MessageRead保持精确tuple、非product拒绝；format16、
边界/inner与diff通过。CMake正式注册和真实PG由构建/基础设施能力跟进，不冒充生产验收。
