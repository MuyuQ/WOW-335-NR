// 应用图标资源 ID：由 control_center.rc 编译为 .res 并链接进 exe。
// 同一宏同时被 C++（cc_main.cpp，LoadIconW/LoadImageW）和资源脚本（control_center.rc）使用。
// 说明：资源编译器（rc.exe）的预处理器对 #ifndef 支持不稳定，这里保持单一定义、不加头文件守卫；
// 本文件只在各编译单元包含一次，不会触发重定义。
#define IDI_CONTROL_CENTER_ICON 101
