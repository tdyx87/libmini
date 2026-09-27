#ifndef LIBMINI_EXPORT_H
#define LIBMINI_EXPORT_H

// DLL 导出宏。独立成头的原因：libmini.h 是伞头，模块头反向包含它会形成
// include 循环（消费者先包含模块头时，伞头展开到模块头自身处被 include
// guard 挡住，宏拿不到），故把宏拆到无依赖的本头文件，供各模块头标注
// LIBMINI_API。
//
// 平台划分必须在这里做（run #6/#8 教训）：__declspec 是 MSVC/MinGW 的
// 关键字，Linux/macOS 的 GCC 完全不认识——shared 构建下 LIBMINI_API
// 若展开为 __declspec(...) 会直接编译失败。非 Windows 平台
// 用 GCC/Clang 的 visibility 属性达成同等导出语义（-fvisibility=hidden
// 未开启时属性可省，显式标注对 so 的导出面是幂等的）。
#if defined(_WIN32)

#ifndef LIBMINI_STATIC
#ifdef LIBMINI_EXPORTS
#define LIBMINI_API __declspec(dllexport)
#else
#define LIBMINI_API __declspec(dllimport)
#endif
#else
#define LIBMINI_API
#endif

#else  // 非 Windows（Linux / macOS）

#if !defined(LIBMINI_STATIC) && defined(LIBMINI_EXPORTS)
#define LIBMINI_API __attribute__((visibility("default")))
#else
#define LIBMINI_API
#endif

#endif

#endif  // LIBMINI_EXPORT_H
