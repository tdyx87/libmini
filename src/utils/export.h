#ifndef LIBMINI_EXPORT_H
#define LIBMINI_EXPORT_H

// DLL 导出宏。独立成头的原因：libmini.h 是伞头，模块头反向包含它会形成
// include 循环（消费者先包含模块头时，伞头展开到模块头自身处被 include
// guard 挡住，宏拿不到），故把宏拆到无依赖的本头文件，供各模块头标注
// LIBMINI_API。与 libmini.h 原有定义完全一致（libmini.h 已改为引用本头）
#ifndef LIBMINI_STATIC
#ifdef LIBMINI_EXPORTS
#define LIBMINI_API __declspec(dllexport)
#else
#define LIBMINI_API __declspec(dllimport)
#endif
#else
#define LIBMINI_API
#endif

#endif  // LIBMINI_EXPORT_H
