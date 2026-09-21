# conanfile.py：用条件依赖替代纯 [requires] 声明式文件。
# 原因：aes_gcm 的 AES-GCM 实现 Windows 走系统 CNG（bcrypt，无第三方依赖），
# POSIX 走 OpenSSL EVP——只有非 Windows 平台才需要 openssl 包。
# 纯文本格式无法按平台写条件依赖，只能全平台拉取，会让 Windows CI
# 触发无谓的 openssl 源码编译（MSVC 静态 runtime 无预编译包，且要 nasm）。
import os

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class LibminiConan(ConanFile):
    name = "libmini"
    version = "1.0.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("zlib/1.3.1")
        self.requires("gtest/1.15.0")
        self.requires("nlohmann_json/3.11.3")
        self.requires("pugixml/1.14")
        self.requires("spdlog/1.14.1")
        self.requires("cpp-httplib/0.28.0")
        self.requires("sqlite3/3.46.1")
        if self.settings.os != "Windows":
            self.requires("openssl/3.0.17")

    def configure(self):
        # 只需要 sqlite 库本体：shell.c 在中文 locale 的 MSVC 下按 GBK 读
        # UTF-8 源码直接编译失败（C2001），与旧 conanfile.txt 的 [options]
        # 约定一致
        self.options["sqlite3/*"].build_executable = False

    # 不用 cmake_layout：生成器直接落在 -of 目录（deps/conan_toolchain.cmake），
    # 与 CI/文档里既有的工具链路径约定一致
    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()
