KMBoxDetect 自写驱动 - VM 测试包 (v2)
======================================

文件:
  vm_kdbg.exe           - 一键准备: 测试签名 + 内核调试 + 装驱动
  usb_filter.sys        - 自写 KMDF 内核驱动 (USB 上层过滤驱动, 已测试签名)
  usb_filter.inf        - 驱动安装 INF
  usb_client.exe        - 用户态客户端 (枚举全部 USB 设备 + 固件行为指纹)
  kmboxdetect_test.cer  - 测试签名证书 (可选导入)

前提:
  1. 虚拟机 (VMware Workstation, 当前: Windows10x64-1903)
  2. 虚拟机是 64 位 Windows 10/11
  3. 有真实 USB 设备接入 (鼠标/键盘/U 盘/WinUSB 设备均可)

----------------------------------------------------------------------
一、驱动架构 (v2 变更)
----------------------------------------------------------------------

v1 是"功能驱动", 靠 INF 绑定到 USB\VID_6688&PID_2021&MI_01 一个设备,
没插 KMBox 就没有设备可绑, 客户端也就找不到。

v2 改为"USB 上层过滤驱动":
  - WdfFdoInitSetFilter(DeviceInit)  -> 不占用设备, 旁挂到 USB 设备栈
  - 每个 USB 设备注册一个 GUID_DEVINTERFACE_USBFILTER 设备接口
  - 客户端枚举接口 -> 拿到全部已插入的 USB 设备
  - 注册了 EvtDriverUnload -> 支持热卸载 (sc stop 不再需要重启)

注册方式 (INF 类安装被微软限制, 直接写注册表):
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{88BAE032-5A81-49F0-BC3D-A4FF138216D6}" /v UpperFilters /t REG_MULTI_SZ /d usb_filter /f
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{745A17A0-62D0-46D7-BA83-4922AF6C3C1F}" /v LowerFilters /t REG_MULTI_SZ /d usb_filter /f

  类 GUID 对照:
    {36FC9E60-C465-11CF-8056-444553540000}  USB 主机控制器/集线器
    {88BAE032-5A81-49F0-BC3D-A4FF138216D6}  USBDevice (WinUSB/通用设备)
    {745A17A0-62D0-46D7-BA83-4922AF6C3C1F}  HIDClass (鼠标/键盘/KMBox)
    {4D36E967-E325-11CE-BFC1-08002BE10318}  DiskDrive (U 盘，风险高)

  注意: 改 UpperFilters/LowerFilters 后必须重启, 已存在的设备才会重新枚举。

----------------------------------------------------------------------
二、指纹方案 (v2 核心)
----------------------------------------------------------------------

思路: 用固定数量的控制传输探针去"问"设备, 把设备的响应记成转录,
      排序去重后算 SHA256 —— 同一个固件得到同一个哈希。

五个关键设计 (缺一个就会导致"同款设备不同指纹"):

  [1] 探针只走 IN 方向 (bmRequestType & 0x80)
      不发写请求。SET_CONFIGURATION(0)/SET_INTERFACE/CLEAR_FEATURE
      这类请求会改变设备状态, 让后续探针结果不可复现。

  [2] 错误码映射成粗分类
      GEN_FAIL(31) / INVALID_PARAM(87) / TIMEOUT(121) /
      DISCONNECTED(1167) / NOMEM / ABORTED / OTHER
      不把原始 GetLastError 放进哈希 —— 它随主机控制器和 USB 栈变化,
      跨芯片组会不一样。

  [3] 描述符进哈希
      设备描述符(0x01) + 配置描述符(0x02) + HID 报告描述符(0x22)
      + 字符串描述符(0x03) —— 这是设备身份最稳定的部分。

  [4] 探针参数随机抽样 (数量固定)
      默认 512 个探针, 每次随机选 bmRequestType/bRequest/wValue/wIndex。
      因为哈希侧是排序去重的, 随机顺序不影响哈希值,
      但能让攻击者无法预先录制全部组合来克隆。

  [5] 输出基线键
      VID_xxxx&PID_xxxx&REV_xxxx&USB_xxxx
      其中 REV 就是设备描述符里的 bcdDevice (固件版本)。
      指纹要按这个键建对照表, 否则跨固件版本会误报。

----------------------------------------------------------------------
三、客户端命令
----------------------------------------------------------------------

  usb_client list                    列出所有 USB 设备接口
  usb_client scan                    扫描全部设备, 输出基线键 + 指纹
  usb_client scan --n 1024           指定探针数量 (默认 512)
  usb_client scan --verbose          打印转录预览 (调试用)
  usb_client fuzz [--full]           只做随机 fuzz, 不计算指纹
  usb_client diff                    只做边界值 diff

  scan 输出示例:
    [0] \\?\usbstor#disk&ven_...
          基线键: VID_046D&PID_C077&REV_2301&USB_0200
          条目数: 1372
          指纹:   9f2c...e41a

----------------------------------------------------------------------
四、验证方法 (用之前必须先做)
----------------------------------------------------------------------

同一只设备:
  [1] 连续扫描 10 次         -> 指纹必须完全相同
  [2] 换一个 USB 端口        -> 指纹必须相同
  [3] 换一台电脑(不同芯片组) -> 指纹必须相同   <- 依赖 [2] 的错误码粗分类
  [4] 同款设备另一只         -> 指纹应该相同
  [5] 不同型号设备           -> 指纹必须不同

只要有一项不通过, 指纹就不能用于判定。

----------------------------------------------------------------------
五、KMBox 检测逻辑
----------------------------------------------------------------------

KMBox 接法: 鼠标插 KMBox, KMBox 插电脑。主机看到的是 KMBox, 不是鼠标。

  指纹匹配已知型号          -> 真鼠标, 或完美克隆的 KMBox
  指纹不匹配任何已知型号    -> 高度可疑 (新固件/模拟设备)
  指纹匹配但设备树异常      -> 可疑 (鼠标下面还挂 hub)
  指纹匹配但厂商协议无响应  -> 可疑 (KMBox 不实现 HID++/Razer 协议)

单一指纹不够, 要三维交叉: 指纹 + 设备树 + 厂商协议。

----------------------------------------------------------------------
六、安装步骤
----------------------------------------------------------------------

  1. 编译
     msbuild usb_filter.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false
     cl /nologo /O2 /utf-8 usb_client.c setupapi.lib

  2. 签名 (用自己的测试证书)
     signtool sign /f <自己的>.pfx /p <密码> /fd SHA256 bin\x64\Release\usb_filter.sys

  3. 拷 deploy 到 VM

  4. VM 里装证书 + 起服务
     certutil -addstore -f Root <证书>.cer
     certutil -addstore -f TrustedPublisher <证书>.cer
     sc create usb_filter type= kernel start= demand binPath= C:\Windows\Temp\deploy\usb_filter.sys
     sc start usb_filter

  5. 注册过滤器 (见第一节) + 重启

  6. 跑 usb_client.exe scan

----------------------------------------------------------------------
七、注意事项
----------------------------------------------------------------------

  - vm_kdbg.exe 需要管理员权限
  - 测试证书是自签的, 只用于测试; 正式发布需 WHQL 签名
  - 内核调试生效需重启
  - 过滤驱动跑在内核态, 出 bug 会导致设备类失效:
      HIDClass   -> 鼠标键盘失灵
      DiskDrive  -> 系统盘无法访问 (蓝屏)
    USBDevice 相对安全, 建议先只加这一类验证
  - 已提供 EvtDriverUnload, sc stop 可直接卸载, 换驱动不需要重启
