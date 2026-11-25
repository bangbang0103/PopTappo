# PopTappo

用于 Windows 10 / 11 x64 的鸣潮桌宠托盘助手。使用 Win32 和 Windows UI Automation，提供单个便携 EXE。

## 使用

1. 启动 Wuwa Tappo，让自己的桌宠显示在桌面上。
2. 双击 `PopTappo.exe`，确认 Windows 管理员权限提示。桌宠以管理员权限运行，助手需要同等权限访问它的界面。
3. 在任务栏通知区域找到 PopTappo，右键或左键点击图标打开原生菜单。图标可能收在右下角的「^」内。
4. 分别勾选「自动连点」和「自动开箱」。菜单打开时约每秒刷新本次发送次数、桌宠累计计数、宝箱状态和确认开箱数量。连点在菜单打开期间暂缓发送，关闭菜单后继续。重新启动助手后，两项自动操作均为暂停。

自动连点默认发送 F24，速度默认为每秒 10 次。它不会移动鼠标或主动切换焦点，并在检测到键盘操作、鼠标按键或系统菜单时让出输入。F21–F24 仍属于系统键盘输入。如果前台应用绑定了所选功能键，可切换模拟按键或暂停连点。「本次发送」是助手成功提交给 Windows 的次数，实际计数以桌宠界面为准。

在「连点速度」中可选择 5、10、20 次/秒，或通过「自定义」填写 1–20 之间任意整数。这个数字是目标频率，操作避让和系统调度可能降低实际发送频率。

自动开箱读取桌宠无障碍树中的按钮状态。每次只调用一枚就绪宝箱，并在检测到冷却状态或奖励动画后记录为已确认。菜单按从左到右的顺序显示检测到的宝箱。

## 挂机与电源状态

「防自动睡眠」在检测到桌宠且有自动任务运行时向 Windows 请求保持系统唤醒。按 Win+L 锁屏后，已启用的「自动开箱」会继续读取桌宠宝箱。锁屏前同时勾选「自动开箱」和「防自动睡眠」，助手会在锁屏期间继续请求系统保持唤醒。连点在解锁后恢复。

| Windows 状态 | 连点 | 开箱 |
| --- | --- | --- |
| 桌面已解锁且显示器开启 | 运行，键鼠操作时避让 | 运行 |
| 显示器关闭、系统仍唤醒且桌面可访问 | 暂停，避免按键点亮屏幕 | 桌宠界面可读取时继续检查 |
| Win+L 锁屏、系统仍唤醒 | 暂停 | 桌宠界面可读取时继续检查 |
| 断开桌面会话 | 暂停 | 暂停 |
| 系统睡眠 S3 / 休眠 S4 | 系统恢复后继续 | 系统恢复后继续 |

Windows 在 S3 或 S4 期间暂停桌面进程。手动睡眠、合盖动作及低电量策略仍由系统处理。[Microsoft 电源状态说明](https://learn.microsoft.com/en-us/windows/win32/power/system-power-states)

## 构建

使用 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载、Windows 10/11 SDK 和 CMake 3.21+。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

便携产物为 `dist\PopTappo.exe`。Release 使用静态 C++ 运行库，可以直接复制该 EXE。

GitHub Actions 会在推送到 `main` 或提交指向 `main` 的 Pull Request 时构建 Windows x64 版本，也可手动运行 `Windows build`。构建成功后，在运行记录的 **Artifacts** 中下载 `PopTappo-Windows-x64`。

如需排查桌宠识别，可运行只读诊断。结果包括窗口、UI Automation、计数及宝箱状态。

```powershell
Start-Process .\PopTappo.exe -Wait -ArgumentList '--diagnose "C:\可写目录\tappo-status.txt"'
```

实现思路参考 [WuwaTappoHelper](https://github.com/rnpfdl/WuwaTappoHelper) 和 [AutoTappo](https://github.com/To0Ru/AutoTappo)。
