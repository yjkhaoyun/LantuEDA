# 编译
    确保顶部环境是 x64-Release
    顶部菜单 生成 (Build) -> 全部生成 (Build All)。

# 打包安装包
    （https://jrsoftware.org/isdl.php），下载并安装 Inno Setup 7。

右键点击 CMakeLists.txt，选择 删除缓存并重新配置（或者点 配置 kicad）。

# 一键打包命令
    cmake --build out/build/x64-Release --target install ; cmake --build out/build/x64-Release --target create_installer

# 安装包位于 kicad\out\build\x64-Release\installer_output\




