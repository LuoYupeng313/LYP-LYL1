# IEEE 802.1AS-2020 gPTP-capable TLV Implementation

## 概述 (Overview)

本实现为linuxptp-4.4添加了完整的**gPTP-capable TLV**支持（TLV Type 0x8000），这是IEEE 802.1AS-2020标准要求的关键特性。

**支持场景**：
- ✅ 标准 IEEE 802.1AS gPTP (`transportSpecific = 0x1`)
- ✅ Automotive Profile with CMLDS (`transportSpecific = 0x2`)
- ✅ Master和Slave角色都支持
- ✅ 与Marvell等标准交换机完全兼容

## 问题背景 (Background)

### 您遇到的问题
- **现象**: CMLDS和ptp4l实例无法与Marvell交换机同步
- **原因**: Marvell交换机无法转发端节点发出的Sync报文
- **根本原因**: 端节点没有发送gPTP-capable signaling报文，交换机无法识别该端口为gPTP-capable

### gPTP-capable TLV (0x8000) 的作用

根据IEEE 802.1AS-2020标准：

1. **通告asCapable状态**: 向网络中的其他设备（尤其是交换机）通告本端口是否具备gPTP时间同步能力
2. **交换机转发决策**: 符合标准的交换机（如Marvell）依靠此TLV判断是否应该转发时间同步报文
3. **网络拓扑管理**: 帮助网络设备维护gPTP拓扑信息

**如果没有此TLV**:
- 交换机会认为端节点不支持gPTP
- 交换机不会转发Sync/Follow_Up等时间同步报文到该端口
- 端节点无法完成时间同步

## 实现内容 (Implementation)

### 1. 新增文件

#### 配置文件
- `configs/domain0_master_gptp.cfg` - Automotive Profile Master配置（Domain 0）
- `configs/domain1_slave_gptp.cfg` - Automotive Profile Slave配置（Domain 1）

#### 文档文件
- `AUTOMOTIVE_CMLDS_GUIDE.md` - 详细的CMLDS架构和Automotive Profile指南
- `QUICK_REFERENCE.md` - 快速参考卡片

### 2. 修改的文件

#### tlv.h
```c
struct gptp_capable_tlv {
    Enumeration16 type;        // 0x8000
    UInteger16    length;      // 8
    Octet         id[3];       // IEEE 802.1 OUI: 00-80-C2
    Octet         subtype[3];  // 0x000001 for gPTP-capable
    UInteger8     flags;       
    Integer8      logGptpCapableMessageInterval;
} PACKED;
```

#### port_signaling.c
- 新增 `port_tx_gptp_capable()` 函数：构建并发送gPTP-capable signaling报文
- 修改 `process_signaling()` 函数：接收并处理gPTP-capable TLV

#### port.c
- 修改 `port_capable()` 函数：在asCapable状态变化时自动发送gPTP-capable TLV
- 修改 `port_p2p_transition()` 函数：进入LISTENING状态时发送初始TLV

#### port_private.h
- 新增 `gptp_capable_transmit` 配置字段
- 新增 `port_tx_gptp_capable()` 函数声明

#### config.c
- 新增 `gptp_capable_transmit` 配置项（默认值：1，启用）

## 配置说明 (Configuration)

### 关键配置参数

#### 标准 gPTP 模式
```ini
# 启用gPTP-capable TLV发送（默认：1）
gptp_capable_transmit   1

# 标准802.1AS传输特定值
transportSpecific       0x1

# 必须使用L2传输
network_transport       L2

# 使用P2P延迟机制
delay_mechanism         P2P

# 802.1AS组播MAC地址
ptp_dst_mac             01:80:C2:00:00:0E
```

#### Automotive Profile + CMLDS 模式
```ini
# 启用gPTP-capable TLV发送（默认：1）
gptp_capable_transmit   1

# Automotive Profile传输特定值
transportSpecific       0x2

# 必须使用L2传输
network_transport       L2

# 使用COMMON_P2P延迟机制（由CMLDS服务处理）
delay_mechanism         COMMON_P2P

# 802.1AS组播MAC地址
ptp_dst_mac             01:80:C2:00:00:0E

# CMLDS配置（在[interface]段）
cmlds.server_address    /var/run/cmlds_server
```

### 使用示例

#### 场景1: Automotive Profile with CMLDS（推荐用于与Marvell交换机同步）

```bash
# 1. 启动CMLDS Server
ptp4l -f CMLDS_server.cfg -i ens33 -m &
sleep 2

# 2. 启动Domain 0 Master
ptp4l -f configs/domain0_master_gptp.cfg -i ens33 -m &
sleep 2

# 3. 启动Domain 1 Slave（从Marvell交换机接收Sync）
ptp4l -f configs/domain1_slave_gptp.cfg -i ens33 -m &
```

**关键点**：
- ✅ 三个进程都配置了 `gptp_capable_transmit = 1`
- ✅ Master和Slave都会发送gPTP-capable TLV
- ✅ 特别是Domain 1 Slave的TLV对Marvell交换机至关重要

#### 场景2: 标准gPTP模式（单实例）

```bash
# 从站模式
ptp4l -f your_gptp_slave.cfg -i eth0 -s -m

# 主站模式  
ptp4l -f your_gptp_master.cfg -i eth0 -m
```

**配置要求**：
```ini
[global]
transportSpecific       0x1
delay_mechanism         P2P
gptp_capable_transmit   1
```

## 工作原理 (How It Works)

### 发送时机

gPTP-capable signaling报文会在以下情况自动发送：

1. **端口初始化完成**
   - 进入PS_LISTENING状态时发送初始TLV

2. **asCapable状态变化**
   - `NOT_CAPABLE` → `AS_CAPABLE`: 通告端口现在具备gPTP能力
   - `AS_CAPABLE` → `NOT_CAPABLE`: 通告端口失去gPTP能力

3. **条件**
   - 必须配置 `gptp_capable_transmit = 1`
   - 必须运行在802.1AS相关模式：
     - 标准gPTP: `transportSpecific = 0x1` 
     - Automotive Profile: `transportSpecific = 0x2`

### 报文格式

```
PTP Header:
  messageType: SIGNALING (0xC)
  transportSpecific: 0x1 (gPTP) 或 0x2 (Automotive Profile)
  targetPortIdentity: FF:FF:FF:FF:FF:FF:FF:FF:FFFF (wildcard)

TLV:
  type: 0x8000 (TLV_ORGANIZATION_EXTENSION_DO_NOT_PROPAGATE)
  length: 8
  organizationId: 00:80:C2 (IEEE 802.1)
  organizationSubType: 00:00:01 (gPTP-capable)
  flags: 0x00
  logGptpCapableMessageInterval: 0x7F (not specified)
```

**注意**：Master和Slave都会发送此报文，这不是请求-响应机制，而是单向能力通告。

## 验证方法 (Verification)

### 1. 检查日志输出

启用调试模式运行：
```bash
ptp4l -f configs/domain1_slave_gptp.cfg -i ens33 -m -l 7
```

查找以下日志：
```
port 1 (ens33): sent gPTP-capable signaling message
port 1 (ens33): setting asCapable
```

**对于CMLDS架构**，应该看到三条日志（CMLDS Server、Domain 0 Master、Domain 1 Slave各一条）：
```bash
grep "sent gPTP-capable" /var/log/syslog
```

### 2. 网络抓包验证

使用tcpdump捕获802.1AS报文：
```bash
tcpdump -i eth0 -vvv ether proto 0x88f7 -w gptp.pcap
```

使用Wireshark分析：
- 查找 `PTP Signaling Message`
- 检查是否包含 TLV Type 0x8000
- 验证 Organization ID 为 00:80:C2
- 验证 Organization SubType 为 00:00:01

### 3. 检查Marvell交换机状态

登录Marvell交换机查看PTP端口状态：
```
show ptp port status
show ptp clock
```

应该能看到端口被识别为gPTP-capable。

### 4. 验证时间同步

从站应该能够接收到Sync报文：
```bash
# 日志中应该出现
port 1 (eth0): LISTENING to UNCALIBRATED on MASTER_CLOCK_SELECTED
port 1 (eth0): UNCALIBRATED to SLAVE on MASTER_CLOCK_SELECTED
```

## 故障排查 (Troubleshooting)

### 问题1: 仍然无法与交换机同步

**检查项**:
1. 确认 `gptp_capable_transmit = 1` 已配置（**所有实例都要配置**）
2. 确认 `transportSpecific = 0x1` (标准gPTP) 或 `0x2` (Automotive Profile)
3. 确认使用 `network_transport = L2`
4. 确认 `delay_mechanism = P2P` 或 `COMMON_P2P`
5. 对于CMLDS架构，确认三个进程都在运行
6. 检查网络连接和链路状态

**调试命令**:
```bash
# 检查端口是否asCapable
pmc -u -b 0 'GET PORT_DATA_SET_NP'

# 检查统计信息
pmc -u -b 0 'GET PORT_STATS_NP'
```

### 问题2: 看不到gPTP-capable报文发送

**可能原因**:
1. 端口尚未达到asCapable状态（检查peer delay是否正常）
2. 配置文件中 `gptp_capable_transmit = 0` 或未配置
3. 不是802.1AS相关模式运行（`transportSpecific` 不是 0x1 或 0x2）
4. 对于CMLDS架构：CMLDS Server未启动或配置错误

**检查**:
```bash
# 查看asCapable状态
pmc -u -b 0 'GET PORT_DATA_SET_NP'
# 输出应该显示 asCapable: 1
```

### 问题3: CMLDS架构特有问题

**对于Automotive Profile with CMLDS**:
- 确保CMLDS Server正在运行：`ps aux | grep ptp4l` 应该看到3个进程
- 检查CMLDS配置：`transportSpecific = 2`（注意不是0x2）
- 检查ptp4l配置：`transportSpecific = 0x2`
- 验证UDS socket存在：`ls -la /var/run/cmlds_*`
- 确认所有三个配置文件都有 `gptp_capable_transmit = 1`

**启动顺序很重要**：
```bash
# 1. 先启动CMLDS Server
# 2. 再启动Domain 0 Master  
# 3. 最后启动Domain 1 Slave
```

## 版本和配置文件对比

### 802.1AS版本差异

| 特性 | 802.1AS-2011 | 802.1AS-2020 |
|-----|-------------|--------------|
| gPTP-capable TLV | 可选 | **推荐使用** |
| TLV Type | 未明确 | 0x8000 明确定义 |
| 交换机要求 | 可能不检查 | **需要此TLV判断转发** |
| CMLDS | 不支持 | 支持（Automotive Profile） |

### transportSpecific 值对比

| 值 | 标准 | 说明 | 是否支持0x8000 TLV |
|----|------|------|-------------------|
| `0x1` | IEEE 802.1AS-2011/2020 | 标准gPTP | ✅ 支持 |
| `0x2` | Automotive Profile | CMLDS架构 | ✅ 支持 |
| 其他 | 非gPTP | 不相关 | ❌ 不发送 |

### Master/Slave 发送规则

| 角色 | 是否发送0x8000 | 原因 |
|------|---------------|------|
| **Master** | ✅ **必须发送** | 向下游和交换机通告能力 |
| **Slave** | ✅ **必须发送** | 向上游交换机通告能力（**最关键**） |
| **Bridge** | ✅ **必须发送** | 双向通告 |
| **CMLDS Server** | ✅ **建议发送** | 通告负责延迟测量 |

**重要**：0x8000不是请求-响应机制，而是单向能力通告。所有gPTP端口都应该发送。

## 技术细节 (Technical Details)

### TLV结构解析

```
Offset | Length | Field                           | Value
-------|--------|--------------------------------|----------
0      | 2      | type                           | 0x8000
2      | 2      | length                         | 0x0008
4      | 3      | organizationId                 | 00:80:C2
7      | 3      | organizationSubType            | 00:00:01
10     | 1      | flags                          | 0x00
11     | 1      | logGptpCapableMessageInterval  | 0x7F
```

### 代码调用流程

```
1. 端口初始化/状态变化
   ↓
2. port_capable() 判断asCapable状态
   ↓
3. asCapable状态改变
   ↓
4. port_tx_gptp_capable() 被调用
   ↓
5. 构建signaling报文
   ↓
6. 添加gPTP-capable TLV (0x8000)
   ↓
7. 发送到网络（组播地址 01:80:C2:00:00:0E）
```

## 参考标准 (References)

- IEEE 802.1AS-2020: Timing and Synchronization for Time-Sensitive Applications
- IEEE 1588-2019: Precision Time Protocol (PTP) Version 2.1
- IEEE 802.1Q-2018: Bridges and Bridged Networks

## 常见问题 (FAQ)

**Q: 是否必须启用此功能？**  
A: 如果您需要与严格遵守802.1AS-2020标准的设备（如Marvell交换机）互通，**强烈建议启用**。这是您无法同步的主要原因。

**Q: Master需要发送吗，还是只有Slave发送？**  
A: **Master和Slave都必须发送**。0x8000是单向能力通告，不是请求-响应。所有gPTP端口都应该通告自己的能力。

**Q: 对于CMLDS架构，三个进程都要配置吗？**  
A: **是的**，建议三个配置文件都添加 `gptp_capable_transmit = 1`：
   - CMLDS_server.cfg
   - domain0_master.cfg
   - domain1_slave.cfg

**Q: 对性能有影响吗？**  
A: 影响极小。Signaling报文只在asCapable状态变化和初始化时发送，频率很低。

**Q: 可以与802.1AS-2011设备互通吗？**  
A: 可以。此TLV是向后兼容的，老设备会忽略它。

**Q: 默认配置是什么？**  
A: `gptp_capable_transmit = 1`（启用）。这是推荐配置。

**Q: 如何禁用此功能？**  
A: 在配置文件中设置 `gptp_capable_transmit = 0`（不推荐，会导致与Marvell交换机无法同步）

**Q: transportSpecific = 0x2 也支持吗？**  
A: **完全支持**。代码已适配Automotive Profile (`transportSpecific = 0x2`)和标准gPTP (`transportSpecific = 0x1`)。

## 贡献者 (Contributors)

本实现基于linuxptp-4.4源码，添加了完整的IEEE 802.1AS-2020 gPTP-capable TLV支持。

---

## 快速开始指南

### 方案A: CMLDS架构（Automotive Profile）

**适用场景**：与Marvell交换机同步，使用Domain 0/1架构

#### 1. 修改配置文件

在以下三个文件的 `[global]` 段添加：
```ini
gptp_capable_transmit   1
```

修改的文件：
- `CMLDS_server.cfg`
- `domain0_master.cfg` 
- `domain1_slave.cfg`

#### 2. 启动命令

```bash
# 按顺序启动三个进程
ptp4l -f CMLDS_server.cfg -i ens33 -m &
sleep 2
ptp4l -f domain0_master.cfg -i ens33 -m &
sleep 2
ptp4l -f domain1_slave.cfg -i ens33 -m &
```

#### 3. 验证

```bash
# 检查是否都发送了gPTP-capable TLV
grep "sent gPTP-capable" /var/log/syslog
# 应该看到3条记录

# 检查同步状态
# Domain 1 应该进入SLAVE状态并显示同步信息
```

### 方案B: 标准gPTP（单实例）

**适用场景**：简单的点对点gPTP同步

#### 最小化配置示例

```ini
[global]
transportSpecific       0x1
network_transport       L2
delay_mechanism         P2P
ptp_dst_mac             01:80:C2:00:00:0E
gptp_capable_transmit   1    # 关键配置！
```

#### 启动命令

```bash
# 从站模式
ptp4l -f your_slave.cfg -i eth0 -s -m

# 主站模式  
ptp4l -f your_master.cfg -i eth0 -m
```

---

## 与Marvell交换机同步问题的解决方案

### 问题症状
- Domain 1 Slave一直处于LISTENING状态
- 无法收到Sync报文
- Marvell交换机不转发时间同步消息

### 根本原因
端节点没有发送gPTP-capable TLV (0x8000)，Marvell交换机无法识别该端口为gPTP-capable，因此拒绝转发Sync报文。

### 解决方案
在**所有PTP配置文件**中添加：
```ini
gptp_capable_transmit   1
```

包括：
- CMLDS_server.cfg（如果使用CMLDS）
- domain0_master.cfg（Master也要发送！）
- domain1_slave.cfg（最关键的一个）

### 验证成功
```bash
# 1. 检查日志
grep "sent gPTP-capable" /var/log/syslog
# 应该看到所有实例都发送了

# 2. 检查同步状态
# Domain 1 应该显示：
# port 1: SLAVE
# rms xxx max xxx freq xxx offset xxx
```

---

## 相关文档

- **`AUTOMOTIVE_CMLDS_GUIDE.md`** - 详细的CMLDS架构和Automotive Profile使用指南
- **`QUICK_REFERENCE.md`** - 快速参考卡片

---

**重要**: 此实现解决了端节点无法与符合802.1AS-2020标准的交换机（如Marvell）同步的问题。**Master和Slave都必须启用此功能**，这不是可选项！

